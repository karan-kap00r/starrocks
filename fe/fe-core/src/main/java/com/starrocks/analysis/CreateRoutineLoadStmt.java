// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/analysis/CreateRoutineLoadStmt.java

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

import com.google.common.base.Strings;
import com.google.common.collect.ImmutableSet;
import com.google.common.collect.Lists;
import com.google.common.collect.Maps;
import com.starrocks.common.AnalysisException;
import com.starrocks.common.Config;
import com.starrocks.common.FeNameFormat;
import com.starrocks.common.Pair;
import com.starrocks.common.UserException;
import com.starrocks.common.util.TimeUtils;
import com.starrocks.common.util.Util;
import com.starrocks.load.RoutineLoadDesc;
import com.starrocks.load.routineload.KafkaProgress;
import com.starrocks.load.routineload.LoadDataSourceType;
import com.starrocks.load.routineload.RoutineLoadJob;
import com.starrocks.qe.ConnectContext;

import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.function.Predicate;
import java.util.regex.Pattern;

/*
 Create routine Load statement,  continually load data from a streaming app

 syntax:
      CREATE ROUTINE LOAD [database.]name on table
      [load properties]
      [PROPERTIES
      (
          desired_concurrent_number = xxx,
          max_error_number = xxx,
          k1 = v1,
          ...
          kn = vn
      )]
      FROM type of routine load
      [(
          k1 = v1,
          ...
          kn = vn
      )]

      load properties:
          load property [[,] load property] ...

      load property:
          column separator | columns_mapping | partitions | where

      column separator:
          COLUMNS TERMINATED BY xxx
      columns_mapping:
          COLUMNS (c1, c2, c3 = c1 + c2)
      partitions:
          PARTITIONS (p1, p2, p3)
      where:
          WHERE c1 > 1

      type of routine load:
          KAFKA
*/
public class CreateRoutineLoadStmt extends DdlStmt {
    // routine load properties
    public static final String DESIRED_CONCURRENT_NUMBER_PROPERTY = "desired_concurrent_number";
    // max error number in ten thousand records
    public static final String MAX_ERROR_NUMBER_PROPERTY = "max_error_number";

    public static final String MAX_BATCH_INTERVAL_SEC_PROPERTY = "max_batch_interval";
    public static final String MAX_BATCH_ROWS_PROPERTY = "max_batch_rows";
    public static final String MAX_BATCH_SIZE_PROPERTY = "max_batch_size";  // deprecated

    public static final String FORMAT = "format";// the value is csv or json, default is csv
    public static final String STRIP_OUTER_ARRAY = "strip_outer_array";
    public static final String JSONPATHS = "jsonpaths";
    public static final String JSONROOT = "json_root";

    // kafka type properties
    public static final String KAFKA_BROKER_LIST_PROPERTY = "kafka_broker_list";
    public static final String KAFKA_TOPIC_PROPERTY = "kafka_topic";
    // optional
    public static final String KAFKA_PARTITIONS_PROPERTY = "kafka_partitions";
    public static final String KAFKA_OFFSETS_PROPERTY = "kafka_offsets";
    public static final String KAFKA_DEFAULT_OFFSETS = "kafka_default_offsets";

    private static final String NAME_TYPE = "ROUTINE LOAD NAME";
    private static final String ENDPOINT_REGEX = "[-A-Za-z0-9+&@#/%?=~_|!:,.;]+[-A-Za-z0-9+&@#/%=~_|]";

    private static final ImmutableSet<String> PROPERTIES_SET = new ImmutableSet.Builder<String>()
            .add(DESIRED_CONCURRENT_NUMBER_PROPERTY)
            .add(MAX_ERROR_NUMBER_PROPERTY)
            .add(MAX_BATCH_INTERVAL_SEC_PROPERTY)
            .add(MAX_BATCH_ROWS_PROPERTY)
            .add(MAX_BATCH_SIZE_PROPERTY)
            .add(FORMAT)
            .add(JSONPATHS)
            .add(STRIP_OUTER_ARRAY)
            .add(JSONROOT)
            .add(LoadStmt.STRICT_MODE)
            .add(LoadStmt.TIMEZONE)
            .add(LoadStmt.PARTIAL_UPDATE)
            .build();

    private static final ImmutableSet<String> KAFKA_PROPERTIES_SET = new ImmutableSet.Builder<String>()
            .add(KAFKA_BROKER_LIST_PROPERTY)
            .add(KAFKA_TOPIC_PROPERTY)
            .add(KAFKA_PARTITIONS_PROPERTY)
            .add(KAFKA_OFFSETS_PROPERTY)
            .build();

