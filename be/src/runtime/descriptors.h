// This file is made available under Elastic License 2.0.
// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/be/src/runtime/descriptors.h

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

#pragma once

#include <google/protobuf/repeated_field.h>
#include <google/protobuf/stubs/common.h>

#include <ostream>
#include <unordered_map>
#include <vector>

#include "common/global_types.h"
#include "common/status.h"
#include "gen_cpp/Descriptors_types.h"     // for TTupleId
#include "gen_cpp/FrontendService_types.h" // for TTupleId
#include "gen_cpp/Types_types.h"
#include "runtime/types.h"

namespace starrocks {

class ObjectPool;
class TDescriptorTable;
class TSlotDescriptor;
class TTupleDescriptor;
class Expr;
class ExprContext;
class RuntimeState;
namespace vectorized {
class SchemaScanner;
} // namespace vectorized
class OlapTableSchemaParam;
class PTupleDescriptor;
class PSlotDescriptor;

// Location information for null indicator bit for particular slot.
// For non-nullable slots, the byte_offset will be 0 and the bit_mask will be 0.
// This allows us to do the NullIndicatorOffset operations (tuple + byte_offset &/|
// bit_mask) regardless of whether the slot is nullable or not.
// This is more efficient than branching to check if the slot is non-nullable.
struct NullIndicatorOffset {
    int byte_offset;
    uint8_t bit_mask;   // to extract null indicator
    uint8_t bit_offset; // only used to serialize, from 1 to 8

    NullIndicatorOffset(int byte_offset, int bit_offset_)
            : byte_offset(byte_offset),
              bit_mask(bit_offset_ == -1 ? 0 : 1 << (7 - bit_offset_)),
              bit_offset(bit_offset_) {}

    bool equals(const NullIndicatorOffset& o) const {
        return this->byte_offset == o.byte_offset && this->bit_mask == o.bit_mask;
    }

    std::string debug_string() const;
};

std::ostream& operator<<(std::ostream& os, const NullIndicatorOffset& null_indicator);

class SlotDescriptor {
public:
    SlotId id() const { return _id; }
    const TypeDescriptor& type() const { return _type; }
    TypeDescriptor& type() { return _type; }
    TupleId parent() const { return _parent; }
    // Returns the column index of this slot, including partition keys.
    // (e.g., col_pos - num_partition_keys = the table column this slot corresponds to)
    int col_pos() const { return _col_pos; }
    // Returns the field index in the generated llvm struct for this slot's tuple
    int field_idx() const { return _field_idx; }
    int tuple_offset() const { return _tuple_offset; }
    const NullIndicatorOffset& null_indicator_offset() const { return _null_indicator_offset; }
    bool is_materialized() const { return _is_materialized; }
    bool is_nullable() const { return _null_indicator_offset.bit_mask != 0; }

    int slot_size() const { return _slot_size; }

    const std::string& col_name() const { return _col_name; }

    /// Return true if the physical layout of this descriptor matches the physical layout
    /// of other_desc, but not necessarily ids.
    bool layout_equals(const SlotDescriptor& other_desc) const;

    void to_protobuf(PSlotDescriptor* pslot) const;

    std::string debug_string() const;

private:
    friend class DescriptorTbl;
    friend class TupleDescriptor;
    friend class vectorized::SchemaScanner;
    friend class OlapTableSchemaParam;

    const SlotId _id;
    TypeDescriptor _type;
    const TupleId _parent;
    const int _col_pos;
    const int _tuple_offset;
    const NullIndicatorOffset _null_indicator_offset;
    const std::string _col_name;

    // the idx of the slot in the tuple descriptor (0-based).
    // this is provided by the FE
    const int _slot_idx;

    // the byte size of this slot.
    const int _slot_size;

    // the idx of the slot in the llvm codegen'd tuple struct
    // this is set by TupleDescriptor during codegen and takes into account
    // leading null bytes.
    int _field_idx;

    const bool _is_materialized;

    SlotDescriptor(const TSlotDescriptor& tdesc);
    SlotDescriptor(const PSlotDescriptor& pdesc);
};

// Base class for table descriptors.
class TableDescriptor {
public:
    TableDescriptor(const TTableDescriptor& tdesc);
    virtual ~TableDescriptor() = default;
    int num_cols() const { return _num_cols; }
    int num_clustering_cols() const { return _num_clustering_cols; }
    TableId table_id() const { return _id; }
    virtual std::string debug_string() const;

