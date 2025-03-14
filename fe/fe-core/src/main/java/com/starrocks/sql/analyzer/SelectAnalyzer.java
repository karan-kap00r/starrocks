// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.
package com.starrocks.sql.analyzer;

import com.google.common.base.Preconditions;
import com.google.common.collect.ImmutableList;
import com.google.common.collect.Lists;
import com.google.common.collect.Sets;
import com.google.common.collect.Streams;
import com.starrocks.analysis.AnalyticExpr;
import com.starrocks.analysis.Expr;
import com.starrocks.analysis.FunctionCallExpr;
import com.starrocks.analysis.GroupByClause;
import com.starrocks.analysis.GroupingFunctionCallExpr;
import com.starrocks.analysis.IntLiteral;
import com.starrocks.analysis.LimitElement;
import com.starrocks.analysis.OrderByElement;
import com.starrocks.analysis.ParseNode;
import com.starrocks.analysis.SelectList;
import com.starrocks.analysis.SelectListItem;
import com.starrocks.analysis.SlotRef;
import com.starrocks.analysis.TableName;
import com.starrocks.catalog.Catalog;
import com.starrocks.catalog.Column;
import com.starrocks.catalog.Type;
import com.starrocks.common.TreeNode;
import com.starrocks.qe.ConnectContext;
import com.starrocks.sql.ast.AstVisitor;
import com.starrocks.sql.ast.CTERelation;
import com.starrocks.sql.ast.FieldReference;
import com.starrocks.sql.ast.JoinRelation;
import com.starrocks.sql.ast.Relation;
import com.starrocks.sql.ast.SelectRelation;
import com.starrocks.sql.ast.SetOperationRelation;
import com.starrocks.sql.ast.SubqueryRelation;
import com.starrocks.sql.ast.TableFunctionRelation;
import com.starrocks.sql.ast.TableRelation;
import com.starrocks.sql.ast.ValuesRelation;
import com.starrocks.sql.ast.ViewRelation;
import com.starrocks.sql.common.StarRocksPlannerException;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Optional;
import java.util.stream.Collectors;
import java.util.stream.IntStream;
import java.util.stream.Stream;

import static com.starrocks.analysis.Expr.pushNegationToOperands;
import static com.starrocks.sql.analyzer.AggregationAnalyzer.verifyOrderByAggregations;
import static com.starrocks.sql.analyzer.AggregationAnalyzer.verifySourceAggregations;
import static com.starrocks.sql.common.ErrorType.INTERNAL_ERROR;

public class SelectAnalyzer {
    private final Catalog catalog;
    private final ConnectContext session;

    public SelectAnalyzer(Catalog catalog, ConnectContext session) {
        this.catalog = catalog;
        this.session = session;
    }

