// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

package com.starrocks.sql.optimizer.task;

import com.google.common.collect.Lists;
import com.starrocks.catalog.Catalog;
import com.starrocks.common.Pair;
import com.starrocks.qe.ConnectContext;
import com.starrocks.sql.optimizer.ExpressionContext;
import com.starrocks.sql.optimizer.Group;
import com.starrocks.sql.optimizer.GroupExpression;
import com.starrocks.sql.optimizer.OutputPropertyDeriver;
import com.starrocks.sql.optimizer.RequiredPropertyDeriver;
import com.starrocks.sql.optimizer.Utils;
import com.starrocks.sql.optimizer.base.ColumnRefSet;
import com.starrocks.sql.optimizer.base.DistributionProperty;
import com.starrocks.sql.optimizer.base.DistributionSpec;
import com.starrocks.sql.optimizer.base.GatherDistributionSpec;
import com.starrocks.sql.optimizer.base.PhysicalPropertySet;
import com.starrocks.sql.optimizer.base.SortProperty;
import com.starrocks.sql.optimizer.cost.CostModel;
import com.starrocks.sql.optimizer.operator.OperatorType;
import com.starrocks.sql.optimizer.operator.physical.PhysicalDistributionOperator;
import com.starrocks.sql.optimizer.operator.physical.PhysicalHashAggregateOperator;
import com.starrocks.sql.optimizer.operator.physical.PhysicalHashJoinOperator;
import com.starrocks.sql.optimizer.operator.scalar.BinaryPredicateOperator;
import com.starrocks.sql.optimizer.statistics.ColumnStatistic;
import com.starrocks.sql.optimizer.statistics.Statistics;
import com.starrocks.sql.optimizer.statistics.StatisticsCalculator;

import java.util.List;

import static com.starrocks.sql.optimizer.rule.transformation.JoinPredicateUtils.getEqConj;

/**
 * EnforceAndCostTask costs a physical expression.
 * The root operator is cost first
 * and the lowest cost of each child group is added.
 * <p>
 * Finally, properties are enforced to meet requirement in the context.
 * <p>
 * We apply pruning by terminating if
 * the current expression's cost is larger than the upper bound of the current group
 * <p>
 * EnforceAndCostTask implementation inspire by Cascades paper and CMU noisepage project
 */
public class EnforceAndCostTask extends OptimizerTask implements Cloneable {
    private final GroupExpression groupExpression;
    // multi required PropertySets for children
    private List<List<PhysicalPropertySet>> requiredPropertiesList;
    // localCost + sum of all InputCost entries.
    private double curTotalCost;
    // the local cost of the group expression
    private double localCost;
    // Current stage of enumeration through child groups
    private int curChildIndex = -1;
    // Indicator of last child group that we waited for optimization
    private int prevChildIndex = -1;
    // Current stage of enumeration through outputInputProperties
    private int curPropertyPairIndex = 0;
    //
    private final List<GroupExpression> childrenBestExprList = Lists.newArrayList();
    private final List<PhysicalPropertySet> childrenOutputProperties = Lists.newArrayList();

    EnforceAndCostTask(TaskContext context, GroupExpression expression) {
        super(context);
        this.groupExpression = expression;
    }

    // Shallow Clone here
    // We don't need to clone outputInputProperties and groupExpression
    @Override
    public Object clone() {
        EnforceAndCostTask task = null;
        try {
            task = (EnforceAndCostTask) super.clone();
        } catch (CloneNotSupportedException ignored) {
        }
        return task;
    }

    @Override
    public String toString() {
        return "EnforceAndCostTask for groupExpression " + groupExpression +
                "\n curChildIndex " + curChildIndex +
                "\n prevChildIndex " + prevChildIndex +
                "\n curTotalCost " + curTotalCost;
    }

