// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "exec/vectorized/cross_join_node.h"

#include "column/chunk.h"
#include "column/column_helper.h"
#include "exec/pipeline/crossjoin/cross_join_context.h"
#include "exec/pipeline/crossjoin/cross_join_left_operator.h"
#include "exec/pipeline/crossjoin/cross_join_right_sink_operator.h"
#include "exec/pipeline/limit_operator.h"
#include "exec/pipeline/operator.h"
#include "exec/pipeline/pipeline_builder.h"
#include "exprs/expr_context.h"
#include "runtime/current_thread.h"
#include "runtime/runtime_state.h"

namespace starrocks::vectorized {
CrossJoinNode::CrossJoinNode(ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs)
        : ExecNode(pool, tnode, descs) {}

Status CrossJoinNode::init(const TPlanNode& tnode, RuntimeState* state) {
    RETURN_IF_ERROR(ExecNode::init(tnode, state));
    if (tnode.__isset.need_create_tuple_columns) {
        _need_create_tuple_columns = tnode.need_create_tuple_columns;
    }
    return Status::OK();
}

Status CrossJoinNode::prepare(RuntimeState* state) {
    RETURN_IF_ERROR(ExecNode::prepare(state));

    _build_timer = ADD_TIMER(runtime_profile(), "BuildTime");
    _probe_timer = ADD_TIMER(runtime_profile(), "ProbeTime");
    _build_rows_counter = ADD_COUNTER(runtime_profile(), "BuildRows", TUnit::UNIT);
    _probe_rows_counter = ADD_COUNTER(runtime_profile(), "ProbeRows", TUnit::UNIT);

    _init_row_desc();
    return Status::OK();
}

Status CrossJoinNode::open(RuntimeState* state) {
    SCOPED_TIMER(_runtime_profile->total_time_counter());
    RETURN_IF_ERROR(ExecNode::open(state));

    RETURN_IF_ERROR(_build(state));

    RETURN_IF_ERROR(child(0)->open(state));

    if (_build_chunk != nullptr) {
        _mem_tracker->set(_build_chunk->memory_usage());
    }

    return Status::OK();
}

Status CrossJoinNode::_get_next_probe_chunk(RuntimeState* state) {
    while (true) {
        RETURN_IF_ERROR(child(0)->get_next(state, &_probe_chunk, &_eos));
        if (_eos) {
            return Status::OK();
        }
        if (_probe_chunk->num_rows() <= 0) {
            continue;
        }

        COUNTER_UPDATE(_probe_rows_counter, _probe_chunk->num_rows());
        break;
    }

    _build_chunks_index = 0;
    _probe_chunk_index = 0;

    return Status::OK();
}

void CrossJoinNode::_copy_joined_rows_with_index_base_probe(ChunkPtr& chunk, size_t row_count, size_t probe_index,
                                                            size_t build_index) {
    for (size_t i = 0; i < _probe_column_count; i++) {
        SlotDescriptor* slot = _col_types[i];
        ColumnPtr& dest_col = chunk->get_column_by_slot_id(slot->id());
        ColumnPtr& src_col = _probe_chunk->get_column_by_slot_id(slot->id());
        _copy_probe_rows_with_index_base_probe(dest_col, src_col, probe_index, row_count);
    }

    for (size_t i = 0; i < _build_column_count; i++) {
        SlotDescriptor* slot = _col_types[i + _probe_column_count];
        ColumnPtr& dest_col = chunk->get_column_by_slot_id(slot->id());
        ColumnPtr& src_col = _build_chunk->get_column_by_slot_id(slot->id());
        _copy_build_rows_with_index_base_probe(dest_col, src_col, build_index, row_count);
    }

    for (int tuple_id : _output_probe_tuple_ids) {
        if (_probe_chunk->is_tuple_exist(tuple_id)) {
            ColumnPtr& src_col = _probe_chunk->get_tuple_column_by_id(tuple_id);
            auto& src_data = ColumnHelper::as_raw_column<BooleanColumn>(src_col)->get_data();
            ColumnPtr& dest_col = chunk->get_tuple_column_by_id(tuple_id);
            auto& dest_data = ColumnHelper::as_raw_column<BooleanColumn>(dest_col)->get_data();
            for (size_t i = 0; i < row_count; i++) {
                dest_data.emplace_back(src_data[probe_index]);
            }
        }
    }

    for (int tuple_id : _output_build_tuple_ids) {
        if (_build_chunk->is_tuple_exist(tuple_id)) {
            ColumnPtr& src_col = _build_chunk->get_tuple_column_by_id(tuple_id);
            auto& src_data = ColumnHelper::as_raw_column<BooleanColumn>(src_col)->get_data();
            ColumnPtr& dest_col = chunk->get_tuple_column_by_id(tuple_id);
            auto& dest_data = ColumnHelper::as_raw_column<BooleanColumn>(dest_col)->get_data();

            for (size_t i = 0; i < row_count; i++) {
                dest_data.emplace_back(src_data[build_index + i]);
            }
        }
    }
}

void CrossJoinNode::_copy_joined_rows_with_index_base_build(ChunkPtr& chunk, size_t row_count, size_t probe_index,
                                                            size_t build_index) {
    for (size_t i = 0; i < _probe_column_count; i++) {
        SlotDescriptor* slot = _col_types[i];
        ColumnPtr& dest_col = chunk->get_column_by_slot_id(slot->id());
        ColumnPtr& src_col = _probe_chunk->get_column_by_slot_id(slot->id());
        _copy_probe_rows_with_index_base_build(dest_col, src_col, probe_index, row_count);
    }

    for (size_t i = 0; i < _build_column_count; i++) {
        SlotDescriptor* slot = _col_types[i + _probe_column_count];
        ColumnPtr& dest_col = chunk->get_column_by_slot_id(slot->id());
        ColumnPtr& src_col = _build_chunk->get_column_by_slot_id(slot->id());
        _copy_build_rows_with_index_base_build(dest_col, src_col, build_index, row_count);
    }

    for (int tuple_id : _output_probe_tuple_ids) {
        if (_probe_chunk->is_tuple_exist(tuple_id)) {
            ColumnPtr& src_col = _probe_chunk->get_tuple_column_by_id(tuple_id);
            auto& src_data = ColumnHelper::as_raw_column<BooleanColumn>(src_col)->get_data();
            ColumnPtr& dest_col = chunk->get_tuple_column_by_id(tuple_id);
            auto& dest_data = ColumnHelper::as_raw_column<BooleanColumn>(dest_col)->get_data();
            for (size_t i = 0; i < row_count; i++) {
                dest_data.emplace_back(src_data[probe_index + i]);
            }
        }
    }

    for (int tuple_id : _output_build_tuple_ids) {
        if (_build_chunk->is_tuple_exist(tuple_id)) {
            ColumnPtr& src_col = _build_chunk->get_tuple_column_by_id(tuple_id);
            auto& src_data = ColumnHelper::as_raw_column<BooleanColumn>(src_col)->get_data();
            ColumnPtr& dest_col = chunk->get_tuple_column_by_id(tuple_id);
            auto& dest_data = ColumnHelper::as_raw_column<BooleanColumn>(dest_col)->get_data();

            for (size_t i = 0; i < row_count; i++) {
                dest_data.emplace_back(src_data[build_index]);
            }
        }
    }
}

void CrossJoinNode::_copy_probe_rows_with_index_base_probe(ColumnPtr& dest_col, ColumnPtr& src_col, size_t start_row,
                                                           size_t copy_number) {
    if (src_col->is_nullable()) {
        if (src_col->is_constant()) {
            // current can't reach here
            dest_col->append_nulls(copy_number);
        } else {
            // repeat the value from probe table for copy_number times
            dest_col->append_value_multiple_times(*src_col.get(), start_row, copy_number);
        }
    } else {
        if (src_col->is_constant()) {
            // current can't reach here
            auto* const_col = ColumnHelper::as_raw_column<ConstColumn>(src_col);
            // repeat the constant value from probe table for copy_number times
            _buf_selective.assign(copy_number, 0);
            dest_col->append_selective(*const_col->data_column(), &_buf_selective[0], 0, copy_number);
        } else {
            // repeat the value from probe table for copy_number times
            dest_col->append_value_multiple_times(*src_col.get(), start_row, copy_number);
        }
    }
}

void CrossJoinNode::_copy_probe_rows_with_index_base_build(ColumnPtr& dest_col, ColumnPtr& src_col, size_t start_row,
                                                           size_t copy_number) {
    if (src_col->is_nullable()) {
        if (src_col->is_constant()) {
            // current can't reach here
            dest_col->append_nulls(copy_number);
        } else {
            // repeat the value from probe table for copy_number times
            dest_col->append(*src_col.get(), start_row, copy_number);
        }
    } else {
        if (src_col->is_constant()) {
            // current can't reach here
            auto* const_col = ColumnHelper::as_raw_column<ConstColumn>(src_col);
            // repeat the constant value from probe table for copy_number times
            _buf_selective.assign(copy_number, 0);
            dest_col->append_selective(*const_col->data_column(), &_buf_selective[0], 0, copy_number);
        } else {
            // repeat the value from probe table for copy_number times
            dest_col->append(*src_col.get(), start_row, copy_number);
        }
    }
}

void CrossJoinNode::_copy_build_rows_with_index_base_probe(ColumnPtr& dest_col, ColumnPtr& src_col, size_t start_row,
                                                           size_t row_count) {
    if (!src_col->is_nullable()) {
        if (src_col->is_constant()) {
            // current can't reach here
            auto* const_col = ColumnHelper::as_raw_column<ConstColumn>(src_col);
            // repeat the constant value for copy_number times
            _buf_selective.assign(row_count, 0);
            dest_col->append_selective(*const_col->data_column(), &_buf_selective[0], 0, row_count);
        } else {
            dest_col->append(*src_col.get(), start_row, row_count);
        }
    } else {
        if (src_col->is_constant()) {
            // current can't reach here
            dest_col->append_nulls(row_count);
        } else {
            dest_col->append(*src_col.get(), start_row, row_count);
        }
    }
}

void CrossJoinNode::_copy_build_rows_with_index_base_build(ColumnPtr& dest_col, ColumnPtr& src_col, size_t start_row,
                                                           size_t row_count) {
    if (!src_col->is_nullable()) {
        if (src_col->is_constant()) {
            // current can't reach here
            auto* const_col = ColumnHelper::as_raw_column<ConstColumn>(src_col);
            // repeat the constant value for copy_number times
            _buf_selective.assign(row_count, 0);
            dest_col->append_selective(*const_col->data_column(), &_buf_selective[0], 0, row_count);
        } else {
            dest_col->append_value_multiple_times(*src_col.get(), start_row, row_count);
        }
    } else {
        if (src_col->is_constant()) {
            // current can't reach here
            dest_col->append_nulls(row_count);
        } else {
            dest_col->append_value_multiple_times(*src_col.get(), start_row, row_count);
        }
    }
}

/*
First, build a large chunk to contain the right table.
Then the right table is divided into two parts.
The number of rows is a multiple of 4096 (big_Chunk) and small with less than 4096 rows(small_chunk).

right table is about _number_of_build_rows rows.
big_chunk's range is    [0 - _build_chunks_size)
small_chunk's range is  [_build_chunks_size - _number_of_build_rows)

For each chunk probe in the left table, probe_chunk,
we need to make it iterate with the right table, as iteratoe big_chunk and small_chunk separately.

Our way is
step 1: For every row in probe_chunk, use it to iterate with big_chunk.
Step 2: For probe_chunk and small_chunk, use every row in smaller chunk to iterate with bigger chunk.

So far, probe_chunk is done, we will process next chunk.
*/
Status CrossJoinNode::get_next_internal(RuntimeState* state, ChunkPtr* chunk, bool* eos,
                                        ScopedTimer<MonotonicStopWatch>& probe_timer) {
    RETURN_IF_CANCELLED(state);

    *chunk = nullptr;
    if (_eos) {
        *eos = true;
        return Status::OK();
    }

    if (_build_chunk == nullptr || _build_chunk->num_rows() == 0) {
        _eos = true;
        *eos = true;
        return Status::OK();
    }

    for (;;) {
        // need to get probe_chunk
        if (_probe_chunk == nullptr || _probe_chunk->num_rows() == 0) {
            probe_timer.stop();
            RETURN_IF_ERROR(_get_next_probe_chunk(state));
            probe_timer.start();
            if (_eos) {
                if (*chunk == nullptr || (*chunk)->num_rows() < 1) {
                    *chunk = nullptr;
                    *eos = true;
                    return Status::OK();
                } else {
                    // should output (*chunk) first before EOS
                    ExecNode::eval_conjuncts(_conjunct_ctxs, (*chunk).get());
                    break;
                }
            }
            continue;
        }

        if ((*chunk) == nullptr) {
            // we need a valid probe chunk to initialize the new chunk.
            _init_chunk(chunk);
        }

        // need row_count to fill in chunk.
        size_t row_count = runtime_state()->chunk_size() - (*chunk)->num_rows();

        // means we have scan all chunks of right tables.
        // we should scan all remain rows of right table.
        // once _probe_chunk_index == _probe_chunk->num_rows() is true,
        // this condition will always true for this _probe_chunk,
        // Until _probe_chunk be done.
        if (_probe_chunk_index == _probe_chunk->num_rows()) {
            // step 2:
            // if left chunk is bigger than right, we shuld scan left based on right.
            if (_probe_chunk_index > _number_of_build_rows - _build_chunks_size) {
                if (row_count > _probe_chunk_index - _probe_rows_index) {
                    row_count = _probe_chunk_index - _probe_rows_index;
                }

                _copy_joined_rows_with_index_base_build(*chunk, row_count, _probe_rows_index, _build_rows_index);
                _probe_rows_index += row_count;

                if (_probe_rows_index == _probe_chunk_index) {
                    ++_build_rows_index;
                    _probe_rows_index = 0;
                }

                // _probe_chunk is done with _build_chunk.
                if (_build_rows_index >= _number_of_build_rows) {
                    _probe_chunk = nullptr;
                }
            } else {
                // if remain rows of right is bigger than left, we should scan right based on left.
                if (row_count > _number_of_build_rows - _build_rows_index) {
                    row_count = _number_of_build_rows - _build_rows_index;
                }

                _copy_joined_rows_with_index_base_probe(*chunk, row_count, _probe_rows_index, _build_rows_index);
                _build_rows_index += row_count;

                if (_build_rows_index == _number_of_build_rows) {
                    ++_probe_rows_index;
                    _build_rows_index = _build_chunks_size;
                }

                // _probe_chunk is done with _build_chunk.
                if (_probe_rows_index >= _probe_chunk_index) {
                    _probe_chunk = nullptr;
                }
            }
        } else if (_build_chunks_index < _build_chunks_size) {
            // step 1:
            // we scan all chunks of right table.
            if (row_count > _build_chunks_size - _build_chunks_index) {
                row_count = _build_chunks_size - _build_chunks_index;
            }

            _copy_joined_rows_with_index_base_probe(*chunk, row_count, _probe_chunk_index, _build_chunks_index);
            _build_chunks_index += row_count;
        } else {
            // step policy decision:
            DCHECK_EQ(_build_chunks_index, _build_chunks_size);

            if (_build_chunks_size != 0) {
                // scan right chunk_size rows for next row of left chunk.
                ++_probe_chunk_index;
                if (_probe_chunk_index < _probe_chunk->num_rows()) {
                    _build_chunks_index = 0;
                } else {
                    // if right table is all about chunks, means _probe_chunk is done.
                    if (_build_chunks_size == _number_of_build_rows) {
                        _probe_chunk = nullptr;
                    } else {
                        _build_rows_index = _build_chunks_size;
                        _probe_rows_index = 0;
                    }
                }
            } else {
                // optimized for smaller right table < 4096 rows.
                _probe_chunk_index = _probe_chunk->num_rows();
                _build_rows_index = _build_chunks_size;
                _probe_rows_index = 0;
            }
            continue;
        }

        if ((*chunk)->num_rows() < runtime_state()->chunk_size()) {
            continue;
        }

        ExecNode::eval_conjuncts(_conjunct_ctxs, (*chunk).get());

        // we get result chunk.
        break;
    }

    _num_rows_returned += (*chunk)->num_rows();
    if (reached_limit()) {
        (*chunk)->set_num_rows((*chunk)->num_rows() - (_num_rows_returned - _limit));
        _num_rows_returned = _limit;
        COUNTER_SET(_rows_returned_counter, _limit);
    } else {
        COUNTER_SET(_rows_returned_counter, _num_rows_returned);
    }

    DCHECK(!(*chunk)->has_const_column());
    DCHECK_CHUNK(*chunk);
    *eos = false;
    return Status::OK();
}

Status CrossJoinNode::get_next(RuntimeState* state, ChunkPtr* chunk, bool* eos) {
    SCOPED_TIMER(_runtime_profile->total_time_counter());
    ScopedTimer<MonotonicStopWatch> probe_timer(_probe_timer);
    return ExecNode::get_next_big_chunk(
            state, chunk, eos, _pre_output_chunk,
            [this, &probe_timer](RuntimeState* inner_state, ChunkPtr* inner_chunk, bool* inner_eos) -> Status {
                return this->get_next_internal(inner_state, inner_chunk, inner_eos, probe_timer);
            });
}

Status CrossJoinNode::close(RuntimeState* state) {
    if (is_closed()) {
        return Status::OK();
    }

    if (_build_chunk != nullptr) {
        _build_chunk->reset();
    }
    if (_probe_chunk != nullptr) {
        _probe_chunk->reset();
    }

    child(0)->close(state);
    return ExecNode::close(state);
}

void CrossJoinNode::_init_row_desc() {
    for (auto& tuple_desc : child(0)->row_desc().tuple_descriptors()) {
        for (auto& slot : tuple_desc->slots()) {
            _col_types.emplace_back(slot);
            _probe_column_count++;
        }
        if (_need_create_tuple_columns) {
            if (_row_descriptor.get_tuple_idx(tuple_desc->id()) != RowDescriptor::INVALID_IDX) {
                _output_probe_tuple_ids.emplace_back(tuple_desc->id());
            }
        }
    }
    for (auto& tuple_desc : child(1)->row_desc().tuple_descriptors()) {
        for (auto& slot : tuple_desc->slots()) {
            _col_types.emplace_back(slot);
            _build_column_count++;
        }
        if (_need_create_tuple_columns) {
            if (_row_descriptor.get_tuple_idx(tuple_desc->id()) != RowDescriptor::INVALID_IDX) {
                _output_build_tuple_ids.emplace_back(tuple_desc->id());
            }
        }
    }
}

Status CrossJoinNode::_build(RuntimeState* state) {
    ScopedTimer<MonotonicStopWatch> build_timer(_build_timer);
    RETURN_IF_ERROR(child(1)->open(state));

    while (true) {
        RETURN_IF_ERROR(state->check_mem_limit("CrossJoin"));
        bool eos = false;
        ChunkPtr chunk = nullptr;
        RETURN_IF_CANCELLED(state);
        build_timer.stop();
        RETURN_IF_ERROR(child(1)->get_next(state, &chunk, &eos));
        build_timer.start();
        if (eos) {
            break;
        }
        const size_t row_number = chunk->num_rows();
        if (row_number <= 0) {
            continue;
        } else {
            COUNTER_UPDATE(_build_rows_counter, row_number);
            if (_build_chunk == nullptr) {
                _build_chunk = std::move(chunk);
            } else {
                // merge chunks from child(1) (the right table) into a big chunk, which can reduce
                // the complexity and time of cross-join chunks from left table with small chunks
                // from right table.
                TRY_CATCH_BAD_ALLOC(_build_chunk->append(*chunk));
            }
        }
    }

    // Should not call num_rows on nullptr.
    if (_build_chunk != nullptr) {
        _number_of_build_rows = _build_chunk->num_rows();
        _build_chunks_size = (_number_of_build_rows / runtime_state()->chunk_size()) * runtime_state()->chunk_size();
    }

    RETURN_IF_ERROR(child(1)->close(state));
    return Status::OK();
}

void CrossJoinNode::_init_chunk(ChunkPtr* chunk) {
    ChunkPtr new_chunk = std::make_shared<Chunk>();

    // init columns for the new chunk from _probe_chunk and _build_chunk
    for (size_t i = 0; i < _probe_column_count; ++i) {
        SlotDescriptor* slot = _col_types[i];
        ColumnPtr& src_col = _probe_chunk->get_column_by_slot_id(slot->id());
        auto new_col = ColumnHelper::create_column(slot->type(), src_col->is_nullable());
        new_chunk->append_column(std::move(new_col), slot->id());
    }
    for (size_t i = 0; i < _build_column_count; ++i) {
        SlotDescriptor* slot = _col_types[_probe_column_count + i];
        ColumnPtr& src_col = _build_chunk->get_column_by_slot_id(slot->id());
        ColumnPtr new_col = ColumnHelper::create_column(slot->type(), src_col->is_nullable());
        new_chunk->append_column(std::move(new_col), slot->id());
    }

    for (int tuple_id : _output_probe_tuple_ids) {
        if (_probe_chunk->is_tuple_exist(tuple_id)) {
            ColumnPtr dest_col = BooleanColumn::create();
            new_chunk->append_tuple_column(dest_col, tuple_id);
        }
    }
    for (int tuple_id : _output_build_tuple_ids) {
        if (_build_chunk->is_tuple_exist(tuple_id)) {
            ColumnPtr dest_col = BooleanColumn::create();
            new_chunk->append_tuple_column(dest_col, tuple_id);
        }
    }

    *chunk = std::move(new_chunk);
    (*chunk)->reserve(runtime_state()->chunk_size());
}

pipeline::OpFactories CrossJoinNode::decompose_to_pipeline(pipeline::PipelineBuilderContext* context) {
    using namespace pipeline;

    // step 0: construct pipeline end with cross join right operator.
    OpFactories right_ops = _children[1]->decompose_to_pipeline(context);

    // Create a shared RefCountedRuntimeFilterCollector
    auto&& rc_rf_probe_collector = std::make_shared<RcRfProbeCollector>(2, std::move(this->runtime_filter_collector()));
    // communication with CrossJoinLeft through shared_datas.
    auto* right_source = down_cast<SourceOperatorFactory*>(right_ops[0].get());
    auto cross_join_context = std::make_shared<CrossJoinContext>(right_source->degree_of_parallelism());

    // cross_join_right as sink operator
    auto right_factory =
            std::make_shared<CrossJoinRightSinkOperatorFactory>(context->next_operator_id(), id(), cross_join_context);
    // Initialize OperatorFactory's fields involving runtime filters.
    this->init_runtime_filter_for_operator(right_factory.get(), context, rc_rf_probe_collector);
    right_ops.emplace_back(std::move(right_factory));
    context->add_pipeline(right_ops);

    // step 1: construct pipeline end with cross join left operator(cross join left maybe not sink operator).
    OpFactories left_ops = _children[0]->decompose_to_pipeline(context);

    // communication with CrossJoioRight through shared_datas.
    auto left_factory = std::make_shared<CrossJoinLeftOperatorFactory>(
            context->next_operator_id(), id(), _row_descriptor, child(0)->row_desc(), child(1)->row_desc(),
            std::move(_conjunct_ctxs), std::move(cross_join_context));
    // Initialize OperatorFactory's fields involving runtime filters.
    this->init_runtime_filter_for_operator(left_factory.get(), context, rc_rf_probe_collector);
    left_ops.emplace_back(std::move(left_factory));
    if (limit() != -1) {
        left_ops.emplace_back(std::make_shared<LimitOperatorFactory>(context->next_operator_id(), id(), limit()));
    }

    // return as the following pipeline
    return left_ops;
}

} // namespace starrocks::vectorized
