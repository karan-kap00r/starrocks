// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.
package com.starrocks.sql.optimizer.operator.scalar;

import com.starrocks.catalog.Type;
import com.starrocks.sql.optimizer.base.ColumnRefSet;

import java.util.List;
import java.util.Objects;
import java.util.stream.Collectors;

import static com.starrocks.sql.optimizer.operator.OperatorType.ARRAY;

/**
 * ArrayOperator corresponds to ArrayExpr at the syntax level.
 * When Array is explicitly declared, ArrayExpr will be generated
 * eg. array[1,2,3]
 */
public class ArrayOperator extends ScalarOperator {
    private final Type type;
    private final boolean nullable;
    private final List<ScalarOperator> arguments;

    public ArrayOperator(Type type, boolean nullable, List<ScalarOperator> arguments) {
        super(ARRAY, type);
        this.type = type;
        this.nullable = nullable;
        this.arguments = arguments;
    }

    @Override
    public boolean isNullable() {
        return nullable;
    }

    @Override
    public List<ScalarOperator> getChildren() {
        return arguments;
    }

    @Override
    public ScalarOperator getChild(int index) {
        return arguments.get(index);
    }

    @Override
    public void setChild(int index, ScalarOperator child) {
        arguments.set(index, child);
    }

    @Override
    public String toString() {
        return arguments.stream().map(ScalarOperator::toString).collect(Collectors.joining(","));
    }

    @Override
    public ColumnRefSet getUsedColumns() {
        ColumnRefSet usedColumns = new ColumnRefSet();
        arguments.forEach(arg -> usedColumns.union(arg.getUsedColumns()));
        return usedColumns;
    }

    @Override
    public <R, C> R accept(ScalarOperatorVisitor<R, C> visitor, C context) {
        return visitor.visitArray(this, context);
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) {
            return true;
        }
        if (o == null || getClass() != o.getClass()) {
            return false;
        }
        ArrayOperator that = (ArrayOperator) o;
        return Objects.equals(type, that.type) && Objects.equals(arguments, that.arguments);
    }

    @Override
    public int hashCode() {
        return Objects.hash(type, arguments);
    }
}