    private final LabelName labelName;
    private final String tableName;
    private final List<ParseNode> loadPropertyList;
    private final Map<String, String> jobProperties;
    private final String typeName;
    private final Map<String, String> dataSourceProperties;

    // the following variables will be initialized after analyze
    // -1 as unset, the default value will set in RoutineLoadJob
    private String name;
    private String dbName;
    private RoutineLoadDesc routineLoadDesc;
    private int desiredConcurrentNum = 1;
    private long maxErrorNum = -1;
    private long maxBatchIntervalS = -1;
    private long maxBatchRows = -1;
    private boolean strictMode = true;
    private String timezone = TimeUtils.DEFAULT_TIME_ZONE;
    private boolean partialUpdate = false;
    /**
     * RoutineLoad support json data.
     * Require Params:
     * 1) dataFormat = "json"
     * 2) jsonPaths = "$.XXX.xxx"
     */
    private String format = ""; //default is csv.
    private String jsonPaths = "";
    private String jsonRoot = ""; // MUST be a jsonpath string
    private boolean stripOuterArray = false;

    // kafka related properties
    private String kafkaBrokerList;
    private String kafkaTopic;
    // pair<partition id, offset>
    private List<Pair<Integer, Long>> kafkaPartitionOffsets = Lists.newArrayList();

    // custom kafka property map<key, value>
    private Map<String, String> customKafkaProperties = Maps.newHashMap();

    public static final Predicate<Long> DESIRED_CONCURRENT_NUMBER_PRED = (v) -> v > 0L;
    public static final Predicate<Long> MAX_ERROR_NUMBER_PRED = (v) -> v >= 0L;
    public static final Predicate<Long> MAX_BATCH_INTERVAL_PRED = (v) -> v >= 5;
    public static final Predicate<Long> MAX_BATCH_ROWS_PRED = (v) -> v >= 200000;

    public CreateRoutineLoadStmt(LabelName labelName, String tableName, List<ParseNode> loadPropertyList,
                                 Map<String, String> jobProperties,
                                 String typeName, Map<String, String> dataSourceProperties) {
        this.labelName = labelName;
        this.tableName = tableName;
        this.loadPropertyList = loadPropertyList;
        this.jobProperties = jobProperties == null ? Maps.newHashMap() : jobProperties;
        this.typeName = typeName.toUpperCase();
        this.dataSourceProperties = dataSourceProperties;
    }

    public String getName() {
        return name;
    }

    public String getDBName() {
        return dbName;
    }

    public String getTableName() {
        return tableName;
    }

    public String getTypeName() {
        return typeName;
    }

    public RoutineLoadDesc getRoutineLoadDesc() {
        return routineLoadDesc;
    }

    public int getDesiredConcurrentNum() {
        return desiredConcurrentNum;
    }

    public long getMaxErrorNum() {
        return maxErrorNum;
    }

    public long getMaxBatchIntervalS() {
        return maxBatchIntervalS;
    }

    public long getMaxBatchRows() {
        return maxBatchRows;
    }

    public boolean isStrictMode() {
        return strictMode;
    }

    public String getTimezone() {
        return timezone;
    }

    public boolean isPartialUpdate() {
        return partialUpdate;
    }

    public String getFormat() {
        return format;
    }

    public boolean isStripOuterArray() {
        return stripOuterArray;
    }

    public String getJsonPaths() {
        return jsonPaths;
    }

    public String getJsonRoot() {
        return jsonRoot;
    }

    public String getKafkaBrokerList() {
        return kafkaBrokerList;
    }

    public String getKafkaTopic() {
        return kafkaTopic;
    }

    public List<Pair<Integer, Long>> getKafkaPartitionOffsets() {
        return kafkaPartitionOffsets;
    }

    public Map<String, String> getCustomKafkaProperties() {
        return customKafkaProperties;
    }

    public List<ParseNode> getLoadPropertyList() {
        return loadPropertyList;
    }

    @Override
    public void analyze(Analyzer analyzer) throws UserException {
        super.analyze(analyzer);
        // check dbName and tableName
        checkDBTable(analyzer);
        // check name
        FeNameFormat.checkCommonName(NAME_TYPE, name);
        // check load properties include column separator etc.
        routineLoadDesc = buildLoadDesc(loadPropertyList);
        // check routine load job properties include desired concurrent number etc.
        checkJobProperties();
        // check data source properties
        checkDataSourceProperties();
    }

