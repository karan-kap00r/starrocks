// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "storage/vectorized/rowset_merger.h"

#include <memory>

#include "gutil/stl_util.h"
#include "storage/primary_key_encoder.h"
#include "storage/rowset/beta_rowset_writer.h"
#include "storage/rowset/rowset_writer.h"
#include "storage/rowset/vectorized/rowset_options.h"
#include "storage/tablet.h"
#include "storage/vectorized/chunk_helper.h"
#include "storage/vectorized/empty_iterator.h"
#include "storage/vectorized/merge_iterator.h"
#include "storage/vectorized/union_iterator.h"
#include "util/pretty_printer.h"
#include "util/starrocks_metrics.h"

namespace starrocks::vectorized {

class RowsetMerger {
public:
    RowsetMerger() = default;

    virtual ~RowsetMerger() = default;

    virtual Status do_merge(Tablet& tablet, int64_t version, const Schema& schema,
                            const vector<RowsetSharedPtr>& rowsets, RowsetWriter* writer, const MergeConfig& cfg) = 0;
};

template <class T>
struct MergeEntry {
    const T* pk_cur = nullptr;
    const T* pk_last = nullptr;
    const T* pk_start = nullptr;
    uint32_t cur_segment_idx = 0;
    uint32_t rowset_seg_id = 0;
    ColumnPtr chunk_pk_column;
    ChunkPtr chunk;
    vector<ChunkIteratorPtr> segment_itrs;
    std::unique_ptr<RowsetReleaseGuard> rowset_release_guard;
    // set |encode_schema| if require encode chunk pk columns
    const vectorized::Schema* encode_schema = nullptr;
    uint16_t order;

    MergeEntry() = default;
    ~MergeEntry() { close(); }

    string debug_string() {
        string ret;
        StringAppendF(&ret, "%u: %ld/%ld %u/%zu : ", rowset_seg_id, offset(pk_cur), offset(pk_last) + 1,
                      cur_segment_idx, segment_itrs.size());
        for (const T* cur = pk_cur; cur <= pk_last; cur++) {
            if constexpr (std::is_arithmetic_v<T>) {
                StringAppendF(&ret, " %ld", (long int)*cur);
            } else {
                // must be Slice
                StringAppendF(&ret, " %s", cur->to_string().c_str());
            }
        }
        return ret;
    }

    ptrdiff_t offset(const T* p) const { return p - pk_start; }

    bool at_start() const { return pk_cur == pk_start; }

    void close() {
        chunk_pk_column.reset();
        chunk.reset();
        for (auto& itr : segment_itrs) {
            if (itr) {
                itr->close();
                itr.reset();
            }
        }
        STLClearObject(&segment_itrs);
        rowset_release_guard.reset();
    }

    Status init() {
        while (cur_segment_idx < segment_itrs.size() && !segment_itrs[cur_segment_idx]) {
            cur_segment_idx++;
        }
        return next();
    }

    Status next() {
        if (cur_segment_idx >= segment_itrs.size()) {
            return Status::EndOfFile("End of merge entry iterator");
        }
        DCHECK(pk_cur == nullptr || pk_cur > pk_last);
        chunk->reset();
        while (true) {
            auto& itr = segment_itrs[cur_segment_idx];
            auto st = itr->get_next(chunk.get());
            if (st.ok()) {
                // 1. setup chunk_pk_column
                if (encode_schema != nullptr) {
                    // need to encode
                    chunk_pk_column->reset_column();
                    PrimaryKeyEncoder::encode(*encode_schema, *chunk, 0, chunk->num_rows(), chunk_pk_column.get());
                } else {
                    // just use chunk's first column
                    chunk_pk_column = chunk->get_column_by_index(0);
                }
                DCHECK(chunk_pk_column->size() > 0);
                DCHECK(chunk_pk_column->size() == chunk->num_rows());
                // 2. setup pk cursor
                pk_start = reinterpret_cast<const T*>(chunk_pk_column->raw_data());
                pk_cur = pk_start;
                pk_last = pk_start + chunk_pk_column->size() - 1;
                return Status::OK();
            } else if (st.is_end_of_file()) {
                itr->close();
                itr.reset();
                while (true) {
                    cur_segment_idx++;
                    rowset_seg_id++;
                    if (cur_segment_idx == segment_itrs.size()) {
                        return Status::EndOfFile("End of merge entry iterator");
                    }
                    if (segment_itrs[cur_segment_idx]) {
                        break;
                    }
                }
                continue;
            } else {
                // error
                return st;
            }
        }
    }
};

template <class T>
struct MergeEntryCmp {
    bool operator()(const MergeEntry<T>* lhs, const MergeEntry<T>* rhs) const {
        return *(lhs->pk_cur) > *(rhs->pk_cur);
    }
};

// heap based rowset merger used for updatable tablet's compaction
template <class T>
class RowsetMergerImpl : public RowsetMerger {
public:
    RowsetMergerImpl() = default;