    public void analyze(AnalyzeState analyzeState,
                        SelectList selectList,
                        Relation fromRelation,
                        Scope sourceScope,
                        GroupByClause groupByClause,
                        Expr havingClause,
                        Expr whereClause,
                        List<OrderByElement> sortClause,
                        LimitElement limitElement) {
        analyzeWhere(whereClause, analyzeState, sourceScope);

        List<Expr> outputExpressions =
                analyzeSelect(selectList, fromRelation, groupByClause != null, analyzeState, sourceScope);
        Scope outputScope = computeAndAssignOutputScope(selectList, analyzeState, sourceScope);

        List<Expr> groupByExpressions =
                new ArrayList<>(
                        analyzeGroupBy(groupByClause, analyzeState, sourceScope, outputScope, outputExpressions));
        if (selectList.isDistinct()) {
            groupByExpressions.addAll(outputExpressions);
        }

        analyzeHaving(havingClause, analyzeState, sourceScope, outputScope, outputExpressions);

        // Construct sourceAndOutputScope with sourceScope and outputScope
        Scope sourceAndOutputScope = computeAndAssignOrderScope(analyzeState, sourceScope, outputScope);

        List<OrderByElement> orderByElements =
                analyzeOrderBy(sortClause, analyzeState, sourceAndOutputScope, outputExpressions);
        List<Expr> orderByExpressions =
                orderByElements.stream().map(OrderByElement::getExpr).collect(Collectors.toList());

        analyzeGroupingOperations(analyzeState, groupByClause, outputExpressions);

        List<Expr> sourceExpressions = new ArrayList<>(outputExpressions);
        if (havingClause != null) {
            sourceExpressions.add(analyzeState.getHaving());
        }

        List<FunctionCallExpr> aggregates = analyzeAggregations(analyzeState, sourceScope,
                Stream.concat(sourceExpressions.stream(), orderByExpressions.stream()).collect(Collectors.toList()));
        if (AnalyzerUtils.isAggregate(aggregates, groupByExpressions)) {
            if (!groupByExpressions.isEmpty() &&
                    selectList.getItems().stream().anyMatch(SelectListItem::isStar) &&
                    !selectList.isDistinct()) {
                throw new SemanticException("cannot combine '*' in select list with GROUP BY: *");
            }

            verifySourceAggregations(groupByExpressions, sourceExpressions, sourceScope, analyzeState);

            if (orderByElements.size() > 0) {
                verifyOrderByAggregations(groupByExpressions, orderByExpressions, sourceScope, sourceAndOutputScope,
                        analyzeState);
            }
        }

        analyzeWindowFunctions(analyzeState, outputExpressions, orderByExpressions);

        if (AnalyzerUtils.isAggregate(aggregates, groupByExpressions) &&
                (sortClause != null || havingClause != null)) {
            /*
             * Create scope for order by when aggregation is present.
             * This is because transformer requires scope in order to resolve names against fields.
             * Original ORDER BY see source scope. However, if aggregation is present,
             * ORDER BY  expressions should only be resolvable against output scope,
             * group by expressions and aggregation expressions.
             */
            List<FunctionCallExpr> aggregationsInOrderBy = Lists.newArrayList();
            TreeNode.collect(orderByExpressions, Expr.isAggregatePredicate(), aggregationsInOrderBy);

            /*
             * Prohibit the use of aggregate sorting for non-aggregated query,
             * To prevent the generation of incorrect data during non-scalar aggregation (at least 1 row in no-scalar agg)
             * eg. select 1 from t0 order by sum(v)
             */
            List<FunctionCallExpr> aggregationsInOutput = Lists.newArrayList();
            TreeNode.collect(sourceExpressions, Expr.isAggregatePredicate(), aggregationsInOutput);
            if (!AnalyzerUtils.isAggregate(aggregationsInOutput, groupByExpressions) &&
                    !aggregationsInOrderBy.isEmpty()) {
                throw new SemanticException(
                        "ORDER BY contains aggregate function and applies to the result of a non-aggregated query");
            }

            List<Expr> orderSourceExpressions = Streams.concat(
                    aggregationsInOrderBy.stream(),
                    groupByExpressions.stream()).collect(Collectors.toList());

            List<Field> sourceForOrderFields = orderSourceExpressions.stream()
                    .map(expression ->
                            new Field("anonymous", expression.getType(), null, expression))
                    .collect(Collectors.toList());

            Scope sourceScopeForOrder = new Scope(RelationId.anonymous(), new RelationFields(sourceForOrderFields));
            sourceAndOutputScope = new Scope(outputScope.getRelationId(), outputScope.getRelationFields());
            sourceAndOutputScope.setParent(sourceScopeForOrder);
            analyzeState.setOrderScope(sourceAndOutputScope);
            analyzeState.setOrderSourceExpressions(orderSourceExpressions);
        }

        if (limitElement != null && limitElement.hasLimit()) {
            if (limitElement.getOffset() > 0 && orderByElements.isEmpty()) {
                // The offset can only be processed in sort,
                // so when there is no order by, we manually set offset to 0
                analyzeState.setLimit(new LimitElement(0, limitElement.getLimit()));
            } else {
                analyzeState.setLimit(new LimitElement(limitElement.getOffset(), limitElement.getLimit()));
            }
        }
    }