    public void checkDBTable(Analyzer analyzer) throws AnalysisException {
        labelName.analyze(analyzer);
        dbName = labelName.getDbName();
        name = labelName.getLabelName();
        if (Strings.isNullOrEmpty(tableName)) {
            throw new AnalysisException("Table name should not be null");
        }
    }

    public static RoutineLoadDesc buildLoadDesc(List<ParseNode> loadPropertyList) throws UserException {
        if (loadPropertyList == null) {
            return null;
        }
        ColumnSeparator columnSeparator = null;
        RowDelimiter rowDelimiter = null;
        ImportColumnsStmt importColumnsStmt = null;
        ImportWhereStmt importWhereStmt = null;
        PartitionNames partitionNames = null;
        for (ParseNode parseNode : loadPropertyList) {
            if (parseNode instanceof ColumnSeparator) {
                // check column separator
                if (columnSeparator != null) {
                    throw new AnalysisException("repeat setting of column separator");
                }
                columnSeparator = (ColumnSeparator) parseNode;
                columnSeparator.analyze(null);
            } else if (parseNode instanceof RowDelimiter) {
                // check row delimiter
                if (rowDelimiter != null) {
                    throw new AnalysisException("repeat setting of row delimiter");
                }
                rowDelimiter = (RowDelimiter) parseNode;
                rowDelimiter.analyze(null);
            } else if (parseNode instanceof ImportColumnsStmt) {
                // check columns info
                if (importColumnsStmt != null) {
                    throw new AnalysisException("repeat setting of columns info");
                }
                importColumnsStmt = (ImportColumnsStmt) parseNode;
            } else if (parseNode instanceof ImportWhereStmt) {
                // check where expr
                if (importWhereStmt != null) {
                    throw new AnalysisException("repeat setting of where predicate");
                }
                importWhereStmt = (ImportWhereStmt) parseNode;
            } else if (parseNode instanceof PartitionNames) {
                // check partition names
                if (partitionNames != null) {
                    throw new AnalysisException("repeat setting of partition names");
                }
                partitionNames = (PartitionNames) parseNode;
                partitionNames.analyze(null);
            }
        }
        return new RoutineLoadDesc(columnSeparator, rowDelimiter, importColumnsStmt, importWhereStmt,
                partitionNames);
    }

    private void checkJobProperties() throws UserException {
        Optional<String> optional = jobProperties.keySet().stream().filter(
                entity -> !PROPERTIES_SET.contains(entity)).findFirst();
        if (optional.isPresent()) {
            throw new AnalysisException(optional.get() + " is invalid property");
        }

        desiredConcurrentNum =
                ((Long) Util.getLongPropertyOrDefault(jobProperties.get(DESIRED_CONCURRENT_NUMBER_PROPERTY),
                        Config.max_routine_load_task_concurrent_num,
                        DESIRED_CONCURRENT_NUMBER_PRED,
                        DESIRED_CONCURRENT_NUMBER_PROPERTY + " should > 0")).intValue();

        maxErrorNum = Util.getLongPropertyOrDefault(jobProperties.get(MAX_ERROR_NUMBER_PROPERTY),
                RoutineLoadJob.DEFAULT_MAX_ERROR_NUM, MAX_ERROR_NUMBER_PRED,
                MAX_ERROR_NUMBER_PROPERTY + " should >= 0");

        maxBatchIntervalS = Util.getLongPropertyOrDefault(jobProperties.get(MAX_BATCH_INTERVAL_SEC_PROPERTY),
                RoutineLoadJob.DEFAULT_TASK_SCHED_INTERVAL_SECOND, MAX_BATCH_INTERVAL_PRED,
                MAX_BATCH_INTERVAL_SEC_PROPERTY + " should >= 5");

        maxBatchRows = Util.getLongPropertyOrDefault(jobProperties.get(MAX_BATCH_ROWS_PROPERTY),
                RoutineLoadJob.DEFAULT_MAX_BATCH_ROWS, MAX_BATCH_ROWS_PRED,
                MAX_BATCH_ROWS_PROPERTY + " should >= 200000");

        strictMode = Util.getBooleanPropertyOrDefault(jobProperties.get(LoadStmt.STRICT_MODE),
                RoutineLoadJob.DEFAULT_STRICT_MODE,
                LoadStmt.STRICT_MODE + " should be a boolean");

        partialUpdate = Util.getBooleanPropertyOrDefault(jobProperties.get(LoadStmt.PARTIAL_UPDATE),
                false,
                LoadStmt.PARTIAL_UPDATE + " should be a boolean");

        if (ConnectContext.get() != null) {
            timezone = ConnectContext.get().getSessionVariable().getTimeZone();
        }
        timezone = TimeUtils.checkTimeZoneValidAndStandardize(jobProperties.getOrDefault(LoadStmt.TIMEZONE, timezone));

        format = jobProperties.get(FORMAT);
        if (format != null) {
            if (format.equalsIgnoreCase("csv")) {
                format = ""; // if it's not json, then it's mean csv and set empty
            } else if (format.equalsIgnoreCase("json")) {
                format = "json";
                jsonPaths = jobProperties.get(JSONPATHS);
                jsonRoot = jobProperties.get(JSONROOT);
                stripOuterArray = Boolean.valueOf(jobProperties.getOrDefault(STRIP_OUTER_ARRAY, "false"));
            } else {
                throw new UserException("Format type is invalid. format=`" + format + "`");
            }
        } else {
            format = "csv"; // default csv
        }
    }

