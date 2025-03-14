// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

package com.starrocks.catalog;

import com.google.common.collect.Maps;
import com.starrocks.analysis.AccessTestUtil;
import com.starrocks.analysis.Analyzer;
import com.starrocks.analysis.CreateResourceStmt;
import com.starrocks.common.UserException;
import com.starrocks.mysql.privilege.Auth;
import com.starrocks.mysql.privilege.PrivPredicate;
import com.starrocks.persist.gson.GsonUtils;
import com.starrocks.qe.ConnectContext;
import mockit.Expectations;
import mockit.Injectable;
import mockit.Mocked;
import org.junit.Assert;
import org.junit.Before;
import org.junit.Test;

import java.util.Map;

public class HiveResourceTest {
    private Analyzer analyzer;

    @Before
    public void setUp() {
        analyzer = AccessTestUtil.fetchAdminAnalyzer(true);
    }

    @Test
    public void testFromStmt(@Mocked Catalog catalog, @Injectable Auth auth) throws UserException {
        new Expectations() {
            {
                catalog.getAuth();
                result = auth;
                auth.checkGlobalPriv((ConnectContext) any, PrivPredicate.ADMIN);
                result = true;
            }
        };

        String name = "hive0";
        String type = "hive";
        String metastoreURIs = "thrift://127.0.0.1:9380";
        Map<String, String> properties = Maps.newHashMap();
        properties.put("type", type);
        properties.put("hive.metastore.uris", metastoreURIs);
        CreateResourceStmt stmt = new CreateResourceStmt(true, name, properties);
        stmt.analyze(analyzer);
        HiveResource resource = (HiveResource) Resource.fromStmt(stmt);
        Assert.assertEquals("hive0", resource.getName());
        Assert.assertEquals(type, resource.getType().name().toLowerCase());
        Assert.assertEquals(metastoreURIs, resource.getHiveMetastoreURIs());
    }

    @Test
    public void testSerialization() throws Exception {
        Resource resource = new HiveResource("hive0");
        String metastoreURIs = "thrift://127.0.0.1:9380";
        Map<String, String> properties = Maps.newHashMap();
        properties.put("hive.metastore.uris", metastoreURIs);
        resource.setProperties(properties);

        String json = GsonUtils.GSON.toJson(resource);
        Resource resource2 = GsonUtils.GSON.fromJson(json, Resource.class);
        Assert.assertTrue(resource2 instanceof HiveResource);
        Assert.assertEquals(metastoreURIs, ((HiveResource) resource2).getHiveMetastoreURIs());
    }
}