    private List<Expr> analyzeSelect(SelectList selectList, Relation fromRelation, boolean hasGroupByClause,
                                     AnalyzeState analyzeState, Scope scope) {
        ImmutableList.Builder<Expr> outputExpressionBuilder = ImmutableList.builder();
        List<String> columnOutputNames = new ArrayList<>();

        for (SelectListItem item : selectList.getItems()) {
            if (item.isStar()) {
                List<Field> fields = item.getTblName() == null ? scope.getRelationFields().getAllFields()
                        : scope.getRelationFields().resolveFieldsWithPrefix(item.getTblName());
                if (fields.isEmpty()) {
                    if (item.getTblName() != null) {
                        throw new SemanticException("Table %s not found", item.getTblName());
                    }
                    if (fromRelation != null) {
                        throw new SemanticException("SELECT * not allowed in queries without FROM clause");
                    }
                    throw new StarRocksPlannerException("SELECT * not allowed from relation that has no columns",
                            INTERNAL_ERROR);
                }

                columnOutputNames.addAll(expandStar(item, fromRelation).stream().map(AST2SQL::toString)
                        .collect(Collectors.toList()));

                for (Field field : fields) {
                    //shadow column is not visible
                    if (!field.isVisible()) {
                        continue;
                    }

                    int fieldIndex = scope.getRelationFields().indexOf(field);
                    /*
                     * Generate a special "SlotRef" as FieldReference,
                     * which represents a reference to the expression in the source scope.
                     * Because the real expression cannot be obtained in star
                     * eg: "select * from (select count(*) from table) t"
                     */
                    FieldReference fieldReference = new FieldReference(fieldIndex, item.getTblName());
                    analyzeExpression(fieldReference, analyzeState, scope);
                    outputExpressionBuilder.add(fieldReference);
                }
            } else {
                if (item.getAlias() != null) {
                    columnOutputNames.add(item.getAlias());
                } else {
                    columnOutputNames.add(AST2SQL.toString(item.getExpr()));
                }

                analyzeExpression(item.getExpr(), analyzeState, scope);
                outputExpressionBuilder.add(item.getExpr());
            }

            if (selectList.isDistinct()) {
                outputExpressionBuilder.build().forEach(expr -> {
                    if (!expr.getType().canDistinct()) {
                        throw new SemanticException("DISTINCT can only be applied to comparable types : %s",
                                expr.getType());
                    }
                    if (expr.isAggregate()) {
                        throw new SemanticException(
                                "cannot combine SELECT DISTINCT with aggregate functions or GROUP BY");
                    }
                });

                if (hasGroupByClause) {
                    throw new SemanticException("cannot combine SELECT DISTINCT with aggregate functions or GROUP BY");
                }
                analyzeState.setIsDistinct(true);
            }
        }

        List<Expr> outputExpr = outputExpressionBuilder.build();
        Preconditions.checkArgument(outputExpr.size() == columnOutputNames.size());
        analyzeState.setOutputExpression(outputExpr);
        analyzeState.setColumnOutputNames(columnOutputNames);
        return outputExpressionBuilder.build();
    }