    // 1. Get required properties according to node for children nodes.
    // 2. Get best child group expression, it will optimize the children group from the top down
    // 3. Get node output property with children output properties, it will add enforcer for children if children output
    //    property can not satisfy the requirements now.
    // 4. Add enforcer for node if it can not satisfy the requirements.
    @Override
    public void execute() {
        if (groupExpression.isUnused()) {
            return;
        }
        // Init costs and get required properties for children
        initRequiredProperties();

        for (; curPropertyPairIndex < requiredPropertiesList.size(); curPropertyPairIndex++) {
            List<PhysicalPropertySet> requiredProperties = requiredPropertiesList.get(curPropertyPairIndex);

            // Calculate local cost and update total cost
            if (curChildIndex == 0 && prevChildIndex == -1) {
                localCost = CostModel.calculateCost(groupExpression);
                curTotalCost += localCost;
            }

            for (; curChildIndex < groupExpression.getInputs().size(); curChildIndex++) {
                PhysicalPropertySet requiredProperty = requiredProperties.get(curChildIndex);
                Group childGroup = groupExpression.getInputs().get(curChildIndex);

                // Check whether the child group is already optimized for the property
                GroupExpression childBestExpr = childGroup.getBestExpression(requiredProperty);

                if (childBestExpr == null && prevChildIndex >= curChildIndex) {
                    // If there can not find best child expr or push child's OptimizeGroupTask, The child has been
                    // pruned because of UpperBound cost prune, and parent task can break here and return
                    break;
                }

                if (childBestExpr == null) {
                    // We haven't optimized child group
                    prevChildIndex = curChildIndex;
                    optimizeChildGroup(requiredProperty, childGroup);
                    return;
                }

                childrenBestExprList.add(childBestExpr);
                // Get the output properties of children
                PhysicalPropertySet childOutputProperty = childBestExpr.getOutputProperty(requiredProperty);
                childrenOutputProperties.add(childOutputProperty);
                // Change child required property to child output property
                requiredProperties.set(curChildIndex, childOutputProperty);

                // check if we can generate one stage agg
                if (!canGenerateOneStageAgg(childBestExpr)) {
                    break;
                }

                if (!checkBroadcastRowCountLimit(requiredProperty, childBestExpr)) {
                    break;
                }

                curTotalCost += childBestExpr.getCost(requiredProperty);
                if (curTotalCost > context.getUpperBoundCost()) {
                    break;
                }
            }

            // Successfully optimize all child group
            if (curChildIndex == groupExpression.getInputs().size()) {
                // compute the output property
                OutputPropertyDeriver outputPropertyDeriver = new OutputPropertyDeriver(context);
                Pair<PhysicalPropertySet, Double> outputPropertyWithCost = outputPropertyDeriver
                        .getOutputPropertyWithCost(context.getRequiredProperty(), groupExpression, childrenBestExprList,
                                requiredProperties, childrenOutputProperties, curTotalCost);
                PhysicalPropertySet outputProperty = outputPropertyWithCost.first;
                curTotalCost = outputPropertyWithCost.second;
                if (curTotalCost > context.getUpperBoundCost()) {
                    break;
                }

                // update current group statistics and re-compute costs
                if (!computeCurrentGroupStatistics()) {
                    // child group has been prune
                    return;
                }

                recordCostsAndEnforce(outputProperty, requiredProperties);
            }
            // Reset child idx and total cost
            prevChildIndex = -1;
            curChildIndex = 0;
            curTotalCost = 0;
            childrenBestExprList.clear();
            childrenOutputProperties.clear();
        }
    }

    private void initRequiredProperties() {
        if (curChildIndex != -1) {
            // Has been init output properties, is optimizer the operator again
            return;
        }

        localCost = 0;
        curTotalCost = 0;

        // TODO(kks): do Lower Bound Pruning here
        RequiredPropertyDeriver requiredPropertyDeriver = new RequiredPropertyDeriver(context.getRequiredProperty());
        requiredPropertiesList = requiredPropertyDeriver.getRequiredProps(groupExpression);
        curChildIndex = 0;
    }

    private void optimizeChildGroup(PhysicalPropertySet inputProperty, Group childGroup) {
        pushTask((EnforceAndCostTask) clone());
        double newUpperBound = context.getUpperBoundCost() - curTotalCost;
        TaskContext taskContext = new TaskContext(context.getOptimizerContext(),
                inputProperty, context.getRequiredColumns(), newUpperBound, context.getAllScanOperators());
        pushTask(new OptimizeGroupTask(taskContext, childGroup));
    }