    private void checkDataSourceProperties() throws AnalysisException {
        LoadDataSourceType type;
        try {
            type = LoadDataSourceType.valueOf(typeName);
        } catch (IllegalArgumentException e) {
            throw new AnalysisException("routine load job does not support this type " + typeName);
        }
        switch (type) {
            case KAFKA:
                checkKafkaProperties();
                break;
            default:
                break;
        }
    }

    private void checkKafkaProperties() throws AnalysisException {
        Optional<String> optional = dataSourceProperties.keySet().stream()
                .filter(entity -> !KAFKA_PROPERTIES_SET.contains(entity))
                .filter(entity -> !entity.startsWith("property.")).findFirst();
        if (optional.isPresent()) {
            throw new AnalysisException(optional.get() + " is invalid kafka custom property");
        }

        // check broker list
        kafkaBrokerList = Strings.nullToEmpty(dataSourceProperties.get(KAFKA_BROKER_LIST_PROPERTY)).replaceAll(" ", "");
        if (Strings.isNullOrEmpty(kafkaBrokerList)) {
            throw new AnalysisException(KAFKA_BROKER_LIST_PROPERTY + " is a required property");
        }
        String[] kafkaBrokerList = this.kafkaBrokerList.split(",");
        for (String broker : kafkaBrokerList) {
            if (!Pattern.matches(ENDPOINT_REGEX, broker)) {
                throw new AnalysisException(KAFKA_BROKER_LIST_PROPERTY + ":" + broker
                        + " not match pattern " + ENDPOINT_REGEX);
            }
        }

        // check topic
        kafkaTopic = Strings.nullToEmpty(dataSourceProperties.get(KAFKA_TOPIC_PROPERTY)).replaceAll(" ", "");
        if (Strings.isNullOrEmpty(kafkaTopic)) {
            throw new AnalysisException(KAFKA_TOPIC_PROPERTY + " is a required property");
        }

        // check custom kafka property before check partitions,
        // because partitions can use kafka_default_offsets property
        analyzeCustomProperties(dataSourceProperties, customKafkaProperties);

        // check partitions
        String kafkaPartitionsString = dataSourceProperties.get(KAFKA_PARTITIONS_PROPERTY);
        if (kafkaPartitionsString != null) {
            analyzeKafkaPartitionProperty(kafkaPartitionsString, customKafkaProperties, kafkaPartitionOffsets);
        }

        // check offset
        String kafkaOffsetsString = dataSourceProperties.get(KAFKA_OFFSETS_PROPERTY);
        if (kafkaOffsetsString != null) {
            analyzeKafkaOffsetProperty(kafkaOffsetsString, kafkaPartitionOffsets);
        }
    }