    // The first _num_clustering_cols columns by position are clustering
    // columns.
    bool is_clustering_col(const SlotDescriptor* slot_desc) const {
        return slot_desc->col_pos() < _num_clustering_cols;
    }

    const std::string& name() const { return _name; }
    const std::string& database() const { return _database; }

private:
    std::string _name;
    std::string _database;
    TableId _id;
    int _num_cols;
    int _num_clustering_cols;
};

class HdfsPartitionDescriptor {
public:
    HdfsPartitionDescriptor(const THdfsTable& thrift_table, const THdfsPartition& thrift_partition);
    HdfsPartitionDescriptor(const THudiTable& thrift_table, const THdfsPartition& thrift_partition);

    int64_t id() const { return _id; }
    THdfsFileFormat::type file_format() { return _file_format; }
    std::string& location() { return _location; }
    // ExprContext is constant/literal for sure
    // such as hdfs://path/x=1/y=2/zzz, then
    // partition slots would be [x, y]
    // partition key values wold be [1, 2]
    std::vector<ExprContext*>& partition_key_value_evals() { return _partition_key_value_evals; }
    Status create_part_key_exprs(ObjectPool* pool, int32_t chunk_size);

private:
    int64_t _id = 0;
    THdfsFileFormat::type _file_format;
    std::string _location;

    const std::vector<TExpr>& _thrift_partition_key_exprs;
    std::vector<ExprContext*> _partition_key_value_evals;
};

class HdfsTableDescriptor : public TableDescriptor {
public:
    HdfsTableDescriptor(const TTableDescriptor& tdesc, ObjectPool* pool);
    ~HdfsTableDescriptor() override = default;

    bool is_partition_col(const SlotDescriptor* slot) const;
    int get_partition_col_index(const SlotDescriptor* slot) const;
    HdfsPartitionDescriptor* get_partition(int64_t partition_id) const;

    const std::string& hdfs_base_dir() const { return _hdfs_base_dir; }

    Status create_key_exprs(ObjectPool* pool, int32_t chunk_size) {
        for (auto& part : _partition_id_to_desc_map) {
            RETURN_IF_ERROR(part.second->create_part_key_exprs(pool, chunk_size));
        }
        return Status::OK();
    }

private:
    std::string _hdfs_base_dir;
    std::vector<TColumn> _columns;
    std::vector<TColumn> _partition_columns;
    std::map<int64_t, HdfsPartitionDescriptor*> _partition_id_to_desc_map;
};

class IcebergTableDescriptor : public TableDescriptor {
public:
    IcebergTableDescriptor(const TTableDescriptor& tdesc);
    ~IcebergTableDescriptor() override = default;

private:
    std::string _table_location;
    std::vector<TColumn> _columns;
};

class HudiTableDescriptor : public TableDescriptor {
public:
    HudiTableDescriptor(const TTableDescriptor& tdesc, ObjectPool* pool);
    ~HudiTableDescriptor() override = default;

    bool is_partition_col(const SlotDescriptor* slot) const;
    int get_partition_col_index(const SlotDescriptor* slot) const;
    HdfsPartitionDescriptor* get_partition(int64_t partition_id) const;

    const std::string& hdfs_base_dir() const { return _table_location; }