    private List<OrderByElement> analyzeOrderBy(List<OrderByElement> orderByElements, AnalyzeState analyzeState,
                                                Scope orderByScope,
                                                List<Expr> outputExpressions) {
        if (orderByElements == null) {
            analyzeState.setOrderBy(Collections.emptyList());
            return Collections.emptyList();
        }

        for (OrderByElement orderByElement : orderByElements) {
            Expr expression = orderByElement.getExpr();
            AnalyzerUtils.verifyNoGroupingFunctions(expression, "ORDER BY");

            if (expression instanceof IntLiteral) {
                long ordinal = ((IntLiteral) expression).getLongValue();
                if (ordinal < 1 || ordinal > outputExpressions.size()) {
                    throw new SemanticException("ORDER BY position %s is not in select list", ordinal);
                }
                expression = outputExpressions.get((int) ordinal - 1);
            }

            analyzeExpression(expression, analyzeState, orderByScope);

            if (!expression.getType().canOrderBy()) {
                throw new SemanticException(Type.OnlyMetricTypeErrorMsg);
            }

            orderByElement.setExpr(expression);
        }

        analyzeState.setOrderBy(orderByElements);
        return orderByElements;
    }

    private void analyzeWindowFunctions(AnalyzeState analyzeState, List<Expr> outputExpressions,
                                        List<Expr> orderByExpressions) {
        List<AnalyticExpr> outputWindowFunctions = new ArrayList<>();
        for (Expr expression : outputExpressions) {
            List<AnalyticExpr> window = Lists.newArrayList();
            expression.collect(AnalyticExpr.class, window);
            if (outputWindowFunctions.stream()
                    .anyMatch((e -> TreeNode.contains(e.getChildren(), AnalyticExpr.class)))) {
                throw new SemanticException("Nesting of analytic expressions is not allowed: " + expression.toSql());
            }
            outputWindowFunctions.addAll(window);
        }
        analyzeState.setOutputAnalytic(outputWindowFunctions);

        List<AnalyticExpr> orderByWindowFunctions = new ArrayList<>();
        for (Expr expression : orderByExpressions) {
            List<AnalyticExpr> window = Lists.newArrayList();
            expression.collect(AnalyticExpr.class, window);
            if (orderByWindowFunctions.stream()
                    .anyMatch((e -> TreeNode.contains(e.getChildren(), AnalyticExpr.class)))) {
                throw new SemanticException("Nesting of analytic expressions is not allowed: " + expression.toSql());
            }
            orderByWindowFunctions.addAll(window);
        }
        analyzeState.setOrderByAnalytic(orderByWindowFunctions);
    }

    private void analyzeWhere(Expr whereClause, AnalyzeState analyzeState, Scope scope) {
        if (whereClause == null) {
            return;
        }

        Expr predicate = pushNegationToOperands(whereClause);
        analyzeExpression(predicate, analyzeState, scope);

        AnalyzerUtils.verifyNoAggregateFunctions(predicate, "WHERE");
        AnalyzerUtils.verifyNoWindowFunctions(predicate, "WHERE");
        AnalyzerUtils.verifyNoGroupingFunctions(predicate, "WHERE");

        if (!predicate.getType().matchesType(Type.BOOLEAN) && !predicate.getType().matchesType(Type.NULL)) {
            throw new SemanticException("WHERE clause must evaluate to a boolean: actual type %s", predicate.getType());
        }

        analyzeState.setPredicate(predicate);
    }

    private void analyzeGroupingOperations(AnalyzeState analyzeState, GroupByClause groupByClause,
                                           List<Expr> outputExpressions) {
        List<Expr> groupingFunctionCallExprs = Lists.newArrayList();

        TreeNode.collect(outputExpressions, expr -> expr instanceof GroupingFunctionCallExpr,
                groupingFunctionCallExprs);

        if (!groupingFunctionCallExprs.isEmpty() &&
                (groupByClause == null ||
                        groupByClause.getGroupingType().equals(GroupByClause.GroupingType.GROUP_BY))) {
            throw new SemanticException("cannot use GROUPING functions without [grouping sets|rollup|cube] clause");
        }

        analyzeState.setGroupingFunctionCallExprs(groupingFunctionCallExprs);
    }

