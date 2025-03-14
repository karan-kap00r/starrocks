// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "exec/pipeline/hashjoin/hash_join_build_operator.h"

#include "runtime/runtime_filter_worker.h"

namespace starrocks {
namespace pipeline {

HashJoinBuildOperator::HashJoinBuildOperator(OperatorFactory* factory, int32_t id, const string& name,
                                             int32_t plan_node_id, HashJoinerPtr join_builder,
                                             const std::vector<HashJoinerPtr>& read_only_join_probers,
                                             size_t driver_sequence, PartialRuntimeFilterMerger* partial_rf_merger,
                                             const TJoinDistributionMode::type distribution_mode)
        : Operator(factory, id, name, plan_node_id),
          _join_builder(std::move(join_builder)),
          _read_only_join_probers(read_only_join_probers),
          _driver_sequence(driver_sequence),
          _partial_rf_merger(partial_rf_merger),
          _distribution_mode(distribution_mode) {}

Status HashJoinBuildOperator::push_chunk(RuntimeState* state, const vectorized::ChunkPtr& chunk) {
    return _join_builder->append_chunk_to_ht(state, chunk);
}

Status HashJoinBuildOperator::prepare(RuntimeState* state) {
    RETURN_IF_ERROR(Operator::prepare(state));

    _join_builder->ref();
    for (auto& read_only_join_prober : _read_only_join_probers) {
        read_only_join_prober->ref();
    }

    return _join_builder->prepare(state);
}
void HashJoinBuildOperator::close(RuntimeState* state) {
    for (auto& read_only_join_prober : _read_only_join_probers) {
        read_only_join_prober->unref(state);
    }
    _join_builder->unref(state);

    Operator::close(state);
}

StatusOr<vectorized::ChunkPtr> HashJoinBuildOperator::pull_chunk(RuntimeState* state) {
    const char* msg = "pull_chunk not supported in HashJoinBuildOperator";
    CHECK(false) << msg;
    return Status::NotSupported(msg);
}

void HashJoinBuildOperator::set_finishing(RuntimeState* state) {
    _is_finished = true;
    _join_builder->build_ht(state);

    size_t merger_index = _driver_sequence;
    // Broadcast Join only has one build operator.
    DCHECK(_distribution_mode != TJoinDistributionMode::BROADCAST || _driver_sequence == 0);

    _join_builder->create_runtime_filters(state);

    auto ht_row_count = _join_builder->get_ht_row_count();
    auto& partial_in_filters = _join_builder->get_runtime_in_filters();
    auto& partial_bloom_filter_build_params = _join_builder->get_runtime_bloom_filter_build_params();
    auto& partial_bloom_filters = _join_builder->get_runtime_bloom_filters();
    // add partial filters generated by this HashJoinBuildOperator to PartialRuntimeFilterMerger to merge into a
    // total one.
    auto status = _partial_rf_merger->add_partial_filters(merger_index, ht_row_count, std::move(partial_in_filters),
                                                          std::move(partial_bloom_filter_build_params),
                                                          std::move(partial_bloom_filters));
    if (status.ok() && status.value()) {
        auto&& in_filters = _partial_rf_merger->get_total_in_filters();
        auto&& bloom_filters = _partial_rf_merger->get_total_bloom_filters();

        // publish runtime bloom-filters
        state->runtime_filter_port()->publish_runtime_filters(bloom_filters);
        // move runtime filters into RuntimeFilterHub.
        runtime_filter_hub()->set_collector(_plan_node_id, std::make_unique<RuntimeFilterCollector>(
                                                                   std::move(in_filters), std::move(bloom_filters)));
    }

    for (auto& read_only_join_prober : _read_only_join_probers) {
        read_only_join_prober->reference_hash_table(_join_builder.get());
    }
    _join_builder->enter_probe_phase();
    for (auto& read_only_join_prober : _read_only_join_probers) {
        read_only_join_prober->enter_probe_phase();
    }
}

HashJoinBuildOperatorFactory::HashJoinBuildOperatorFactory(
        int32_t id, int32_t plan_node_id, HashJoinerFactoryPtr hash_joiner_factory,
        std::unique_ptr<PartialRuntimeFilterMerger>&& partial_rf_merger,
        const TJoinDistributionMode::type distribution_mode)
        : OperatorFactory(id, "hash_join_build", plan_node_id),
          _hash_joiner_factory(std::move(hash_joiner_factory)),
          _partial_rf_merger(std::move(partial_rf_merger)),
          _distribution_mode(distribution_mode) {}

Status HashJoinBuildOperatorFactory::prepare(RuntimeState* state) {
    RETURN_IF_ERROR(OperatorFactory::prepare(state));
    return _hash_joiner_factory->prepare(state);
}

void HashJoinBuildOperatorFactory::close(RuntimeState* state) {
    _hash_joiner_factory->close(state);
    OperatorFactory::close(state);
}

OperatorPtr HashJoinBuildOperatorFactory::create(int32_t degree_of_parallelism, int32_t driver_sequence) {
    return std::make_shared<HashJoinBuildOperator>(this, _id, _name, _plan_node_id,
                                                   _hash_joiner_factory->create_builder(driver_sequence),
                                                   _hash_joiner_factory->get_read_only_probers(), driver_sequence,
                                                   _partial_rf_merger.get(), _distribution_mode);
}

} // namespace pipeline
} // namespace starrocks