    Status create_key_exprs(ObjectPool* pool, int32_t chunk_size) {
        for (auto& part : _partition_id_to_desc_map) {
            RETURN_IF_ERROR(part.second->create_part_key_exprs(pool, chunk_size));
        }
        return Status::OK();
    }

private:
    std::string _table_location;
    std::vector<TColumn> _columns;
    std::vector<TColumn> _partition_columns;
    std::map<int64_t, HdfsPartitionDescriptor*> _partition_id_to_desc_map;
};

class OlapTableDescriptor : public TableDescriptor {
public:
    OlapTableDescriptor(const TTableDescriptor& tdesc);
    std::string debug_string() const override;
};

class SchemaTableDescriptor : public TableDescriptor {
public:
    SchemaTableDescriptor(const TTableDescriptor& tdesc);
    ~SchemaTableDescriptor() override;
    std::string debug_string() const override;
    TSchemaTableType::type schema_table_type() const { return _schema_table_type; }

private:
    TSchemaTableType::type _schema_table_type;
};

class BrokerTableDescriptor : public TableDescriptor {
public:
    BrokerTableDescriptor(const TTableDescriptor& tdesc);
    ~BrokerTableDescriptor() override;
    std::string debug_string() const override;

private:
};

class EsTableDescriptor : public TableDescriptor {
public:
    EsTableDescriptor(const TTableDescriptor& tdesc);
    ~EsTableDescriptor() override;
    std::string debug_string() const override;

private:
};

class MySQLTableDescriptor : public TableDescriptor {
public:
    MySQLTableDescriptor(const TTableDescriptor& tdesc);
    std::string debug_string() const override;
    const std::string mysql_db() const { return _mysql_db; }
    const std::string mysql_table() const { return _mysql_table; }
    const std::string host() const { return _host; }
    const std::string port() const { return _port; }
    const std::string user() const { return _user; }
    const std::string passwd() const { return _passwd; }

private:
    std::string _mysql_db;
    std::string _mysql_table;
    std::string _host;
    std::string _port;
    std::string _user;
    std::string _passwd;
};

class TupleDescriptor {
public:
    int byte_size() const { return _byte_size; }
    int num_null_slots() const { return _num_null_slots; }
    int num_null_bytes() const { return _num_null_bytes; }
    const std::vector<SlotDescriptor*>& slots() const { return _slots; }
    std::vector<SlotDescriptor*>& slots() { return _slots; }
    const std::vector<SlotDescriptor*>& decoded_slots() const { return _decoded_slots; }
    std::vector<SlotDescriptor*>& decoded_slots() { return _decoded_slots; }
    bool has_varlen_slots() const { return _has_varlen_slots; }
    const TableDescriptor* table_desc() const { return _table_desc; }
    void set_table_desc(TableDescriptor* table_desc) { _table_desc = table_desc; }

    TupleId id() const { return _id; }

    /// Return true if the physical layout of this descriptor matches that of other_desc,
    /// but not necessarily the id.
    bool layout_equals(const TupleDescriptor& other_desc) const;

    std::string debug_string() const;

    void to_protobuf(PTupleDescriptor* ptuple) const;

private:
    friend class DescriptorTbl;
    friend class OlapTableSchemaParam;

    const TupleId _id;
    TableDescriptor* _table_desc;
    int _byte_size;
    int _num_null_slots;
    int _num_null_bytes;
    std::vector<SlotDescriptor*> _slots; // contains all slots
    // For a low cardinality string column with global dict,
    // The type in _slots is int, in _decode_slots is varchar
    std::vector<SlotDescriptor*> _decoded_slots;

    // Provide quick way to check if there are variable length slots.
    // True if _string_slots or _collection_slots have entries.
    bool _has_varlen_slots;

    TupleDescriptor(const TTupleDescriptor& tdesc);
    TupleDescriptor(const PTupleDescriptor& tdesc);
    void add_slot(SlotDescriptor* slot);

    /// Returns slots in their physical order.
    std::vector<SlotDescriptor*> slots_ordered_by_idx() const;
};

class DescriptorTbl {
public:
    // Creates a descriptor tbl within 'pool' from thrift_tbl and returns it via 'tbl'.
    // Returns OK on success, otherwise error (in which case 'tbl' will be unset).
    static Status create(ObjectPool* pool, const TDescriptorTable& thrift_tbl, DescriptorTbl** tbl, int32_t chunk_size);

    TableDescriptor* get_table_descriptor(TableId id) const;
    TupleDescriptor* get_tuple_descriptor(TupleId id) const;
    SlotDescriptor* get_slot_descriptor(SlotId id) const;

    // return all registered tuple descriptors
    void get_tuple_descs(std::vector<TupleDescriptor*>* descs) const;

    std::string debug_string() const;

private:
    typedef std::unordered_map<TableId, TableDescriptor*> TableDescriptorMap;
    typedef std::unordered_map<TupleId, TupleDescriptor*> TupleDescriptorMap;
    typedef std::unordered_map<SlotId, SlotDescriptor*> SlotDescriptorMap;

    TableDescriptorMap _tbl_desc_map;
    TupleDescriptorMap _tuple_desc_map;
    SlotDescriptorMap _slot_desc_map;