    // Check if the broadcast table row count exceeds the broadcastRowCountLimit.
    // This check needs to meet several criteria, such as the join type and the size of the left and right tables。
    private boolean checkBroadcastRowCountLimit(PhysicalPropertySet inputProperty, GroupExpression childBestExpr) {
        if (!inputProperty.getDistributionProperty().isBroadcast()) {
            return true;
        }

        if (!OperatorType.PHYSICAL_HASH_JOIN.equals(groupExpression.getOp().getOpType())) {
            return true;
        }
        PhysicalHashJoinOperator node = (PhysicalHashJoinOperator) groupExpression.getOp();
        // If broadcast child has hint, need to change the cost to zero
        double childCost = childBestExpr.getCost(inputProperty);
        if (node.getJoinHint().equalsIgnoreCase("BROADCAST")
                && childCost == Double.POSITIVE_INFINITY) {
            List<PhysicalPropertySet> childInputProperties =
                    childBestExpr.getInputProperties(inputProperty);
            childBestExpr.setPropertyWithCost(inputProperty, childInputProperties, 0);
        }

        // if this groupExpression can only do Broadcast, don't need to check the broadcastRowCountLimit
        ColumnRefSet leftChildColumns = groupExpression.getChildOutputColumns(0);
        ColumnRefSet rightChildColumns = groupExpression.getChildOutputColumns(1);
        List<BinaryPredicateOperator> equalOnPredicate =
                getEqConj(leftChildColumns, rightChildColumns, Utils.extractConjuncts(node.getOnPredicate()));
        if (Utils.canOnlyDoBroadcast(node, equalOnPredicate, node.getJoinHint())) {
            return true;
        }
        // Only when right table is not significantly smaller than left table, consider the
        // broadcastRowCountLimit, Otherwise, this limit is not considered, which can avoid
        // shuffling large left-hand table data
        int parallelExecInstance = Math.max(1,
                Math.min(groupExpression.getGroup().getLogicalProperty().getLeftMostScanTabletsNum(),
                        ConnectContext.get().getSessionVariable().getDegreeOfParallelism()));
        int beNum = Math.max(1, Catalog.getCurrentSystemInfo().getBackendIds(true).size());
        Statistics leftChildStats = groupExpression.getInputs().get(curChildIndex - 1).getStatistics();
        Statistics rightChildStats = groupExpression.getInputs().get(curChildIndex).getStatistics();
        if (leftChildStats == null || rightChildStats == null) {
            return false;
        }
        double leftOutputSize = leftChildStats.getOutputSize(groupExpression.getChildOutputColumns(curChildIndex - 1));
        double rightOutputSize = rightChildStats.getOutputSize(groupExpression.getChildOutputColumns(curChildIndex));

        if (leftOutputSize < rightOutputSize * parallelExecInstance * beNum * 10
                && rightChildStats.getOutputRowCount() >
                ConnectContext.get().getSessionVariable().getBroadcastRowCountLimit()) {
            return false;
        }
        return true;
    }

    private void setSatisfiedPropertyWithCost(PhysicalPropertySet outputProperty,
                                              List<PhysicalPropertySet> inputProperties) {
        // groupExpression can satisfy the itself output property
        setPropertyWithCost(groupExpression, outputProperty, inputProperties);
        // groupExpression can satisfy the ANY type output property
        setPropertyWithCost(groupExpression, outputProperty, PhysicalPropertySet.EMPTY, inputProperties);
    }

    private void recordCostsAndEnforce(PhysicalPropertySet outputProperty, List<PhysicalPropertySet> inputProperties) {
        // re-calculate local cost and update total cost
        curTotalCost -= localCost;
        localCost = CostModel.calculateCost(groupExpression);
        curTotalCost += localCost;

        setSatisfiedPropertyWithCost(outputProperty, inputProperties);
        PhysicalPropertySet requiredProperty = context.getRequiredProperty();
        // Enforce property if outputProperty doesn't satisfy context requiredProperty
        if (!outputProperty.isSatisfy(requiredProperty)) {
            // Enforce the property to meet the required property
            PhysicalPropertySet enforcedProperty = enforceProperty(outputProperty, requiredProperty);

            // enforcedProperty is superset of requiredProperty
            if (!enforcedProperty.equals(requiredProperty)) {
                setPropertyWithCost(groupExpression.getGroup().getBestExpression(enforcedProperty),
                        requiredProperty, Lists.newArrayList(outputProperty));
            }
        } else {

            // outputProperty is superset of requiredProperty
            if (!outputProperty.equals(requiredProperty)) {
                setPropertyWithCost(groupExpression, outputProperty, requiredProperty, inputProperties);
            }
        }

        if (curTotalCost < context.getUpperBoundCost()) {
            // update context upperbound cost
            context.setUpperBoundCost(curTotalCost);
        }
    }