    private List<FunctionCallExpr> analyzeAggregations(AnalyzeState analyzeState, Scope sourceScope,
                                                       List<Expr> outputAndOrderByExpressions) {
        List<FunctionCallExpr> aggregations = Lists.newArrayList();
        TreeNode.collect(outputAndOrderByExpressions, Expr.isAggregatePredicate()::apply, aggregations);
        aggregations.forEach(e -> analyzeExpression(e, analyzeState, sourceScope));

        if (aggregations.stream().filter(FunctionCallExpr::isDistinct).count() > 1) {
            for (FunctionCallExpr agg : aggregations) {
                if (agg.isDistinct() && agg.getChildren().size() > 0 && agg.getChild(0).getType().isArrayType()) {
                    throw new SemanticException("No matching function with signature: multi_distinct_count(ARRAY)");
                }
            }
        }

        analyzeState.setAggregate(aggregations);

        return aggregations;
    }

    private List<Expr> analyzeGroupBy(GroupByClause groupByClause, AnalyzeState analyzeState, Scope sourceScope,
                                      Scope outputScope, List<Expr> outputExpressions) {
        List<Expr> groupByExpressions = new ArrayList<>();
        if (groupByClause != null) {
            if (groupByClause.getGroupingType() == GroupByClause.GroupingType.GROUP_BY) {
                List<Expr> groupingExprs = groupByClause.getGroupingExprs();
                for (Expr groupingExpr : groupingExprs) {
                    if (groupingExpr instanceof IntLiteral) {
                        long ordinal = ((IntLiteral) groupingExpr).getLongValue();
                        if (ordinal < 1 || ordinal > outputExpressions.size()) {
                            throw new SemanticException("Group by position %s is not in select list", ordinal);
                        }
                        groupingExpr = outputExpressions.get((int) ordinal - 1);
                    } else {
                        RewriteAliasVisitor visitor =
                                new RewriteAliasVisitor(sourceScope, outputScope, outputExpressions, session);
                        groupingExpr = groupingExpr.accept(visitor, null);
                        analyzeExpression(groupingExpr, analyzeState, sourceScope);
                    }

                    if (!groupingExpr.getType().canGroupBy()) {
                        throw new SemanticException(Type.OnlyMetricTypeErrorMsg);
                    }

                    if (analyzeState.getColumnReferences().get(groupingExpr) == null) {
                        AnalyzerUtils.verifyNoAggregateFunctions(groupingExpr, "GROUP BY");
                        AnalyzerUtils.verifyNoWindowFunctions(groupingExpr, "GROUP BY");
                        AnalyzerUtils.verifyNoGroupingFunctions(groupingExpr, "GROUP BY");
                    }

                    groupByExpressions.add(groupingExpr);
                }
            } else {
                if (groupByClause.getGroupingType().equals(GroupByClause.GroupingType.GROUPING_SETS)) {
                    List<List<Expr>> groupingSets = new ArrayList<>();
                    ArrayList<Expr> groupByList = Lists.newArrayList();
                    for (ArrayList<Expr> g : groupByClause.getGroupingSetList()) {
                        List<Expr> rewriteGrouping = rewriteGroupByAlias(g, analyzeState,
                                sourceScope, outputScope, outputExpressions);
                        rewriteGrouping.forEach(e -> {
                            if (!groupByList.contains(e)) {
                                groupByList.add(e);
                            }
                        });
                        groupingSets.add(rewriteGrouping);
                    }

                    groupByExpressions.addAll(groupByList);
                    analyzeState.setGroupingSetsList(groupingSets);
                } else if (groupByClause.getGroupingType().equals(GroupByClause.GroupingType.CUBE)) {
                    groupByExpressions.addAll(rewriteGroupByAlias(groupByClause.getGroupingExprs(), analyzeState,
                            sourceScope, outputScope, outputExpressions));
                    List<Expr> rewriteOriGrouping =
                            rewriteGroupByAlias(groupByClause.getOriGroupingExprs(), analyzeState,
                                    sourceScope, outputScope, outputExpressions);

                    List<List<Expr>> groupingSets =
                            Sets.powerSet(IntStream.range(0, rewriteOriGrouping.size())
                                    .boxed().collect(Collectors.toSet())).stream()
                                    .map(l -> l.stream().map(rewriteOriGrouping::get).collect(Collectors.toList()))
                                    .collect(Collectors.toList());

                    analyzeState.setGroupingSetsList(groupingSets);
                } else if (groupByClause.getGroupingType().equals(GroupByClause.GroupingType.ROLLUP)) {
                    groupByExpressions.addAll(rewriteGroupByAlias(groupByClause.getGroupingExprs(), analyzeState,
                            sourceScope, outputScope, outputExpressions));
                    List<Expr> rewriteOriGrouping =
                            rewriteGroupByAlias(groupByClause.getOriGroupingExprs(), analyzeState, sourceScope,
                                    outputScope, outputExpressions);

                    List<List<Expr>> groupingSets = IntStream.rangeClosed(0, rewriteOriGrouping.size())
                            .mapToObj(i -> rewriteOriGrouping.subList(0, i)).collect(Collectors.toList());

                    analyzeState.setGroupingSetsList(groupingSets);
                } else {
                    throw new StarRocksPlannerException("unknown grouping type", INTERNAL_ERROR);
                }
            }
        }
        analyzeState.setGroupBy(groupByExpressions);
        return groupByExpressions;
    }