    ~RowsetMergerImpl() override = default;

    Status _fill_heap(MergeEntry<T>* entry) {
        auto st = entry->next();
        if (st.ok()) {
            _heap.push(entry);
        } else if (!st.is_end_of_file()) {
            return st;
        }
        return Status::OK();
    }

    Status get_next(Chunk* chunk, vector<uint32_t>* rssids, vector<RowSourceMask>* source_masks) {
        size_t nrow = 0;
        while (!_heap.empty() && nrow < _chunk_size) {
            MergeEntry<T>& top = *_heap.top();
            //LOG(INFO) << "m" << _heap.size() << " top: " << top.debug_string();
            DCHECK_LE(top.pk_cur, top.pk_last);
            _heap.pop();
            if (_heap.empty() || *(top.pk_last) < *(_heap.top()->pk_cur)) {
                if (nrow == 0 && top.at_start()) {
                    chunk->swap_chunk(*top.chunk);
                    rssids->insert(rssids->end(), chunk->num_rows(), top.rowset_seg_id);
                    if (source_masks) {
                        source_masks->insert(source_masks->end(), chunk->num_rows(), RowSourceMask{top.order, false});
                    }
                    top.pk_cur = top.pk_last + 1;
                    return _fill_heap(&top);
                } else {
                    // TODO(cbl): make dest chunk size larger, so we can copy all rows at once
                    int nappend = std::min((int)(top.pk_last - top.pk_cur + 1), (int)(_chunk_size - nrow));
                    auto start_offset = top.offset(top.pk_cur);
                    chunk->append(*top.chunk, start_offset, nappend);
                    rssids->insert(rssids->end(), nappend, top.rowset_seg_id);
                    if (source_masks) {
                        source_masks->insert(source_masks->end(), nappend, RowSourceMask{top.order, false});
                    }
                    top.pk_cur += nappend;
                    if (top.pk_cur > top.pk_last) {
                        //LOG(INFO) << "  append all " << nappend << "  get_next batch";
                        return _fill_heap(&top);
                    } else {
                        //LOG(INFO) << "  append all " << nappend << "  ";
                        _heap.push(&top);
                    }
                    return Status::OK();
                }
            }

            auto start = top.pk_cur;
            while (true) {
                nrow++;
                top.pk_cur++;
                rssids->push_back(top.rowset_seg_id);
                if (source_masks) {
                    source_masks->emplace_back(RowSourceMask{top.order, false});
                }
                if (top.pk_cur > top.pk_last) {
                    auto start_offset = top.offset(start);
                    auto end_offset = top.offset(top.pk_cur);
                    chunk->append(*top.chunk, start_offset, end_offset - start_offset);
                    DCHECK(chunk->num_rows() == nrow);
                    //LOG(INFO) << "  append " << end_offset - start_offset << "  get_next batch";
                    return _fill_heap(&top);
                }
                if (nrow >= _chunk_size || !(*(top.pk_cur) < *(_heap.top()->pk_cur))) {
                    auto start_offset = top.offset(start);
                    auto end_offset = top.offset(top.pk_cur);
                    chunk->append(*top.chunk, start_offset, end_offset - start_offset);
                    DCHECK(chunk->num_rows() == nrow);
                    //if (nrow >= _chunk_size) {
                    //	LOG(INFO) << "  append " << end_offset - start_offset << "  chunk full";
                    //} else {
                    //	LOG(INFO) << "  append " << end_offset - start_offset
                    //			  << "  other entry is smaller";
                    //}
                    _heap.push(&top);
                    if (nrow >= _chunk_size) {
                        return Status::OK();
                    }
                    break;
                }
            }
        }
        return Status::EndOfFile("merge end");
    }

