// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/test/java/org/apache/doris/analysis/ShowTableStmtTest.java

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

import com.starrocks.qe.ConnectContext;
import com.starrocks.sql.analyzer.SemanticException;
import com.starrocks.utframe.UtFrameUtils;
import org.junit.Assert;
import org.junit.Before;
import org.junit.Test;

public class ShowTableStmtTest {

    private ConnectContext ctx;

    @Before
    public void setUp() throws Exception {
    }

    @Test
    public void testNormal() throws Exception {
        ctx = UtFrameUtils.createDefaultCtx();
        ctx.setCluster("testCluster");
        ctx.setDatabase("testDb");

        ShowTableStmt stmt = new ShowTableStmt("", false, null);

        com.starrocks.sql.analyzer.Analyzer analyzer =
                new com.starrocks.sql.analyzer.Analyzer(ctx.getCatalog(), ctx);
        analyzer.analyze(stmt);
        Assert.assertEquals("SHOW TABLES FROM testCluster:testDb", stmt.toString());
        Assert.assertEquals("testCluster:testDb", stmt.getDb());
        Assert.assertFalse(stmt.isVerbose());
        Assert.assertEquals(1, stmt.getMetaData().getColumnCount());
        Assert.assertEquals("Tables_in_testDb", stmt.getMetaData().getColumn(0).getName());

        stmt = new ShowTableStmt("abc", true, null);
        analyzer.analyze(stmt);
        Assert.assertEquals("SHOW FULL TABLES FROM testCluster:abc", stmt.toString());
        Assert.assertEquals(2, stmt.getMetaData().getColumnCount());
        Assert.assertEquals("Tables_in_abc", stmt.getMetaData().getColumn(0).getName());
        Assert.assertEquals("Table_type", stmt.getMetaData().getColumn(1).getName());

        stmt = new ShowTableStmt("abc", true, "bcd");
        analyzer.analyze(stmt);
        Assert.assertEquals("bcd", stmt.getPattern());
        Assert.assertEquals("SHOW FULL TABLES FROM testCluster:abc LIKE 'bcd'", stmt.toString());
        Assert.assertEquals(2, stmt.getMetaData().getColumnCount());
        Assert.assertEquals("Tables_in_abc", stmt.getMetaData().getColumn(0).getName());
        Assert.assertEquals("Table_type", stmt.getMetaData().getColumn(1).getName());
    }

    @Test(expected = SemanticException.class)
    public void testNoDb() throws Exception {
        ctx = UtFrameUtils.createDefaultCtx();
        ShowTableStmt stmt = new ShowTableStmt("", false, null);
        com.starrocks.sql.analyzer.Analyzer analyzer =
                new com.starrocks.sql.analyzer.Analyzer(ctx.getCatalog(), ctx);
        analyzer.analyze(stmt);
        Assert.fail("No exception throws");
    }
}