    private List<Expr> rewriteGroupByAlias(List<Expr> groupingExprs, AnalyzeState analyzeState, Scope sourceScope,
                                           Scope outputScope, List<Expr> outputExpressions) {
        return groupingExprs.stream().map(e -> {
            RewriteAliasVisitor visitor =
                    new RewriteAliasVisitor(sourceScope, outputScope, outputExpressions, session);
            Expr rewrite = e.accept(visitor, null);
            analyzeExpression(rewrite, analyzeState, sourceScope);
            return rewrite;
        }).collect(Collectors.toList());
    }

    private void analyzeHaving(Expr havingClause, AnalyzeState analyzeState,
                               Scope sourceScope, Scope outputScope, List<Expr> outputExprs) {
        if (havingClause != null) {
            Expr predicate = pushNegationToOperands(havingClause);

            AnalyzerUtils.verifyNoWindowFunctions(predicate, "HAVING");
            AnalyzerUtils.verifyNoGroupingFunctions(predicate, "HAVING");

            RewriteAliasVisitor visitor = new RewriteAliasVisitor(sourceScope, outputScope, outputExprs, session);
            predicate = predicate.accept(visitor, null);
            analyzeExpression(predicate, analyzeState, sourceScope);

            if (!predicate.getType().matchesType(Type.BOOLEAN) && !predicate.getType().matchesType(Type.NULL)) {
                throw new SemanticException("HAVING clause must evaluate to a boolean: actual type %s",
                        predicate.getType());
            }
            analyzeState.setHaving(predicate);
        }
    }

    // If alias is same with table column name, we directly use table name.
    // otherwise, we use output expression according to the alias
    private static class RewriteAliasVisitor extends AstVisitor<Expr, Void> {
        private final Scope sourceScope;
        private final Scope outputScope;
        private final List<Expr> outputExprs;
        private final ConnectContext session;

        public RewriteAliasVisitor(Scope sourceScope, Scope outputScope, List<Expr> outputExprs,
                                   ConnectContext session) {
            this.sourceScope = sourceScope;
            this.outputScope = outputScope;
            this.outputExprs = outputExprs;
            this.session = session;
        }

        @Override
        public Expr visit(ParseNode expr) {
            return visit(expr, null);
        }

        @Override
        public Expr visitExpression(Expr expr, Void context) {
            for (int i = 0; i < expr.getChildren().size(); ++i) {
                expr.setChild(i, visit(expr.getChild(i)));
            }
            return expr;
        }