    Status do_merge(Tablet& tablet, int64_t version, const Schema& schema, const vector<RowsetSharedPtr>& rowsets,
                    RowsetWriter* writer, const MergeConfig& cfg) override {
        _chunk_size = cfg.chunk_size;

        size_t total_input_size = 0;
        size_t total_rows = 0;
        size_t total_chunk = 0;
        OlapReaderStatistics stats;
        vector<vector<uint32_t>> column_groups;
        MonotonicStopWatch timer;
        timer.start();
        if (cfg.algorithm == VERTICAL_COMPACTION) {
            int64_t max_columns_per_group = config::vertical_compaction_max_columns_per_group;
            CompactionUtils::split_column_into_groups(tablet.num_columns(), tablet.num_key_columns(),
                                                      max_columns_per_group, &column_groups);
            RETURN_IF_ERROR(_do_merge_vertically(tablet, version, rowsets, writer, cfg, column_groups,
                                                 &total_input_size, &total_rows, &total_chunk, &stats));
        } else {
            RETURN_IF_ERROR(_do_merge_horizontally(tablet, version, schema, rowsets, writer, cfg, &total_input_size,
                                                   &total_rows, &total_chunk, &stats));
        }
        timer.stop();

        StarRocksMetrics::instance()->update_compaction_deltas_total.increment(rowsets.size());
        StarRocksMetrics::instance()->update_compaction_bytes_total.increment(total_input_size);
        StarRocksMetrics::instance()->update_compaction_outputs_total.increment(1);
        StarRocksMetrics::instance()->update_compaction_outputs_bytes_total.increment(writer->total_data_size());
        LOG(INFO) << "compaction merge finished. tablet=" << tablet.tablet_id() << " #key=" << schema.num_key_fields()
                  << " algorithm=" << CompactionUtils::compaction_algorithm_to_string(cfg.algorithm)
                  << " column_group_size=" << column_groups.size() << " input("
                  << "entry=" << _entries.size() << " rows=" << stats.raw_rows_read
                  << " del=" << stats.rows_del_vec_filtered << " actual=" << stats.raw_rows_read
                  << " bytes=" << PrettyPrinter::print(total_input_size, TUnit::BYTES) << ") output(rows=" << total_rows
                  << " chunk=" << total_chunk
                  << " bytes=" << PrettyPrinter::print(writer->total_data_size(), TUnit::BYTES)
                  << ") duration: " << timer.elapsed_time() / 1000000 << "ms";
        return Status::OK();
    }

private:
    Status _do_merge_horizontally(Tablet& tablet, int64_t version, const Schema& schema,
                                  const vector<RowsetSharedPtr>& rowsets, RowsetWriter* writer, const MergeConfig& cfg,
                                  size_t* total_input_size, size_t* total_rows, size_t* total_chunk,
                                  OlapReaderStatistics* stats, RowSourceMaskBuffer* mask_buffer = nullptr) {
        std::unique_ptr<vectorized::Column> pk_column;
        if (schema.num_key_fields() > 1) {
            if (!PrimaryKeyEncoder::create_column(schema, &pk_column).ok()) {
                LOG(FATAL) << "create column for primary key encoder failed";
            }
        }

        uint16_t order = 0;
        for (const auto& i : rowsets) {
            *total_input_size += i->data_disk_size();
            _entries.emplace_back(new MergeEntry<T>());
            MergeEntry<T>& entry = *_entries.back();
            entry.rowset_release_guard = std::make_unique<RowsetReleaseGuard>(i);
            auto rowset = i.get();
            auto beta_rowset = down_cast<BetaRowset*>(rowset);
            auto res = beta_rowset->get_segment_iterators2(schema, tablet.data_dir()->get_meta(), version, stats);
            if (!res.ok()) {
                return res.status();
            }
            entry.rowset_seg_id = rowset->rowset_meta()->get_rowset_seg_id();
            entry.segment_itrs.swap(res.value());
            entry.chunk = ChunkHelper::new_chunk(schema, _chunk_size);
            if (pk_column) {
                entry.encode_schema = &schema;
                entry.chunk_pk_column = pk_column->clone_shared();
                entry.chunk_pk_column->reserve(_chunk_size);
            }
            entry.order = order++;
            auto st = entry.init();
            if (!st.ok()) {
                if (st.is_end_of_file()) {
                    entry.close();
                } else {
                    return st;
                }
            } else {
                _heap.push(&entry);
            }
        }

        auto char_field_indexes = ChunkHelper::get_char_field_indexes(schema);

        vector<uint32_t> column_indexes;
        std::unique_ptr<vector<RowSourceMask>> source_masks;
        if (mask_buffer) {
            source_masks = std::make_unique<vector<RowSourceMask>>();
            column_indexes.reserve(schema.num_key_fields());
            for (uint32_t i = 0; i < schema.num_key_fields(); ++i) {
                column_indexes.emplace_back(i);
            }
        }

        auto chunk = ChunkHelper::new_chunk(schema, _chunk_size);
        vector<uint32_t> rssids;
        rssids.reserve(_chunk_size);
        while (true) {
            chunk->reset();
            rssids.clear();
            Status status = get_next(chunk.get(), &rssids, source_masks.get());
            if (!status.ok()) {
                if (status.is_end_of_file()) {
                    break;
                } else {
                    LOG(WARNING) << "reader get next error. tablet=" << tablet.tablet_id()
                                 << ", err=" << status.to_string();
                    return Status::InternalError("reader get_next error.");
                }
            }

            ChunkHelper::padding_char_columns(char_field_indexes, schema, tablet.tablet_schema(), chunk.get());

            *total_rows += chunk->num_rows();
            (*total_chunk)++;

            if (mask_buffer) {
                if (auto st = writer->add_columns_with_rssid(*chunk, column_indexes, rssids); !st.ok()) {
                    LOG(WARNING) << "writer add_columns_with_rssid error, tablet=" << tablet.tablet_id()
                                 << ", err=" << st;
                    return st;
                }

                if (!source_masks->empty()) {
                    RETURN_IF_ERROR(mask_buffer->write(*source_masks));
                    source_masks->clear();
                }
            } else {
                if (auto st = writer->add_chunk_with_rssid(*chunk, rssids); !st.ok()) {
                    LOG(WARNING) << "writer add_chunk_with_rssid error, tablet=" << tablet.tablet_id()
                                 << ", err=" << st;
                    return st;
                }
            }
        }

        if (mask_buffer) {
            if (auto st = writer->flush_columns(); !st.ok()) {
                LOG(WARNING) << "failed to flush columns when merging rowsets of tablet " << tablet.tablet_id()
                             << ", err=" << st;
                return st;
            }

            RETURN_IF_ERROR(mask_buffer->flush());
        } else {
            if (auto st = writer->flush(); !st.ok()) {
                LOG(WARNING) << "failed to flush rowset when merging rowsets of tablet " << tablet.tablet_id()
                             << ", err=" << st;
                return st;
            }
        }

        if (stats->raw_rows_read != *total_rows) {
            string msg = Substitute("update compaction rows read($0) != rows written($1)", stats->raw_rows_read,
                                    *total_rows);
            LOG(WARNING) << msg;
            return Status::InternalError(msg);
        }

        return Status::OK();
    }