    // Disable one phase Agg node with unknown column statistics or table row count may not accurate because of
    // fe meta may not get real row count from be.
    // NOTE: Not include one phase local Agg node
    private boolean canGenerateOneStageAgg(GroupExpression childBestExpr) {
        if (!OperatorType.PHYSICAL_HASH_AGG.equals(groupExpression.getOp().getOpType())) {
            return true;
        }
        // respect session variable new_planner_agg_stage
        int aggStage = ConnectContext.get().getSessionVariable().getNewPlannerAggStage();
        if (aggStage == 1) {
            return true;
        }
        // Must do one stage aggregate If the child contains limit
        if (childBestExpr.getOp() instanceof PhysicalDistributionOperator) {
            PhysicalDistributionOperator distributionOperator =
                    (PhysicalDistributionOperator) childBestExpr.getOp();
            if (distributionOperator.getDistributionSpec().getType().equals(DistributionSpec.DistributionType.GATHER) &&
                    ((GatherDistributionSpec) distributionOperator.getDistributionSpec()).hasLimit()) {
                return true;
            }
        }

        PhysicalHashAggregateOperator aggregate = (PhysicalHashAggregateOperator) groupExpression.getOp();
        // 1. check the agg node is global aggregation without split and child expr is PhysicalDistributionOperator
        if (aggregate.getType().isGlobal() && !aggregate.isSplit() &&
                childBestExpr.getOp() instanceof PhysicalDistributionOperator) {
            // 1.1 check default column statistics or child output row may not be accurate
            if (groupExpression.getGroup().getStatistics().getColumnStatistics().values().stream()
                    .anyMatch(ColumnStatistic::isUnknown) ||
                    childBestExpr.getGroup().getStatistics().isTableRowCountMayInaccurate()) {
                return false;
            }
            // 1.2 disable one stage agg with multi group by columns
            return aggregate.getGroupBys().size() <= 1;
        }
        return true;
    }

    private boolean computeCurrentGroupStatistics() {
        ExpressionContext expressionContext = new ExpressionContext(groupExpression);
        if (groupExpression.getInputs().stream().anyMatch(group -> group.getStatistics() == null)) {
            return false;
        }

        StatisticsCalculator statisticsCalculator = new StatisticsCalculator(expressionContext,
                context.getOptimizerContext().getColumnRefFactory(), context.getOptimizerContext());
        statisticsCalculator.estimatorStats();
        groupExpression.getGroup().setStatistics(expressionContext.getStatistics());
        return true;
    }

    private void setPropertyWithCost(GroupExpression groupExpression,
                                     PhysicalPropertySet outputProperty,
                                     PhysicalPropertySet requiredProperty,
                                     List<PhysicalPropertySet> inputProperties) {
        if (groupExpression.setPropertyWithCost(requiredProperty, inputProperties, curTotalCost)) {
            // Each group expression need to record the outputProperty satisfy what requiredProperty,
            // because group expression can generate multi outputProperty. eg. Join may have shuffle local
            // and shuffle join two types outputProperty.
            groupExpression.setOutputPropertySatisfyRequiredProperty(outputProperty, requiredProperty);
        }
        this.groupExpression.getGroup().setBestExpression(groupExpression,
                curTotalCost, requiredProperty);
        if (ConnectContext.get().getSessionVariable().isSetUseNthExecPlan()) {
            // record the output/input properties when child group could satisfy this group expression required property
            groupExpression.addValidOutputInputProperties(requiredProperty, inputProperties);
            this.groupExpression.getGroup()
                    .addSatisfyRequiredPropertyGroupExpression(requiredProperty, groupExpression);
        }
    }

    private void setPropertyWithCost(GroupExpression groupExpression,
                                     PhysicalPropertySet requiredProperty,
                                     List<PhysicalPropertySet> inputProperties) {
        setPropertyWithCost(groupExpression, requiredProperty, requiredProperty, inputProperties);
    }