        @Override
        public Expr visitSlot(SlotRef slotRef, Void context) {
            if (sourceScope.tryResolveFeild(slotRef).isPresent() &&
                    !session.getSessionVariable().getEnableGroupbyUseOutputAlias()) {
                return slotRef;
            }

            Optional<ResolvedField> resolvedField = outputScope.tryResolveFeild(slotRef);
            if (resolvedField.isPresent()) {
                return outputExprs.get(resolvedField.get().getRelationFieldIndex());
            }
            return slotRef;
        }
    }

    private Scope computeAndAssignOrderScope(AnalyzeState analyzeState, Scope sourceScope, Scope outputScope) {
        // The Scope used by order by allows parsing of the same column,
        // such as 'select v1 as v, v1 as v from t0 order by v'
        // but normal parsing does not allow it. So add a de-duplication operation here.
        List<Field> allFields = new ArrayList<>();
        for (Field field : outputScope.getRelationFields().getAllFields()) {
            if (field.getName() != null && field.getOriginExpression() != null &&
                    allFields.stream().anyMatch(f ->
                            f.getOriginExpression() != null &&
                                    f.getName() != null &&
                                    field.getName().equals(f.getName()) &&
                                    field.getOriginExpression().equals(f.getOriginExpression()))) {
                continue;
            }
            allFields.add(field);
        }

        Scope orderScope = new Scope(outputScope.getRelationId(), new RelationFields(allFields));
        /*
         * ORDER BY or HAVING should "see" both output and FROM fields
         * Because output scope and source scope may contain the same columns,
         * so they cannot be in the same level of scope to avoid ambiguous semantics
         */
        orderScope.setParent(sourceScope);
        analyzeState.setOrderScope(orderScope);
        return orderScope;
    }

    private Scope computeAndAssignOutputScope(SelectList selectList, AnalyzeState analyzeState, Scope scope) {
        ImmutableList.Builder<Field> outputFields = ImmutableList.builder();

        for (SelectListItem item : selectList.getItems()) {
            if (item.isStar()) {
                if (item.getTblName() == null) {
                    outputFields.addAll(scope.getRelationFields().getAllFields()
                            .stream().filter(Field::isVisible)
                            .map(f -> new Field(f.getName(), f.getType(), f.getRelationAlias(),
                                    f.getOriginExpression(), f.isVisible())).collect(Collectors.toList()));
                } else {
                    outputFields.addAll(scope.getRelationFields().resolveFieldsWithPrefix(item.getTblName())
                            .stream().filter(Field::isVisible)
                            .map(f -> new Field(f.getName(), f.getType(), f.getRelationAlias(),
                                    f.getOriginExpression(), f.isVisible())).collect(Collectors.toList()));
                }
            } else {
                String name;
                TableName relationAlias = null;
                if (item.getAlias() != null) {
                    name = item.getAlias();
                } else {
                    name = AST2SQL.toString(item.getExpr());
                }

                outputFields.add(new Field(name, item.getExpr().getType(), relationAlias, item.getExpr()));
            }
        }
        Scope outputScope = new Scope(RelationId.anonymous(), new RelationFields(outputFields.build()));

        analyzeState.setOutputScope(outputScope);
        return outputScope;
    }

    private void analyzeExpression(Expr expr, AnalyzeState analyzeState, Scope scope) {
        ExpressionAnalyzer.analyzeExpression(expr, analyzeState, scope, catalog, session);
    }