    DescriptorTbl() {}
};

// Records positions of tuples within row produced by ExecNode.
// TODO: this needs to differentiate between tuples contained in row
// and tuples produced by ExecNode (parallel to PlanNode.rowTupleIds and
// PlanNode.tupleIds); right now, we conflate the two (and distinguish based on
// context; for instance, HdfsScanNode uses these tids to create row batches, ie, the
// first case, whereas TopNNode uses these tids to copy output rows, ie, the second
// case)
class RowDescriptor {
public:
    RowDescriptor(const DescriptorTbl& desc_tbl, const std::vector<TTupleId>& row_tuples,
                  const std::vector<bool>& nullable_tuples);

    // standard copy c'tor, made explicit here
    RowDescriptor(const RowDescriptor& desc)
            : _tuple_desc_map(desc._tuple_desc_map),
              _tuple_idx_nullable_map(desc._tuple_idx_nullable_map),
              _tuple_idx_map(desc._tuple_idx_map),
              _has_varlen_slots(desc._has_varlen_slots) {
        _num_null_slots = 0;
        std::vector<TupleDescriptor*>::const_iterator it = desc._tuple_desc_map.begin();
        for (; it != desc._tuple_desc_map.end(); ++it) {
            _num_null_slots += (*it)->num_null_slots();
        }
        _num_null_bytes = (_num_null_slots + 7) / 8;
    }

    RowDescriptor(TupleDescriptor* tuple_desc, bool is_nullable);

    // dummy descriptor, needed for the JNI EvalPredicate() function
    RowDescriptor() = default;

    // Returns total size in bytes.
    // TODO: also take avg string lengths into account, ie, change this
    // to GetAvgRowSize()
    int get_row_size() const;

    int num_null_slots() const { return _num_null_slots; }

    int num_null_bytes() const { return _num_null_bytes; }

    static const int INVALID_IDX;

    // Returns INVALID_IDX if id not part of this row.
    int get_tuple_idx(TupleId id) const;

    // Return true if the Tuple of the given Tuple index is nullable.
    bool tuple_is_nullable(int tuple_idx) const;

    // Return true if any Tuple of the row is nullable.
    bool is_any_tuple_nullable() const;

    // Return true if any Tuple has variable length slots.
    bool has_varlen_slots() const { return _has_varlen_slots; }

    // Return descriptors for all tuples in this row, in order of appearance.
    const std::vector<TupleDescriptor*>& tuple_descriptors() const { return _tuple_desc_map; }

    // Populate row_tuple_ids with our ids.
    void to_thrift(std::vector<TTupleId>* row_tuple_ids);
    void to_protobuf(google::protobuf::RepeatedField<google::protobuf::int32>* row_tuple_ids);

    // Return true if the tuple ids of this descriptor are a prefix
    // of the tuple ids of other_desc.
    bool is_prefix_of(const RowDescriptor& other_desc) const;

    // Return true if the tuple ids of this descriptor match tuple ids of other desc.
    bool equals(const RowDescriptor& other_desc) const;

    /// Return true if the physical layout of this descriptor matches the physical layout
    /// of other_desc, but not necessarily the ids.
    bool layout_equals(const RowDescriptor& other_desc) const;

    /// Return true if the tuples of this descriptor are a prefix of the tuples of
    /// other_desc. Tuples are compared by their physical layout and not by ids.
    bool layout_is_prefix_of(const RowDescriptor& other_desc) const;

    std::string debug_string() const;

private:
    // Initializes tupleIdxMap during c'tor using the _tuple_desc_map.
    void init_tuple_idx_map();

    // Initializes _has_varlen_slots during c'tor using the _tuple_desc_map.
    void init_has_varlen_slots();

    // map from position of tuple w/in row to its descriptor
    std::vector<TupleDescriptor*> _tuple_desc_map;

    // _tuple_idx_nullable_map[i] is true if tuple i can be null
    std::vector<bool> _tuple_idx_nullable_map;

    // map from TupleId to position of tuple w/in row
    std::vector<int> _tuple_idx_map;

    // Provide quick way to check if there are variable length slots.
    bool _has_varlen_slots;

    int _num_null_slots = 0;
    int _num_null_bytes = 0;
};

} // namespace starrocks