    Status _do_merge_vertically(Tablet& tablet, int64_t version, const vector<RowsetSharedPtr>& rowsets,
                                RowsetWriter* writer, const MergeConfig& cfg,
                                const vector<vector<uint32_t>>& column_groups, size_t* total_input_size,
                                size_t* total_rows, size_t* total_chunk, OlapReaderStatistics* stats) {
        DCHECK_GT(column_groups.size(), 1);
        // merge key columns
        auto mask_buffer = std::make_unique<RowSourceMaskBuffer>(tablet.tablet_id(), tablet.data_dir()->path());
        {
            Schema schema = ChunkHelper::convert_schema_to_format_v2(tablet.tablet_schema(), column_groups[0]);
            RETURN_IF_ERROR(_do_merge_horizontally(tablet, version, schema, rowsets, writer, cfg, total_input_size,
                                                   total_rows, total_chunk, stats, mask_buffer.get()));
        }

        // merge non key columns
        auto source_masks = std::make_unique<vector<RowSourceMask>>();
        for (size_t i = 1; i < column_groups.size(); ++i) {
            // read mask buffer from the beginning
            mask_buffer->flip_to_read();

            _entries.clear();
            _entries.reserve(rowsets.size());
            vector<vectorized::ChunkIteratorPtr> iterators;
            iterators.reserve(rowsets.size());
            OlapReaderStatistics non_key_stats;
            Schema schema = ChunkHelper::convert_schema_to_format_v2(tablet.tablet_schema(), column_groups[i]);
            for (const auto& rowset : rowsets) {
                _entries.emplace_back(new MergeEntry<T>());
                MergeEntry<T>& entry = *_entries.back();
                entry.rowset_release_guard = std::make_unique<RowsetReleaseGuard>(rowset);
                auto beta_rowset = down_cast<BetaRowset*>(rowset.get());
                auto res = beta_rowset->get_segment_iterators2(schema, tablet.data_dir()->get_meta(), version,
                                                               &non_key_stats);
                if (!res.ok()) {
                    return res.status();
                }
                vector<vectorized::ChunkIteratorPtr> segment_iters;
                for (const auto& segment_iter : res.value()) {
                    if (segment_iter) {
                        segment_iters.emplace_back(std::move(segment_iter));
                    }
                }
                if (segment_iters.empty()) {
                    iterators.emplace_back(new_empty_iterator(schema, _chunk_size));
                } else {
                    iterators.emplace_back(new_union_iterator(std::move(segment_iters)));
                }
            }

            CHECK_EQ(rowsets.size(), iterators.size());
            std::shared_ptr<ChunkIterator> iter = new_mask_merge_iterator(iterators, mask_buffer.get());
            iter->init_encoded_schema(EMPTY_GLOBAL_DICTMAPS);

            auto chunk = ChunkHelper::new_chunk(schema, _chunk_size);
            auto char_field_indexes = ChunkHelper::get_char_field_indexes(schema);

            while (true) {
                chunk->reset();
                Status status = iter->get_next(chunk.get(), source_masks.get());
                if (!status.ok()) {
                    if (status.is_end_of_file()) {
                        break;
                    } else {
                        LOG(WARNING) << "reader get next error. tablet=" << tablet.tablet_id()
                                     << ", err=" << status.to_string();
                        return Status::InternalError("reader get_next error.");
                    }
                }

                ChunkHelper::padding_char_columns(char_field_indexes, schema, tablet.tablet_schema(), chunk.get());

                if (auto st = writer->add_columns(*chunk, column_groups[i], false); !st.ok()) {
                    LOG(WARNING) << "writer add_columns error, tablet=" << tablet.tablet_id() << ", err=" << st;
                    return st;
                }

                if (!source_masks->empty()) {
                    source_masks->clear();
                }
            }

            if (auto st = writer->flush_columns(); !st.ok()) {
                LOG(WARNING) << "failed to flush columns when merging rowsets of tablet " << tablet.tablet_id()
                             << ", err=" << st;
                return st;
            }

            if (non_key_stats.raw_rows_read != *total_rows) {
                string msg = Substitute("update compaction rows read($0) != rows written($1) when merging non keys",
                                        non_key_stats.raw_rows_read, *total_rows);
                LOG(WARNING) << msg;
                return Status::InternalError(msg);
            }
        }

        if (auto st = writer->final_flush(); !st.ok()) {
            LOG(WARNING) << "failed to final flush rowset when merging rowsets of tablet " << tablet.tablet_id()
                         << ", err=" << st;
            return st;
        }

        return Status::OK();
    }

