// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/analysis/DeleteStmt.java

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

import com.google.common.base.Joiner;
import com.google.common.collect.Lists;
import com.starrocks.analysis.CompoundPredicate.Operator;
import com.starrocks.catalog.Catalog;
import com.starrocks.common.AnalysisException;
import com.starrocks.common.Config;
import com.starrocks.common.ErrorCode;
import com.starrocks.common.ErrorReport;
import com.starrocks.common.UserException;
import com.starrocks.mysql.privilege.PrivPredicate;
import com.starrocks.qe.ConnectContext;

import java.util.LinkedList;
import java.util.List;

public class DeleteStmt extends DdlStmt {
    private final TableName tbl;
    private final PartitionNames partitionNames;
    private final Expr wherePredicate;

    private final List<Predicate> deleteConditions;
    // Each deleteStmt corresponds to a DeleteJob.
    // The JobID is generated here for easy correlation when cancel Delete
    private long jobId = -1;

    public DeleteStmt(TableName tableName, PartitionNames partitionNames, Expr wherePredicate) {
        this.tbl = tableName;
        this.partitionNames = partitionNames;
        this.wherePredicate = wherePredicate;
        this.deleteConditions = Lists.newLinkedList();
    }

    public long getJobId() {
        return jobId;
    }

    public void setJobId(long jobId) {
        this.jobId = jobId;
    }

    public String getTableName() {
        return tbl.getTbl();
    }

    public String getDbName() {
        return tbl.getDb();
    }

    public List<String> getPartitionNames() {
        return partitionNames == null ? Lists.newArrayList() : partitionNames.getPartitionNames();
    }

    public List<Predicate> getDeleteConditions() {
        return deleteConditions;
    }

    @Override
    public void analyze(Analyzer analyzer) throws UserException {
        super.analyze(analyzer);

        if (tbl == null) {
            throw new AnalysisException("Table is not set");
        }

        tbl.analyze(analyzer);

        if (partitionNames != null) {
            partitionNames.analyze(analyzer);
            if (partitionNames.isTemp()) {
                throw new AnalysisException("Do not support deleting temp partitions");
            }
        }

        if (wherePredicate == null) {
            throw new AnalysisException("Where clause is not set");
        }

        // analyze predicate
        analyzePredicate(wherePredicate);

        // check access
        if (!Catalog.getCurrentCatalog().getAuth().checkTblPriv(ConnectContext.get(), tbl.getDb(), tbl.getTbl(),
                PrivPredicate.LOAD)) {
            ErrorReport.reportAnalysisException(ErrorCode.ERR_TABLEACCESS_DENIED_ERROR, "LOAD",
                    ConnectContext.get().getQualifiedUser(),
                    ConnectContext.get().getRemoteIP(), tbl.getTbl());
        }
    }

    private void analyzePredicate(Expr predicate) throws AnalysisException {
        if (predicate instanceof BinaryPredicate) {
            BinaryPredicate binaryPredicate = (BinaryPredicate) predicate;
            Expr leftExpr = binaryPredicate.getChild(0);
            if (!(leftExpr instanceof SlotRef)) {
                throw new AnalysisException("Left expr of binary predicate should be column name");
            }
            Expr rightExpr = binaryPredicate.getChild(1);
            if (!(rightExpr instanceof LiteralExpr)) {
                throw new AnalysisException("Right expr of binary predicate should be value");
            }
            deleteConditions.add(binaryPredicate);
        } else if (predicate instanceof CompoundPredicate) {
            CompoundPredicate compoundPredicate = (CompoundPredicate) predicate;
            if (compoundPredicate.getOp() != Operator.AND) {
                throw new AnalysisException("Compound predicate's op should be AND");
            }

            analyzePredicate(compoundPredicate.getChild(0));
            analyzePredicate(compoundPredicate.getChild(1));
        } else if (predicate instanceof IsNullPredicate) {
            IsNullPredicate isNullPredicate = (IsNullPredicate) predicate;
            Expr leftExpr = isNullPredicate.getChild(0);
            if (!(leftExpr instanceof SlotRef)) {
                throw new AnalysisException("Left expr of is_null predicate should be column name");
            }
            deleteConditions.add(isNullPredicate);
        } else if (predicate instanceof InPredicate) {
            InPredicate inPredicate = (InPredicate) predicate;
            Expr leftExpr = inPredicate.getChild(0);
            if (!(leftExpr instanceof SlotRef)) {
                throw new AnalysisException("Left expr of binary predicate should be column name");
            }
            int inElementNum = inPredicate.getInElementNum();
            int maxAllowedInElementNumOfDelete = Config.max_allowed_in_element_num_of_delete;
            if (inElementNum > maxAllowedInElementNumOfDelete) {
                throw new AnalysisException("Element num of predicate should not be more than " +
                        maxAllowedInElementNumOfDelete);
            }
            for (int i = 1; i <= inElementNum; i++) {
                Expr expr = inPredicate.getChild(i);
                if (!(expr instanceof LiteralExpr)) {
                    throw new AnalysisException("Child of in predicate should be value");
                }
            }
            deleteConditions.add(inPredicate);
        } else {
            throw new AnalysisException("Where clause only supports compound predicate, binary predicate, " +
                    "is_null predicate and in predicate");
        }
    }

    @Override
    public String toSql() {
        StringBuilder sb = new StringBuilder();
        sb.append("DELETE FROM ").append(tbl.toSql());
        if (partitionNames != null) {
            sb.append(" PARTITION (");
            sb.append(Joiner.on(", ").join(partitionNames.getPartitionNames()));
            sb.append(")");
        }
        sb.append(" WHERE ").append(wherePredicate.toSql());
        return sb.toString();
    }

}
