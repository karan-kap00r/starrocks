// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#pragma once

#include "column/column.h"
#include "column/type_traits.h"
#include "common/status.h"
#include "gen_cpp/FrontendService_types.h"
#include "runtime/primitive_type.h"

namespace starrocks::vectorized {

// this class is a helper for getting schema info from FE
class SchemaHelper {
public:
    static Status get_db_names(const std::string& ip, const int32_t port, const TGetDbsParams& db_params,
                               TGetDbsResult* db_result);

    static Status get_table_names(const std::string& ip, const int32_t port, const TGetTablesParams& table_params,
                                  TGetTablesResult* table_result);

    static Status list_table_status(const std::string& ip, const int32_t port, const TGetTablesParams& table_params,
                                    TListTableStatusResult* table_result);

    static Status describe_table(const std::string& ip, const int32_t port, const TDescribeTableParams& desc_params,
                                 TDescribeTableResult* desc_result);

    static Status show_varialbes(const std::string& ip, const int32_t port, const TShowVariableRequest& var_params,
                                 TShowVariableResult* var_result);

    static std::string extract_db_name(const std::string& full_name);

    static Status get_user_privs(const std::string& ip, const int32_t port, const TGetUserPrivsParams& var_params,
                                 TGetUserPrivsResult* var_result);

    static Status get_db_privs(const std::string& ip, const int32_t port, const TGetDBPrivsParams& var_params,
                               TGetDBPrivsResult* var_result);

    static Status get_table_privs(const std::string& ip, const int32_t port, const TGetTablePrivsParams& var_params,
                                  TGetTablePrivsResult* var_result);
};

template <PrimitiveType SlotType>
void fill_data_column_with_slot(vectorized::Column* data_column, void* slot) {
    using ColumnType = typename vectorized::RunTimeTypeTraits<SlotType>::ColumnType;
    using ValueType = typename vectorized::RunTimeTypeTraits<SlotType>::CppType;

    ColumnType* result = down_cast<ColumnType*>(data_column);
    if constexpr (vectorized::IsDate<ValueType>) {
        DateTimeValue* date_time_value = (DateTimeValue*)slot;
        vectorized::DateValue date_value = vectorized::DateValue::create(
                date_time_value->year(), date_time_value->month(), date_time_value->day());
        result->append(date_value);
    } else if constexpr (vectorized::IsTimestamp<ValueType>) {
        DateTimeValue* date_time_value = (DateTimeValue*)slot;
        vectorized::TimestampValue timestamp_value = vectorized::TimestampValue::create(
                date_time_value->year(), date_time_value->month(), date_time_value->day(), date_time_value->hour(),
                date_time_value->minute(), date_time_value->second());
        result->append(timestamp_value);
    } else {
        result->append(*(ValueType*)slot);
    }
}

template <PrimitiveType SlotType>
void fill_column_with_slot(vectorized::Column* result, void* slot) {
    if (result->is_nullable()) {
        vectorized::NullableColumn* nullable_column = down_cast<vectorized::NullableColumn*>(result);
        vectorized::NullData& null_data = nullable_column->null_column_data();
        vectorized::Column* data_column = nullable_column->data_column().get();
        null_data.push_back(0);
        fill_data_column_with_slot<SlotType>(data_column, slot);
    } else {
        fill_data_column_with_slot<SlotType>(result, slot);
    }
}

void fill_data_column_with_null(vectorized::Column* data_column);

} // namespace starrocks::vectorized
