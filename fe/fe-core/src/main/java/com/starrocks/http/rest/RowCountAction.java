// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/http/rest/RowCountAction.java

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

package com.starrocks.http.rest;

import com.google.common.base.Strings;
import com.google.common.collect.Maps;
import com.starrocks.catalog.Catalog;
import com.starrocks.catalog.Database;
import com.starrocks.catalog.MaterializedIndex;
import com.starrocks.catalog.MaterializedIndex.IndexExtState;
import com.starrocks.catalog.OlapTable;
import com.starrocks.catalog.Partition;
import com.starrocks.catalog.Replica;
import com.starrocks.catalog.Table;
import com.starrocks.catalog.Table.TableType;
import com.starrocks.catalog.Tablet;
import com.starrocks.common.DdlException;
import com.starrocks.http.ActionController;
import com.starrocks.http.BaseRequest;
import com.starrocks.http.BaseResponse;
import com.starrocks.http.IllegalArgException;
import com.starrocks.mysql.privilege.PrivPredicate;
import com.starrocks.qe.ConnectContext;
import io.netty.handler.codec.http.HttpMethod;
import org.codehaus.jackson.map.ObjectMapper;

import java.util.Map;

/*
 * calc row count from replica to table
 * fe_host:fe_http_port/api/rowcount?db=dbname&table=tablename
 */
public class RowCountAction extends RestBaseAction {

    public RowCountAction(ActionController controller) {
        super(controller);
    }

    public static void registerAction(ActionController controller) throws IllegalArgException {
        controller.registerHandler(HttpMethod.GET, "/api/rowcount", new RowCountAction(controller));
    }

    @Override
    protected void executeWithoutPassword(BaseRequest request, BaseResponse response) throws DdlException {
        checkGlobalAuth(ConnectContext.get().getCurrentUserIdentity(), PrivPredicate.ADMIN);

        String dbName = request.getSingleParameter(DB_KEY);
        if (Strings.isNullOrEmpty(dbName)) {
            throw new DdlException("No database selected.");
        }

        String tableName = request.getSingleParameter(TABLE_KEY);
        if (Strings.isNullOrEmpty(tableName)) {
            throw new DdlException("No table selected.");
        }

        Map<String, Long> indexRowCountMap = Maps.newHashMap();
        Catalog catalog = Catalog.getCurrentCatalog();
        Database db = catalog.getDb(dbName);
        if (db == null) {
            throw new DdlException("Database[" + dbName + "] does not exist");
        }
        db.writeLock();
        try {
            Table table = db.getTable(tableName);
            if (table == null) {
                throw new DdlException("Table[" + tableName + "] does not exist");
            }

            if (table.getType() != TableType.OLAP) {
                throw new DdlException("Table[" + tableName + "] is not OLAP table");
            }

            OlapTable olapTable = (OlapTable) table;
            for (Partition partition : olapTable.getAllPartitions()) {
                long version = partition.getVisibleVersion();
                for (MaterializedIndex index : partition.getMaterializedIndices(IndexExtState.VISIBLE)) {
                    long indexRowCount = 0L;
                    for (Tablet tablet : index.getTablets()) {
                        long tabletRowCount = 0L;
                        for (Replica replica : tablet.getReplicas()) {
                            if (replica.checkVersionCatchUp(version, false)
                                    && replica.getRowCount() > tabletRowCount) {
                                tabletRowCount = replica.getRowCount();
                            }
                        }
                        indexRowCount += tabletRowCount;
                    } // end for tablets
                    index.setRowCount(indexRowCount);
                    String indexName = olapTable.getIndexNameById(index.getId());
                    indexRowCountMap.put(indexName, indexRowCountMap.getOrDefault(indexName, 0L) + indexRowCount);
                } // end for indices
            } // end for partitions            
        } finally {
            db.writeUnlock();
        }

        // to json response
        String result = "";
        ObjectMapper mapper = new ObjectMapper();
        try {
            result = mapper.writeValueAsString(indexRowCountMap);
        } catch (Exception e) {
            //  do nothing
        }

        // send result
        response.setContentType("application/json");
        response.getContent().append(result);
        sendResult(request, response);
    }
}