    public static List<Expr> expandStar(SelectListItem item, Relation fromRelation) {
        List<Expr> outputExpressions = Lists.newArrayList();
        outputExpressions.addAll(new AstVisitor<List<Expr>, Void>() {
            @Override
            public List<Expr> visitCTE(CTERelation node, Void context) {
                if (item.getTblName() == null || item.getTblName().getTbl().equals(node.getAlias().getTbl())) {
                    List<Expr> outputExpressions = new ArrayList<>();
                    for (String outputName : node.getColumnOutputNames()) {
                        outputExpressions.add(new SlotRef(node.getAlias(), outputName, outputName));
                    }
                    return outputExpressions;
                } else {
                    return new ArrayList<>();
                }
            }

            @Override
            public List<Expr> visitJoin(JoinRelation node, Void context) {
                if (node.getType().isLeftSemiAntiJoin()) {
                    return visit(node.getLeft());
                } else if (node.getType().isRightSemiAntiJoin()) {
                    return visit(node.getRight());
                } else {
                    return Streams.concat(visit(node.getLeft()).stream(), visit(node.getRight()).stream())
                            .collect(Collectors.toList());
                }
            }

            @Override
            public List<Expr> visitSelect(SelectRelation node, Void context) {
                List<Expr> outputExpression = Lists.newArrayList();
                for (SelectListItem item : node.getSelectList().getItems()) {
                    if (item.isStar()) {
                        outputExpression.addAll(expandStar(item, node.getRelation()));
                    } else {
                        outputExpression.add(item.getExpr());
                    }
                }
                return outputExpression;
            }

            @Override
            public List<Expr> visitSetOp(SetOperationRelation node, Void context) {
                return node.getOutputExpression();
            }

            @Override
            public List<Expr> visitSubquery(SubqueryRelation node, Void context) {
                if (item.getTblName() == null || item.getTblName().getTbl().equals(node.getAlias().getTbl())) {
                    List<Expr> outputExpressions = new ArrayList<>();
                    for (String outputName : node.getQuery().getColumnOutputNames()) {
                        outputExpressions.add(new SlotRef(node.getAlias(), outputName, outputName));
                    }
                    return outputExpressions;
                } else {
                    return new ArrayList<>();
                }
            }

            @Override
            public List<Expr> visitView(ViewRelation node, Void context) {
                if (item.getTblName() == null || item.getTblName().getTbl().equals(node.getAlias().getTbl())) {
                    List<Expr> outputExpressions = new ArrayList<>();
                    for (Column column : node.getView().getBaseSchema()) {
                        outputExpressions.add(new SlotRef(node.getAlias(), column.getName(), column.getName()));
                    }

                    return outputExpressions;
                } else {
                    return new ArrayList<>();
                }
            }

            @Override
            public List<Expr> visitTable(TableRelation node, Void context) {
                if (item.getTblName() == null) {
                    List<Expr> outputExpressions = new ArrayList<>();
                    for (Column column : node.getTable().getBaseSchema()) {
                        outputExpressions.add(new SlotRef(node.getAlias(), column.getName(), column.getName()));
                    }
                    return outputExpressions;
                } else {
                    if (!item.getTblName().getTbl().equals(node.getAlias().getTbl())) {
                        return new ArrayList<>();
                    } else {
                        List<Expr> outputExpressions = new ArrayList<>();
                        for (Column column : node.getTable().getBaseSchema()) {
                            outputExpressions.add(new SlotRef(node.getAlias(), column.getName(), column.getName()));
                        }
                        return outputExpressions;
                    }
                }
            }

            @Override
            public List<Expr> visitTableFunction(TableFunctionRelation node, Void context) {
                List<Expr> outputExpressions = Lists.newArrayList();
                for (int i = 0; i < node.getTableFunction().getDefaultColumnNames().size(); ++i) {
                    outputExpressions.add(new SlotRef(node.getAlias(),
                            node.getTableFunction().getDefaultColumnNames().get(i),
                            node.getTableFunction().getDefaultColumnNames().get(i))
                    );
                }
                return outputExpressions;
            }

            @Override
            public List<Expr> visitValues(ValuesRelation node, Void context) {
                return node.getRows().get(0);
            }
        }.visit(fromRelation));
        return outputExpressions;
    }
}
