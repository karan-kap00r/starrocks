// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "exec/vectorized/join_hash_map.h"

#include <column/chunk.h>
#include <gen_cpp/PlanNodes_types.h>
#include <runtime/descriptors.h>

#include "exec/vectorized/hash_join_node.h"
#include "serde/column_array_serde.h"
#include "simd/simd.h"

namespace starrocks::vectorized {

Status SerializedJoinBuildFunc::prepare(RuntimeState* state, JoinHashTableItems* table_items,
                                        HashTableProbeState* probe_state) {
    table_items->build_slice.resize(table_items->row_count + 1);
    probe_state->buckets.resize(state->chunk_size());
    probe_state->is_nulls.resize(state->chunk_size());
    return Status::OK();
}

Status SerializedJoinBuildFunc::construct_hash_table(RuntimeState* state, JoinHashTableItems* table_items,
                                                     HashTableProbeState* probe_state) {
    uint32_t row_count = table_items->row_count;

    // prepare columns
    Columns data_columns;
    NullColumns null_columns;

    for (size_t i = 0; i < table_items->key_columns.size(); i++) {
        if (table_items->join_keys[i].is_null_safe_equal) {
            data_columns.emplace_back(table_items->key_columns[i]);
        } else if (table_items->key_columns[i]->is_nullable()) {
            auto* nullable_column = ColumnHelper::as_raw_column<NullableColumn>(table_items->key_columns[i]);
            data_columns.emplace_back(nullable_column->data_column());
            if (table_items->key_columns[i]->has_null()) {
                null_columns.emplace_back(nullable_column->null_column());
            }
        } else {
            data_columns.emplace_back(table_items->key_columns[i]);
        }
    }

    // calc serialize size
    size_t serialize_size = 0;
    for (const auto& data_column : data_columns) {
        serialize_size += serde::ColumnArraySerde::max_serialized_size(*data_column);
    }
    uint8_t* ptr = table_items->build_pool->allocate(serialize_size);
    RETURN_IF_UNLIKELY_NULL(ptr, Status::MemoryAllocFailed("alloc mem for hash join build failed"));

    // serialize and build hash table
    uint32_t quo = row_count / state->chunk_size();
    uint32_t rem = row_count % state->chunk_size();

    if (!null_columns.empty()) {
        for (size_t i = 0; i < quo; i++) {
            _build_nullable_columns(table_items, probe_state, data_columns, null_columns, 1 + state->chunk_size() * i,
                                    state->chunk_size(), &ptr);
        }
        _build_nullable_columns(table_items, probe_state, data_columns, null_columns, 1 + state->chunk_size() * quo,
                                rem, &ptr);
    } else {
        for (size_t i = 0; i < quo; i++) {
            _build_columns(table_items, probe_state, data_columns, 1 + state->chunk_size() * i, state->chunk_size(),
                           &ptr);
        }
        _build_columns(table_items, probe_state, data_columns, 1 + state->chunk_size() * quo, rem, &ptr);
    }

    return Status::OK();
}

void SerializedJoinBuildFunc::_build_columns(JoinHashTableItems* table_items, HashTableProbeState* probe_state,
                                             const Columns& data_columns, uint32_t start, uint32_t count,
                                             uint8_t** ptr) {
    for (size_t i = 0; i < count; i++) {
        table_items->build_slice[start + i] = JoinHashMapHelper::get_hash_key(data_columns, start + i, *ptr);
        probe_state->buckets[i] = JoinHashMapHelper::calc_bucket_num<Slice>(table_items->build_slice[start + i],
                                                                            table_items->bucket_size);
        *ptr += table_items->build_slice[start + i].size;
    }

    for (size_t i = 0; i < count; i++) {
        table_items->next[start + i] = table_items->first[probe_state->buckets[i]];
        table_items->first[probe_state->buckets[i]] = start + i;
    }
}

void SerializedJoinBuildFunc::_build_nullable_columns(JoinHashTableItems* table_items, HashTableProbeState* probe_state,
                                                      const Columns& data_columns, const NullColumns& null_columns,
                                                      uint32_t start, uint32_t count, uint8_t** ptr) {
    for (uint32_t i = 0; i < count; i++) {
        probe_state->is_nulls[i] = null_columns[0]->get_data()[start + i];
    }
    for (uint32_t i = 1; i < null_columns.size(); i++) {
        for (uint32_t j = 0; j < count; j++) {
            probe_state->is_nulls[j] |= null_columns[i]->get_data()[start + j];
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (probe_state->is_nulls[i] == 0) {
            table_items->build_slice[start + i] = JoinHashMapHelper::get_hash_key(data_columns, start + i, *ptr);
            probe_state->buckets[i] = JoinHashMapHelper::calc_bucket_num<Slice>(table_items->build_slice[start + i],
                                                                                table_items->bucket_size);
            *ptr += table_items->build_slice[start + i].size;
        }
    }

    for (size_t i = 0; i < count; i++) {
        if (probe_state->is_nulls[i] == 0) {
            table_items->next[start + i] = table_items->first[probe_state->buckets[i]];
            table_items->first[probe_state->buckets[i]] = start + i;
        }
    }
}

Status SerializedJoinProbeFunc::lookup_init(const JoinHashTableItems& table_items, HashTableProbeState* probe_state) {
    // prepare columns
    Columns data_columns;
    NullColumns null_columns;

    for (size_t i = 0; i < probe_state->key_columns->size(); i++) {
        if (table_items.join_keys[i].is_null_safe_equal) {
            data_columns.emplace_back((*probe_state->key_columns)[i]);
        } else if ((*probe_state->key_columns)[i]->is_nullable()) {
            auto* nullable_column = ColumnHelper::as_raw_column<NullableColumn>((*probe_state->key_columns)[i]);
            data_columns.emplace_back(nullable_column->data_column());
            if ((*probe_state->key_columns)[i]->has_null()) {
                null_columns.emplace_back(nullable_column->null_column());
            }
        } else {
            data_columns.emplace_back((*probe_state->key_columns)[i]);
        }
    }

    // allocate memory for serialize key columns
    size_t serialize_size = 0;
    for (const auto& data_column : data_columns) {
        serialize_size += serde::ColumnArraySerde::max_serialized_size(*data_column);
    }
    uint8_t* ptr = probe_state->probe_pool->allocate(serialize_size);
    RETURN_IF_UNLIKELY_NULL(ptr, Status::MemoryAllocFailed("alloc mem for hash join probe failed"));

    // serialize and init search
    if (!null_columns.empty()) {
        _probe_nullable_column(table_items, probe_state, data_columns, null_columns, ptr);
    } else {
        _probe_column(table_items, probe_state, data_columns, ptr);
    }
    return Status::OK();
}

void SerializedJoinProbeFunc::_probe_column(const JoinHashTableItems& table_items, HashTableProbeState* probe_state,
                                            const Columns& data_columns, uint8_t* ptr) {
    uint32_t row_count = probe_state->probe_row_count;

    for (uint32_t i = 0; i < row_count; i++) {
        probe_state->probe_slice[i] = JoinHashMapHelper::get_hash_key(data_columns, i, ptr);
        probe_state->buckets[i] =
                JoinHashMapHelper::calc_bucket_num<Slice>(probe_state->probe_slice[i], table_items.bucket_size);
        ptr += probe_state->probe_slice[i].size;
    }

    for (uint32_t i = 0; i < row_count; i++) {
        probe_state->next[i] = table_items.first[probe_state->buckets[i]];
    }
}

void SerializedJoinProbeFunc::_probe_nullable_column(const JoinHashTableItems& table_items,
                                                     HashTableProbeState* probe_state, const Columns& data_columns,
                                                     const NullColumns& null_columns, uint8_t* ptr) {
    uint32_t row_count = probe_state->probe_row_count;

    for (uint32_t i = 0; i < row_count; i++) {
        probe_state->is_nulls[i] = null_columns[0]->get_data()[i];
    }
    for (uint32_t i = 1; i < null_columns.size(); i++) {
        for (uint32_t j = 0; j < row_count; j++) {
            probe_state->is_nulls[j] |= null_columns[i]->get_data()[j];
        }
    }

    for (uint32_t i = 0; i < row_count; i++) {
        if (probe_state->is_nulls[i] == 0) {
            probe_state->probe_slice[i] = JoinHashMapHelper::get_hash_key(data_columns, i, ptr);
            ptr += probe_state->probe_slice[i].size;
        }
    }

    for (uint32_t i = 0; i < row_count; i++) {
        if (probe_state->is_nulls[i] == 0) {
            probe_state->buckets[i] =
                    JoinHashMapHelper::calc_bucket_num<Slice>(probe_state->probe_slice[i], table_items.bucket_size);
            probe_state->next[i] = table_items.first[probe_state->buckets[i]];
        } else {
            probe_state->next[i] = 0;
        }
    }
}

JoinHashTable JoinHashTable::clone_readable_table() {
    JoinHashTable ht;

    ht._hash_map_type = this->_hash_map_type;
    ht._need_create_tuple_columns = this->_need_create_tuple_columns;

    ht._table_items = this->_table_items;
    // Clone a new probe state.
    ht._probe_state = std::make_unique<HashTableProbeState>(*this->_probe_state);

    switch (ht._hash_map_type) {
    case JoinHashMapType::empty:
        break;
#define M(NAME)                                                                                         \
    case JoinHashMapType::NAME:                                                                         \
        ht._##NAME = std::make_unique<typename decltype(_##NAME)::element_type>(ht._table_items.get(),  \
                                                                                ht._probe_state.get()); \
        break;
        APPLY_FOR_JOIN_VARIANTS(M)
#undef M
    default:
        DCHECK(false) << "Unsupported hash_map_type";
    }

    return ht;
}

void JoinHashTable::close() {
    _table_items.reset();
    _probe_state.reset();
}

void JoinHashTable::create(const HashTableParam& param) {
    _need_create_tuple_columns = param.need_create_tuple_columns;
    _table_items = std::make_shared<JoinHashTableItems>();
    _probe_state = std::make_unique<HashTableProbeState>();

    _table_items->row_count = 0;
    _table_items->bucket_size = 0;
    _table_items->need_create_tuple_columns = _need_create_tuple_columns;
    _table_items->build_chunk = std::make_shared<Chunk>();
    _table_items->build_pool = std::make_unique<MemPool>();
    _table_items->with_other_conjunct = param.with_other_conjunct;
    _table_items->join_type = param.join_type;
    _table_items->row_desc = param.row_desc;
    if (_table_items->join_type == TJoinOp::RIGHT_SEMI_JOIN || _table_items->join_type == TJoinOp::RIGHT_ANTI_JOIN ||
        _table_items->join_type == TJoinOp::RIGHT_OUTER_JOIN) {
        _table_items->left_to_nullable = true;
    } else if (_table_items->join_type == TJoinOp::LEFT_SEMI_JOIN ||
               _table_items->join_type == TJoinOp::LEFT_ANTI_JOIN ||
               _table_items->join_type == TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN ||
               _table_items->join_type == TJoinOp::LEFT_OUTER_JOIN) {
        _table_items->right_to_nullable = true;
    } else if (_table_items->join_type == TJoinOp::FULL_OUTER_JOIN) {
        _table_items->left_to_nullable = true;
        _table_items->right_to_nullable = true;
    }
    _table_items->search_ht_timer = param.search_ht_timer;
    _table_items->output_build_column_timer = param.output_build_column_timer;
    _table_items->output_probe_column_timer = param.output_probe_column_timer;
    _table_items->output_tuple_column_timer = param.output_tuple_column_timer;
    _table_items->join_keys = param.join_keys;

    _probe_state->probe_pool = std::make_unique<MemPool>();

    const auto& probe_desc = *param.probe_row_desc;
    for (const auto& tuple_desc : probe_desc.tuple_descriptors()) {
        for (const auto& slot : tuple_desc->slots()) {
            HashTableSlotDescriptor hash_table_slot;
            hash_table_slot.slot = slot;
            if (param.output_slots.empty() ||
                std::find(param.output_slots.begin(), param.output_slots.end(), slot->id()) !=
                        param.output_slots.end() ||
                std::find(param.predicate_slots.begin(), param.predicate_slots.end(), slot->id()) !=
                        param.predicate_slots.end()) {
                hash_table_slot.need_output = true;
            } else {
                hash_table_slot.need_output = false;
            }

            _table_items->probe_slots.emplace_back(hash_table_slot);
            _table_items->probe_column_count++;
        }
        if (_table_items->row_desc->get_tuple_idx(tuple_desc->id()) != RowDescriptor::INVALID_IDX) {
            _table_items->output_probe_tuple_ids.emplace_back(tuple_desc->id());
        }
    }

    const auto& build_desc = *param.build_row_desc;
    for (const auto& tuple_desc : build_desc.tuple_descriptors()) {
        for (const auto& slot : tuple_desc->slots()) {
            HashTableSlotDescriptor hash_table_slot;
            hash_table_slot.slot = slot;
            if (param.output_slots.empty() ||
                std::find(param.output_slots.begin(), param.output_slots.end(), slot->id()) !=
                        param.output_slots.end() ||
                std::find(param.predicate_slots.begin(), param.predicate_slots.end(), slot->id()) !=
                        param.predicate_slots.end()) {
                hash_table_slot.need_output = true;
            } else {
                hash_table_slot.need_output = false;
            }

            _table_items->build_slots.emplace_back(hash_table_slot);
            ColumnPtr column = ColumnHelper::create_column(slot->type(), slot->is_nullable());
            if (slot->is_nullable()) {
                auto* nullable_column = ColumnHelper::as_raw_column<NullableColumn>(column);
                nullable_column->append_default_not_null_value();
            } else {
                column->append_default();
            }
            _table_items->build_chunk->append_column(std::move(column), slot->id());
            _table_items->build_column_count++;
        }
        if (_table_items->row_desc->get_tuple_idx(tuple_desc->id()) != RowDescriptor::INVALID_IDX) {
            _table_items->output_build_tuple_ids.emplace_back(tuple_desc->id());
        }
    }
}

Status JoinHashTable::build(RuntimeState* state) {
    _hash_map_type = _choose_join_hash_map();
    _table_items->bucket_size = JoinHashMapHelper::calc_bucket_size(_table_items->row_count + 1);
    _table_items->first.resize(_table_items->bucket_size, 0);
    _table_items->next.resize(_table_items->row_count + 1, 0);
    if (_table_items->join_type == TJoinOp::RIGHT_OUTER_JOIN || _table_items->join_type == TJoinOp::FULL_OUTER_JOIN ||
        _table_items->join_type == TJoinOp::RIGHT_SEMI_JOIN || _table_items->join_type == TJoinOp::RIGHT_ANTI_JOIN) {
        _probe_state->build_match_index.resize(_table_items->row_count + 1, 0);
        _probe_state->build_match_index[0] = 1;
    }

    JoinHashMapHelper::prepare_map_index(_probe_state.get(), state->chunk_size());

    switch (_hash_map_type) {
    case JoinHashMapType::empty:
        break;
#define M(NAME)                                                                                                       \
    case JoinHashMapType::NAME:                                                                                       \
        _##NAME = std::make_unique<typename decltype(_##NAME)::element_type>(_table_items.get(), _probe_state.get()); \
        RETURN_IF_ERROR(_##NAME->build(state));                                                                       \
        break;
        APPLY_FOR_JOIN_VARIANTS(M)
#undef M
    default:
        return Status::InternalError("not supported");
    }

    return Status::OK();
}

Status JoinHashTable::probe(RuntimeState* state, const Columns& key_columns, ChunkPtr* probe_chunk, ChunkPtr* chunk,
                            bool* eos) {
    switch (_hash_map_type) {
    case JoinHashMapType::empty:
        break;
#define M(NAME)                                                                       \
    case JoinHashMapType::NAME:                                                       \
        RETURN_IF_ERROR(_##NAME->probe(state, key_columns, probe_chunk, chunk, eos)); \
        break;
        APPLY_FOR_JOIN_VARIANTS(M)
#undef M
    default:
        return Status::InternalError("not supported");
    }
    return Status::OK();
}

Status JoinHashTable::probe_remain(RuntimeState* state, ChunkPtr* chunk, bool* eos) {
    switch (_hash_map_type) {
    case JoinHashMapType::empty:
        break;
#define M(NAME)                                                    \
    case JoinHashMapType::NAME:                                    \
        RETURN_IF_ERROR(_##NAME->probe_remain(state, chunk, eos)); \
        break;
        APPLY_FOR_JOIN_VARIANTS(M)
#undef M
    default:
        return Status::InternalError("not supported");
    }
    return Status::OK();
}

Status JoinHashTable::append_chunk(RuntimeState* state, const ChunkPtr& chunk) {
    Columns& columns = _table_items->build_chunk->columns();
    size_t chunk_memory_size = 0;

    for (size_t i = 0; i < _table_items->build_column_count; i++) {
        SlotDescriptor* slot = _table_items->build_slots[i].slot;
        ColumnPtr& column = chunk->get_column_by_slot_id(slot->id());
        chunk_memory_size += column->memory_usage();

        if (columns[i]->is_nullable()) {
            columns[i]->append(*column, 0, chunk->num_rows());
        } else if (column->is_nullable()) {
            // upgrade to nullable column
            columns[i] = NullableColumn::create(columns[i], NullColumn::create(columns[i]->size(), 0));
            columns[i]->append(*column, 0, chunk->num_rows());
        } else {
            columns[i]->append(*column, 0, chunk->num_rows());
        }
    }

    if (_need_create_tuple_columns) {
        const auto& tuple_id_map = chunk->get_tuple_id_to_index_map();
        for (auto iter = tuple_id_map.begin(); iter != tuple_id_map.end(); iter++) {
            if (_table_items->row_desc->get_tuple_idx(iter->first) != RowDescriptor::INVALID_IDX) {
                if (_table_items->build_chunk->is_tuple_exist(iter->first)) {
                    ColumnPtr& src_column = chunk->get_tuple_column_by_id(iter->first);
                    ColumnPtr& dest_column = _table_items->build_chunk->get_tuple_column_by_id(iter->first);
                    dest_column->append(*src_column, 0, src_column->size());
                    chunk_memory_size += src_column->memory_usage();
                } else {
                    ColumnPtr& src_column = chunk->get_tuple_column_by_id(iter->first);
                    ColumnPtr dest_column = BooleanColumn::create(_table_items->row_count + 1, 1);
                    dest_column->append(*src_column, 0, src_column->size());
                    _table_items->build_chunk->append_tuple_column(dest_column, iter->first);
                    chunk_memory_size += src_column->memory_usage();
                }
            }
        }
    }

    _table_items->row_count += chunk->num_rows();
    return Status::OK();
}

void JoinHashTable::remove_duplicate_index(Column::Filter* filter) {
    switch (_table_items->join_type) {
    case TJoinOp::LEFT_OUTER_JOIN:
        _remove_duplicate_index_for_left_outer_join(filter);
        break;
    case TJoinOp::LEFT_SEMI_JOIN:
        _remove_duplicate_index_for_left_semi_join(filter);
        break;
    case TJoinOp::LEFT_ANTI_JOIN:
    case TJoinOp::NULL_AWARE_LEFT_ANTI_JOIN:
        _remove_duplicate_index_for_left_anti_join(filter);
        break;
    case TJoinOp::RIGHT_OUTER_JOIN:
        _remove_duplicate_index_for_right_outer_join(filter);
        break;
    case TJoinOp::RIGHT_SEMI_JOIN:
        _remove_duplicate_index_for_right_semi_join(filter);
        break;
    case TJoinOp::RIGHT_ANTI_JOIN:
        _remove_duplicate_index_for_right_anti_join(filter);
        break;
    case TJoinOp::FULL_OUTER_JOIN:
        _remove_duplicate_index_for_full_outer_join(filter);
        break;
    default:
        break;
    }
}

JoinHashMapType JoinHashTable::_choose_join_hash_map() {
    size_t size = _table_items->join_keys.size();
    DCHECK_GT(size, 0);

    for (size_t i = 0; i < _table_items->join_keys.size(); i++) {
        if (!_table_items->key_columns[i]->has_null()) {
            _table_items->join_keys[i].is_null_safe_equal = false;
        }
    }

    if (size == 1 && !_table_items->join_keys[0].is_null_safe_equal) {
        switch (_table_items->join_keys[0].type) {
        case PrimitiveType::TYPE_BOOLEAN:
            return JoinHashMapType::keyboolean;
        case PrimitiveType::TYPE_TINYINT:
            return JoinHashMapType::key8;
        case PrimitiveType::TYPE_SMALLINT:
            return JoinHashMapType::key16;
        case PrimitiveType::TYPE_INT:
            return JoinHashMapType::key32;
        case PrimitiveType::TYPE_BIGINT:
            return JoinHashMapType::key64;
        case PrimitiveType::TYPE_LARGEINT:
            return JoinHashMapType::key128;
        case PrimitiveType::TYPE_FLOAT:
            // float will be convert to double, so current can't reach here
            return JoinHashMapType::keyfloat;
        case PrimitiveType::TYPE_DOUBLE:
            return JoinHashMapType::keydouble;
        case PrimitiveType::TYPE_VARCHAR:
        case PrimitiveType::TYPE_CHAR:
            return JoinHashMapType::keystring;
        case PrimitiveType::TYPE_DATE:
            // date will be convert to datetime, so current can't reach here
            return JoinHashMapType::keydate;
        case PrimitiveType::TYPE_DATETIME:
            return JoinHashMapType::keydatetime;
        case PrimitiveType::TYPE_DECIMALV2:
            return JoinHashMapType::keydecimal;
        case PrimitiveType::TYPE_DECIMAL32:
            return JoinHashMapType::keydecimal32;
        case PrimitiveType::TYPE_DECIMAL64:
            return JoinHashMapType::keydecimal64;
        case PrimitiveType::TYPE_DECIMAL128:
            return JoinHashMapType::keydecimal128;
        default:
            return JoinHashMapType::slice;
        }
    }

    size_t total_size_in_byte = 0;

    for (auto& join_key : _table_items->join_keys) {
        if (join_key.is_null_safe_equal) {
            total_size_in_byte += 1;
        }
        size_t s = _get_size_of_fixed_and_contiguous_type(join_key.type);
        if (s > 0) {
            total_size_in_byte += s;
        } else {
            return JoinHashMapType::slice;
        }
    }

    if (total_size_in_byte <= 4) {
        return JoinHashMapType::fixed32;
    }
    if (total_size_in_byte <= 8) {
        return JoinHashMapType::fixed64;
    }
    if (total_size_in_byte <= 16) {
        return JoinHashMapType::fixed128;
    }

    return JoinHashMapType::slice;
}

size_t JoinHashTable::_get_size_of_fixed_and_contiguous_type(PrimitiveType data_type) {
    switch (data_type) {
    case PrimitiveType::TYPE_BOOLEAN:
        return sizeof(RunTimeTypeTraits<PrimitiveType::TYPE_BOOLEAN>::CppType);
    case PrimitiveType::TYPE_TINYINT:
        return sizeof(RunTimeTypeTraits<PrimitiveType::TYPE_TINYINT>::CppType);
    case PrimitiveType::TYPE_SMALLINT:
        return sizeof(RunTimeTypeTraits<PrimitiveType::TYPE_SMALLINT>::CppType);
    case PrimitiveType::TYPE_INT:
        return sizeof(RunTimeTypeTraits<PrimitiveType::TYPE_INT>::CppType);
    case PrimitiveType::TYPE_BIGINT:
        return sizeof(RunTimeTypeTraits<PrimitiveType::TYPE_BIGINT>::CppType);
    case PrimitiveType::TYPE_FLOAT:
        // float will be convert to double, so current can't reach here
        return sizeof(RunTimeTypeTraits<PrimitiveType::TYPE_FLOAT>::CppType);
    case PrimitiveType::TYPE_DOUBLE:
        return sizeof(RunTimeTypeTraits<PrimitiveType::TYPE_DOUBLE>::CppType);
    case PrimitiveType::TYPE_DATE:
        // date will be convert to datetime, so current can't reach here
        return sizeof(RunTimeTypeTraits<PrimitiveType::TYPE_DATE>::CppType);
    case PrimitiveType::TYPE_DATETIME:
        return sizeof(RunTimeTypeTraits<PrimitiveType::TYPE_DATETIME>::CppType);
    default:
        return 0;
    }
}

void JoinHashTable::_remove_duplicate_index_for_left_outer_join(Column::Filter* filter) {
    size_t row_count = filter->size();

    for (size_t i = 0; i < row_count; i++) {
        if (_probe_state->probe_match_index[_probe_state->probe_index[i]] == 0) {
            (*filter)[i] = 1;
            continue;
        }

        if (_probe_state->probe_match_index[_probe_state->probe_index[i]] == 1) {
            if ((*filter)[i] == 0) {
                (*filter)[i] = 1;
            }
            continue;
        }

        if ((*filter)[i] == 0) {
            _probe_state->probe_match_index[_probe_state->probe_index[i]]--;
        }
    }
}

void JoinHashTable::_remove_duplicate_index_for_left_semi_join(Column::Filter* filter) {
    size_t row_count = filter->size();
    for (size_t i = 0; i < row_count; i++) {
        if ((*filter)[i] == 1) {
            if (_probe_state->probe_match_index[_probe_state->probe_index[i]] == 0) {
                _probe_state->probe_match_index[_probe_state->probe_index[i]] = 1;
            } else {
                (*filter)[i] = 0;
            }
        }
    }
}

void JoinHashTable::_remove_duplicate_index_for_left_anti_join(Column::Filter* filter) {
    size_t row_count = filter->size();
    for (size_t i = 0; i < row_count; i++) {
        if (_probe_state->probe_match_index[_probe_state->probe_index[i]] == 0) {
            (*filter)[i] = 1;
        } else if (_probe_state->probe_match_index[_probe_state->probe_index[i]] == 1) {
            _probe_state->probe_match_index[_probe_state->probe_index[i]]--;
            (*filter)[i] = !(*filter)[i];
        } else if ((*filter)[i] == 0) {
            _probe_state->probe_match_index[_probe_state->probe_index[i]]--;
        } else {
            (*filter)[i] = 0;
        }
    }
}

void JoinHashTable::_remove_duplicate_index_for_right_outer_join(Column::Filter* filter) {
    size_t row_count = filter->size();
    for (size_t i = 0; i < row_count; i++) {
        if ((*filter)[i] == 1) {
            _probe_state->build_match_index[_probe_state->build_index[i]] = 1;
        }
    }
}

void JoinHashTable::_remove_duplicate_index_for_right_semi_join(Column::Filter* filter) {
    size_t row_count = filter->size();
    for (size_t i = 0; i < row_count; i++) {
        if ((*filter)[i] == 1) {
            if (_probe_state->build_match_index[_probe_state->build_index[i]] == 0) {
                _probe_state->build_match_index[_probe_state->build_index[i]] = 1;
            } else {
                (*filter)[i] = 0;
            }
        }
    }
}

void JoinHashTable::_remove_duplicate_index_for_right_anti_join(Column::Filter* filter) {
    size_t row_count = filter->size();
    for (size_t i = 0; i < row_count; i++) {
        if ((*filter)[i] == 1) {
            _probe_state->build_match_index[_probe_state->build_index[i]] = 1;
        }
    }
}

void JoinHashTable::_remove_duplicate_index_for_full_outer_join(Column::Filter* filter) {
    size_t row_count = filter->size();
    for (size_t i = 0; i < row_count; i++) {
        if (_probe_state->probe_match_index[_probe_state->probe_index[i]] == 0) {
            (*filter)[i] = 1;
            continue;
        }

        if (_probe_state->probe_match_index[_probe_state->probe_index[i]] == 1) {
            if ((*filter)[i] == 0) {
                (*filter)[i] = 1;
            } else {
                _probe_state->build_match_index[_probe_state->build_index[i]] = 1;
            }
            continue;
        }

        if ((*filter)[i] == 0) {
            _probe_state->probe_match_index[_probe_state->probe_index[i]]--;
        } else {
            _probe_state->build_match_index[_probe_state->build_index[i]] = 1;
        }
    }
}

} // namespace starrocks::vectorized