    private PhysicalPropertySet enforceProperty(PhysicalPropertySet outputProperty,
                                                PhysicalPropertySet requiredProperty) {
        boolean satisfyOrderProperty =
                outputProperty.getSortProperty().isSatisfy(requiredProperty.getSortProperty());
        boolean satisfyDistributionProperty =
                outputProperty.getDistributionProperty().isSatisfy(requiredProperty.getDistributionProperty());

        PhysicalPropertySet enforcedProperty = null;
        if (!satisfyDistributionProperty && satisfyOrderProperty) {
            if (requiredProperty.getSortProperty().isEmpty()) {
                enforcedProperty = enforceDistribute(outputProperty);
            } else {
                /*
                 * The sorting attribute does not make sense when the sort property is not empty,
                 * because after the data is redistributed, the original order requirements cannot be guaranteed.
                 * So we need to enforce "SortNode" here
                 *
                 * Because we build a parent-child relationship based on property.
                 * So here we hack to eliminate the original property to prevent an endless loop
                 * eg: [order by v1, gather] -> [order by v1, shuffle] -> [order by v1, shuffle] may endless loop,
                 * because repartition require sort again
                 */
                PhysicalPropertySet newProperty =
                        new PhysicalPropertySet(DistributionProperty.EMPTY, SortProperty.EMPTY);
                groupExpression.getGroup().replaceBestExpressionProperty(outputProperty, newProperty,
                        groupExpression.getCost(outputProperty));
                enforcedProperty = enforceSortAndDistribute(newProperty, requiredProperty);
            }
        } else if (satisfyDistributionProperty && !satisfyOrderProperty) {
            enforcedProperty = enforceSort(outputProperty);
        } else if (!satisfyDistributionProperty) {
            enforcedProperty = enforceSortAndDistribute(outputProperty, requiredProperty);
        }
        return enforcedProperty;
    }

    private PhysicalPropertySet enforceDistribute(PhysicalPropertySet oldOutputProperty) {
        PhysicalPropertySet newOutputProperty = oldOutputProperty.copy();
        newOutputProperty.setDistributionProperty(context.getRequiredProperty().getDistributionProperty());
        GroupExpression enforcer =
                context.getRequiredProperty().getDistributionProperty().appendEnforcers(groupExpression.getGroup());

        updateCostWithEnforcer(enforcer, oldOutputProperty, newOutputProperty);

        return newOutputProperty;
    }

    private PhysicalPropertySet enforceSort(PhysicalPropertySet oldOutputProperty) {
        PhysicalPropertySet newOutputProperty = oldOutputProperty.copy();
        newOutputProperty.setSortProperty(context.getRequiredProperty().getSortProperty());
        GroupExpression enforcer =
                context.getRequiredProperty().getSortProperty().appendEnforcers(groupExpression.getGroup());

        updateCostWithEnforcer(enforcer, oldOutputProperty, newOutputProperty);

        return newOutputProperty;
    }

    private PhysicalPropertySet enforceSortAndDistribute(PhysicalPropertySet outputProperty,
                                                         PhysicalPropertySet requiredProperty) {
        PhysicalPropertySet enforcedProperty;
        if (requiredProperty.getDistributionProperty().getSpec()
                .equals(DistributionSpec.createGatherDistributionSpec())) {
            enforcedProperty = enforceSort(outputProperty);
            enforcedProperty = enforceDistribute(enforcedProperty);
        } else {
            enforcedProperty = enforceDistribute(outputProperty);
            enforcedProperty = enforceSort(enforcedProperty);
        }

        return enforcedProperty;
    }

    private void updateCostWithEnforcer(GroupExpression enforcer,
                                        PhysicalPropertySet oldOutputProperty,
                                        PhysicalPropertySet newOutputProperty) {
        context.getOptimizerContext().getMemo().
                insertEnforceExpression(enforcer, groupExpression.getGroup());
        curTotalCost += CostModel.calculateCost(enforcer);

        if (enforcer.setPropertyWithCost(newOutputProperty, Lists.newArrayList(oldOutputProperty), curTotalCost)) {
            enforcer.setOutputPropertySatisfyRequiredProperty(newOutputProperty, context.getRequiredProperty());
        }
        groupExpression.getGroup().setBestExpression(enforcer, curTotalCost, newOutputProperty);
        if (ConnectContext.get().getSessionVariable().isSetUseNthExecPlan()) {
            enforcer.addValidOutputInputProperties(newOutputProperty, Lists.newArrayList(PhysicalPropertySet.EMPTY));
            groupExpression.getGroup().addSatisfyRequiredPropertyGroupExpression(newOutputProperty, enforcer);
        }
    }
}
