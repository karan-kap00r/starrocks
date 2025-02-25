// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/analysis/LikePredicate.java

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package com.starrocks.analysis;

import com.google.common.base.Preconditions;
import com.starrocks.catalog.Function;
import com.starrocks.common.AnalysisException;
import com.starrocks.sql.ast.AstVisitor;
import com.starrocks.sql.analyzer.SemanticException;
import com.starrocks.thrift.TExprNode;
import com.starrocks.thrift.TExprNodeType;

import java.util.Objects;
import java.util.regex.Pattern;
import java.util.regex.PatternSyntaxException;

// Our new cost based query optimizer is more powerful and stable than old query optimizer,
// The old query optimizer related codes could be deleted safely.
// TODO: Remove old query optimizer related codes before 2021-09-30
public class LikePredicate extends Predicate {

    public enum Operator {
        LIKE("LIKE"),
        REGEXP("REGEXP");

        private final String description;

        Operator(String description) {
            this.description = description;
        }

        @Override
        public String toString() {
            return description;
        }
    }

    private final Operator op;

    public LikePredicate(Operator op, Expr e1, Expr e2) {
        super();
        this.op = op;
        Preconditions.checkNotNull(e1);
        children.add(e1);
        Preconditions.checkNotNull(e2);
        children.add(e2);
        // TODO: improve with histograms?
        selectivity = 0.1;
    }

    protected LikePredicate(LikePredicate other) {
        super(other);
        op = other.op;
    }

    @Override
    public Expr clone() {
        return new LikePredicate(this);
    }

    public Operator getOp() {
        return this.op;
    }

    @Override
    public boolean equals(Object obj) {
        if (!super.equals(obj)) {
            return false;
        }
        return ((LikePredicate) obj).op == op;
    }

    @Override
    public String toSqlImpl() {
        return getChild(0).toSql() + " " + op.toString() + " " + getChild(1).toSql();
    }

    @Override
    public String toDigestImpl() {
        return getChild(0).toDigest() + " " + op.toString().toLowerCase() + " " + getChild(1).toDigest();
    }


    @Override
    protected void toThrift(TExprNode msg) {
        msg.node_type = TExprNodeType.FUNCTION_CALL;
    }

    @Override
    public void analyzeImpl(Analyzer analyzer) throws AnalysisException {
        super.analyzeImpl(analyzer);
        if (!getChild(0).getType().isStringType() && !getChild(0).getType().isNull()) {
            throw new AnalysisException(
                    "left operand of " + op.toString() + " must be of type STRING: " + toSql());
        }
        if (!getChild(1).getType().isStringType() && !getChild(1).getType().isNull()) {
            throw new AnalysisException(
                    "right operand of " + op.toString() + " must be of type STRING: " + toSql());
        }

        fn = getBuiltinFunction(analyzer, op.toString(),
                collectChildReturnTypes(), Function.CompareMode.IS_NONSTRICT_SUPERTYPE_OF);
        if (!getChild(1).getType().isNull() && getChild(1).isLiteral() && (op == Operator.REGEXP)) {
            // let's make sure the pattern works
            // TODO: this checks that it's a Java-supported regex, but the syntax supported
            // by the backend is Posix; add a call to the backend to check the re syntax
            try {
                Pattern.compile(((StringLiteral) getChild(1)).getValue());
            } catch (PatternSyntaxException e) {
                throw new AnalysisException("Invalid regular expression in '" + this.toSql() + "'");
            }
        }
    }

    @Override
    public int hashCode() {
        return 31 * super.hashCode() + Objects.hashCode(op);
    }

    @Override
    public <R, C> R accept(AstVisitor<R, C> visitor, C context) throws SemanticException {
        return visitor.visitLikePredicate(this, context);
    }
}
