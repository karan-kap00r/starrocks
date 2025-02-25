// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "exprs/table_function/java_udtf_function.h"

#include <memory>
#include <utility>

#include "column/array_column.h"
#include "column/column_helper.h"
#include "column/nullable_column.h"
#include "column/vectorized_fwd.h"
#include "exprs/table_function/table_function.h"
#include "gen_cpp/Types_types.h"
#include "gutil/casts.h"
#include "jni.h"
#include "runtime/types.h"
#include "runtime/user_function_cache.h"
#include "udf/java/java_udf.h"

namespace starrocks::vectorized {
template <bool handle_null>
jvalue cast_to_jvalue(MethodTypeDescriptor method_type_desc, const Column* col, int row_num);
void release_jvalue(MethodTypeDescriptor method_type_desc, jvalue val);
void append_jvalue(MethodTypeDescriptor method_type_desc, Column* col, jvalue val);

const TableFunction* getJavaUDTFFunction() {
    static JavaUDTFFunction java_table_function;
    return &java_table_function;
}

class JavaUDTFState : public TableFunctionState {
public:
    JavaUDTFState(std::string libpath, std::string symbol, const TTypeDesc& desc)
            : _libpath(std::move(libpath)), _symbol(std::move(symbol)), _ret_type(TypeDescriptor::from_thrift(desc)) {}
    ~JavaUDTFState() {
        if (_udtf_handle) {
            JVMFunctionHelper::getInstance().getEnv()->DeleteLocalRef(_udtf_handle);
        }
    }

    Status open();
    void close();

    const TypeDescriptor& type_desc() { return _ret_type; }
    JavaMethodDescriptor* method_process() { return _process.get(); }
    jclass get_udtf_clazz() { return _udtf_class.clazz(); }
    jobject handle() { return _udtf_handle; }

private:
    std::string _libpath;
    std::string _symbol;

    std::unique_ptr<ClassLoader> _class_loader;
    std::unique_ptr<ClassAnalyzer> _analyzer;
    JVMClass _udtf_class = nullptr;
    jobject _udtf_handle = nullptr;
    std::unique_ptr<JavaMethodDescriptor> _process;
    TypeDescriptor _ret_type;
};

Status JavaUDTFState::open() {
    _class_loader = std::make_unique<ClassLoader>(std::move(_libpath));
    RETURN_IF_ERROR(_class_loader->init());
    _analyzer = std::make_unique<ClassAnalyzer>();

    _udtf_class = _class_loader->getClass(_symbol);
    if (_udtf_class.clazz() == nullptr) {
        return Status::InternalError(fmt::format("Not found symbol:{}", _symbol));
    }

    RETURN_IF_ERROR(_udtf_class.newInstance(&_udtf_handle));

    auto* analyzer = _analyzer.get();
    auto add_method = [&](const std::string& name, jclass clazz, std::unique_ptr<JavaMethodDescriptor>* res) {
        std::string method_name = name;
        std::string signature;
        std::vector<MethodTypeDescriptor> mtdesc;
        RETURN_IF_ERROR(analyzer->get_signature(clazz, method_name, &signature));
        RETURN_IF_ERROR(analyzer->get_udaf_method_desc(signature, &mtdesc));
        *res = std::make_unique<JavaMethodDescriptor>();
        (*res)->name = std::move(method_name);
        (*res)->signature = std::move(signature);
        (*res)->method_desc = std::move(mtdesc);
        return Status::OK();
    };
    RETURN_IF_ERROR(add_method("process", _udtf_class.clazz(), &_process));

    return Status::OK();
}

Status JavaUDTFFunction::init(const TFunction& fn, TableFunctionState** state) const {
    std::string libpath;
    RETURN_IF_ERROR(UserFunctionCache::instance()->get_libpath(fn.fid, fn.hdfs_location, fn.checksum, &libpath));
    // Now we only support one return types
    *state = new JavaUDTFState(std::move(libpath), fn.table_fn.symbol, fn.table_fn.ret_types[0]);
    return Status::OK();
}

Status JavaUDTFFunction::prepare(TableFunctionState* state) const {
    // Nothing to do
    return Status::OK();
}

Status JavaUDTFFunction::open(TableFunctionState* state) const {
    RETURN_IF_ERROR(down_cast<JavaUDTFState*>(state)->open());
    return Status::OK();
}

Status JavaUDTFFunction::close(TableFunctionState* state) const {
    delete state;
    return Status::OK();
}

std::pair<Columns, ColumnPtr> JavaUDTFFunction::process(TableFunctionState* state, bool* eos) const {
    Columns res;
    const Columns& cols = state->get_columns();
    auto* stateUDTF = down_cast<JavaUDTFState*>(state);

    auto& helper = JVMFunctionHelper::getInstance();
    JNIEnv* env = helper.getEnv();

    jmethodID methodID = env->GetMethodID(stateUDTF->get_udtf_clazz(), stateUDTF->method_process()->name.c_str(),
                                          stateUDTF->method_process()->signature.c_str());

    std::vector<jvalue> call_stack;
    std::vector<jobject> rets;
    size_t num_rows = cols[0]->size();
    size_t num_cols = cols.size();

    call_stack.reserve(num_cols);
    rets.resize(num_rows);
    for (int i = 0; i < num_rows; ++i) {
        for (int j = 0; j < num_cols; ++j) {
            jvalue val = cast_to_jvalue<true>(stateUDTF->method_process()->method_desc[j + 1], cols[j].get(), i);
            call_stack.push_back(val);
        }

        rets[i] = env->CallObjectMethodA(stateUDTF->handle(), methodID, call_stack.data());

        for (int j = 0; j < num_cols; ++j) {
            release_jvalue(stateUDTF->method_process()->method_desc[j + 1], call_stack[j]);
        }

        call_stack.clear();
    }

    // Build Return Type
    auto offsets_col = UInt32Column::create_mutable();
    auto& offsets = offsets_col->get_data();
    offsets.resize(num_rows + 1);

    auto col = ColumnHelper::create_column(stateUDTF->type_desc(), true);
    col->reserve(num_rows);

    // TODO: support primitive array
    MethodTypeDescriptor method_desc{stateUDTF->type_desc().type, true, true};

    for (int i = 0; i < num_rows; ++i) {
        int len = rets[i] != nullptr ? env->GetArrayLength((jarray)rets[i]) : 0;
        offsets[i + 1] = offsets[i] + len;
        // update for col
        for (int j = 0; j < len; ++j) {
            jobject vi = env->GetObjectArrayElement((jobjectArray)rets[i], j);
            append_jvalue(method_desc, col.get(), {.l = vi});
            release_jvalue(method_desc, {.l = vi});
        }
    }

    res.emplace_back(std::move(col));

    // TODO: add error msg to Function State
    if (auto jthr = helper.getEnv()->ExceptionOccurred(); jthr != nullptr) {
        std::string err = fmt::format("execute UDF Function meet Exception:{}", helper.dumpExceptionString(jthr));
        LOG(WARNING) << err;
        helper.getEnv()->ExceptionClear();
    }

    return std::make_pair(std::move(res), std::move(offsets_col));
}

} // namespace starrocks::vectorized