    size_t _chunk_size = 0;
    std::vector<std::unique_ptr<MergeEntry<T>>> _entries;
    using Heap = std::priority_queue<MergeEntry<T>*, std::vector<MergeEntry<T>*>, MergeEntryCmp<T>>;
    Heap _heap;
};

Status compaction_merge_rowsets(Tablet& tablet, int64_t version, const vector<RowsetSharedPtr>& rowsets,
                                RowsetWriter* writer, const MergeConfig& cfg) {
    Schema schema = ChunkHelper::convert_schema(tablet.tablet_schema());
    std::unique_ptr<RowsetMerger> merger;
    auto key_type = PrimaryKeyEncoder::encoded_primary_key_type(schema);
    switch (key_type) {
    case OLAP_FIELD_TYPE_BOOL:
        merger = std::make_unique<RowsetMergerImpl<uint8_t>>();
        break;
    case OLAP_FIELD_TYPE_TINYINT:
        merger = std::make_unique<RowsetMergerImpl<int8_t>>();
        break;
    case OLAP_FIELD_TYPE_SMALLINT:
        merger = std::make_unique<RowsetMergerImpl<int16_t>>();
        break;
    case OLAP_FIELD_TYPE_INT:
        merger = std::make_unique<RowsetMergerImpl<int32_t>>();
        break;
    case OLAP_FIELD_TYPE_BIGINT:
        merger = std::make_unique<RowsetMergerImpl<int64_t>>();
        break;
    case OLAP_FIELD_TYPE_LARGEINT:
        merger = std::make_unique<RowsetMergerImpl<int128_t>>();
        break;
    case OLAP_FIELD_TYPE_VARCHAR:
        merger = std::make_unique<RowsetMergerImpl<Slice>>();
        break;
    case OLAP_FIELD_TYPE_DATE_V2:
        merger = std::make_unique<RowsetMergerImpl<int32_t>>();
        break;
    case OLAP_FIELD_TYPE_TIMESTAMP:
        merger = std::make_unique<RowsetMergerImpl<int64_t>>();
        break;
    default:
        return Status::NotSupported(StringPrintf("primary key type not support: %s", field_type_to_string(key_type)));
    }
    return merger->do_merge(tablet, version, schema, rowsets, writer, cfg);
}

} // namespace starrocks::vectorized