    public static void analyzeKafkaPartitionProperty(String kafkaPartitionsString,
                                                     Map<String, String> customKafkaProperties,
                                                     List<Pair<Integer, Long>> kafkaPartitionOffsets)
            throws AnalysisException {
        kafkaPartitionsString = kafkaPartitionsString.replaceAll(" ", "");
        if (kafkaPartitionsString.isEmpty()) {
            throw new AnalysisException(KAFKA_PARTITIONS_PROPERTY + " could not be a empty string");
        }

        // get kafka default offset if set
        Long kafkaDefaultOffset = null;
        if (customKafkaProperties.containsKey(KAFKA_DEFAULT_OFFSETS)) {
            kafkaDefaultOffset = getKafkaOffset(customKafkaProperties.get(KAFKA_DEFAULT_OFFSETS));
        }

        String[] kafkaPartitionsStringList = kafkaPartitionsString.split(",");
        for (String s : kafkaPartitionsStringList) {
            try {
                kafkaPartitionOffsets.add(
                        Pair.create(getIntegerValueFromString(s, KAFKA_PARTITIONS_PROPERTY),
                                kafkaDefaultOffset == null ? KafkaProgress.OFFSET_END_VAL : kafkaDefaultOffset));
            } catch (AnalysisException e) {
                throw new AnalysisException(KAFKA_PARTITIONS_PROPERTY
                        + " must be a number string with comma-separated");
            }
        }
    }

    public static void analyzeKafkaOffsetProperty(String kafkaOffsetsString,
                                                  List<Pair<Integer, Long>> kafkaPartitionOffsets)
            throws AnalysisException {
        kafkaOffsetsString = kafkaOffsetsString.replaceAll(" ", "");
        if (kafkaOffsetsString.isEmpty()) {
            throw new AnalysisException(KAFKA_OFFSETS_PROPERTY + " could not be a empty string");
        }
        String[] kafkaOffsetsStringList = kafkaOffsetsString.split(",");
        if (kafkaOffsetsStringList.length != kafkaPartitionOffsets.size()) {
            throw new AnalysisException("Partitions number should be equals to offsets number");
        }

        for (int i = 0; i < kafkaOffsetsStringList.length; i++) {
            kafkaPartitionOffsets.get(i).second = getKafkaOffset(kafkaOffsetsStringList[i]);
        }
    }

    // Get kafka offset from string
    // defined in librdkafka/rdkafkacpp.h
    // OFFSET_BEGINNING: -2
    // OFFSET_END: -1
    public static long getKafkaOffset(String offsetStr) throws AnalysisException {
        long offset = -1;
        try {
            offset = getLongValueFromString(offsetStr, "kafka offset");
            if (offset < 0) {
                throw new AnalysisException("Can not specify offset smaller than 0");
            }
        } catch (AnalysisException e) {
            if (offsetStr.equalsIgnoreCase(KafkaProgress.OFFSET_BEGINNING)) {
                offset = KafkaProgress.OFFSET_BEGINNING_VAL;
            } else if (offsetStr.equalsIgnoreCase(KafkaProgress.OFFSET_END)) {
                offset = KafkaProgress.OFFSET_END_VAL;
            } else {
                throw e;
            }
        }
        return offset;
    }

    public static void analyzeCustomProperties(Map<String, String> dataSourceProperties,
                                               Map<String, String> customKafkaProperties) throws AnalysisException {
        for (Map.Entry<String, String> dataSourceProperty : dataSourceProperties.entrySet()) {
            if (dataSourceProperty.getKey().startsWith("property.")) {
                String propertyKey = dataSourceProperty.getKey();
                String propertyValue = dataSourceProperty.getValue();
                String propertyValueArr[] = propertyKey.split("\\.");
                if (propertyValueArr.length < 2) {
                    throw new AnalysisException("kafka property value could not be a empty string");
                }
                customKafkaProperties.put(propertyKey.substring(propertyKey.indexOf(".") + 1), propertyValue);
            }
            // can be extended in the future which other prefix
        }

        // check kafka_default_offsets
        if (customKafkaProperties.containsKey(KAFKA_DEFAULT_OFFSETS)) {
            getKafkaOffset(customKafkaProperties.get(KAFKA_DEFAULT_OFFSETS));
        }
    }

    private static int getIntegerValueFromString(String valueString, String propertyName) throws AnalysisException {
        if (valueString.isEmpty()) {
            throw new AnalysisException(propertyName + " could not be a empty string");
        }
        int value;
        try {
            value = Integer.parseInt(valueString);
        } catch (NumberFormatException e) {
            throw new AnalysisException(propertyName + " must be a integer");
        }
        return value;
    }

    private static long getLongValueFromString(String valueString, String propertyName) throws AnalysisException {
        if (valueString.isEmpty()) {
            throw new AnalysisException(propertyName + " could not be a empty string");
        }
        long value;
        try {
            value = Long.valueOf(valueString);
        } catch (NumberFormatException e) {
            throw new AnalysisException(propertyName + " must be a integer: " + valueString);
        }
        return value;
    }
}
