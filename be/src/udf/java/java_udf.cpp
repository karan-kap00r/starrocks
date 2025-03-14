// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#include "udf/java/java_udf.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>

#include "column/binary_column.h"
#include "column/column.h"
#include "column/fixed_length_column.h"
#include "column/vectorized_fwd.h"
#include "common/status.h"
#include "fmt/compile.h"
#include "fmt/core.h"
#include "jni.h"
#include "runtime/primitive_type.h"
#include "util/defer_op.h"

#define ADD_NUMBERIC_CLASS(prim_clazz, clazz, sign)                                                           \
    {                                                                                                         \
        _class_##prim_clazz = _env->FindClass("java/lang/" #clazz);                                           \
        CHECK(_class_##prim_clazz);                                                                           \
        _value_of_##prim_clazz =                                                                              \
                _env->GetStaticMethodID(_class_##prim_clazz, "valueOf", "(" #sign ")Ljava/lang/" #clazz ";"); \
        CHECK(_value_of_##prim_clazz) << "Not Found"                                                          \
                                      << "(" #sign ")Ljava/lang/" #clazz ";";                                 \
        _val_##prim_clazz = _env->GetMethodID(_class_##prim_clazz, #prim_clazz "Value", "()" #sign);          \
        CHECK(_val_##prim_clazz);                                                                             \
    }

#define DEFINE_NEW_BOX(prim_clazz, cxx_type, CLAZZ, CallType)                                    \
    jobject JVMFunctionHelper::new##CLAZZ(cxx_type value) {                                      \
        return _env->CallStaticObjectMethod(_class_##prim_clazz, _value_of_##prim_clazz, value); \
    }                                                                                            \
    cxx_type JVMFunctionHelper::val##cxx_type(jobject obj) {                                     \
        return _env->Call##CallType##Method(obj, _val_##prim_clazz);                             \
    }

namespace starrocks::vectorized {

constexpr const char* CLASS_UDF_HELPER_NAME = "com.starrocks.udf.UDFHelper";

JVMFunctionHelper& JVMFunctionHelper::getInstance() {
    static thread_local std::unique_ptr<JVMFunctionHelper> helper;
    if (helper == nullptr) {
        JNIEnv* env = getJNIEnv();
        CHECK(env != nullptr) << "couldn't got a JNIEnv";
        helper.reset(new JVMFunctionHelper(env));
        helper->_add_class_path(getenv("STARROCKS_HOME") + std::string("/lib/udf-class-loader.jar"));
        helper->_init();
    }
    return *helper;
}

void JVMFunctionHelper::_init() {
    _object_class = _env->FindClass("java/lang/Object");
    _object_array_class = _env->FindClass("[Ljava/lang/Object;");
    _string_class = _env->FindClass("java/lang/String");
    _throwable_class = _env->FindClass("java/lang/Throwable");
    _jarrays_class = _env->FindClass("java/util/Arrays");

    CHECK(_object_class);
    CHECK(_string_class);
    CHECK(_throwable_class);
    CHECK(_jarrays_class);

    ADD_NUMBERIC_CLASS(boolean, Boolean, Z);
    ADD_NUMBERIC_CLASS(byte, Byte, B);
    ADD_NUMBERIC_CLASS(short, Short, S);
    ADD_NUMBERIC_CLASS(int, Integer, I);
    ADD_NUMBERIC_CLASS(long, Long, J);
    ADD_NUMBERIC_CLASS(float, Float, F);
    ADD_NUMBERIC_CLASS(double, Double, D);

    jclass charsets = _env->FindClass("java/nio/charset/StandardCharsets");
    DCHECK(charsets != nullptr);
    auto fieldId = _env->GetStaticFieldID(charsets, "UTF_8", "Ljava/nio/charset/Charset;");
    DCHECK(fieldId != nullptr);
    _utf8_charsets = _env->GetStaticObjectField(charsets, fieldId);
    DCHECK(_utf8_charsets != nullptr);
    _string_construct_with_bytes = _env->GetMethodID(_string_class, "<init>", "([BLjava/nio/charset/Charset;)V");
    DCHECK(_string_construct_with_bytes != nullptr);

    std::string name = JVMFunctionHelper::to_jni_class_name(CLASS_UDF_HELPER_NAME);
    _udf_helper_class = _env->FindClass(name.c_str());
    _create_boxed_array = _env->GetStaticMethodID(_udf_helper_class, "createBoxedArray",
                                                  "(IIZ[Ljava/nio/ByteBuffer;)[Ljava/lang/Object;");

    _batch_update_single = _env->GetStaticMethodID(
            _udf_helper_class, "batchUpdateSingle",
            "(Ljava/lang/Object;Ljava/lang/reflect/Method;Ljava/lang/Object;[Ljava/lang/Object;)V");
    _batch_update = _env->GetStaticMethodID(_udf_helper_class, "batchUpdate",
                                            "(Ljava/lang/Object;Ljava/lang/reflect/Method;[Ljava/lang/Object;)V");
    _batch_call = _env->GetStaticMethodID(
            _udf_helper_class, "batchCall",
            "(Ljava/lang/Object;Ljava/lang/reflect/Method;I[Ljava/lang/Object;)[Ljava/lang/Object;");
    _batch_call_no_args = _env->GetStaticMethodID(_udf_helper_class, "batchCall",
                                                  "(Ljava/lang/Object;Ljava/lang/reflect/Method;I)[Ljava/lang/Object;");
    _int_batch_call = _env->GetStaticMethodID(_udf_helper_class, "batchCall",
                                              "([Ljava/lang/Object;Ljava/lang/reflect/Method;I)[I");
    _direct_buffer_class = _env->FindClass("java/nio/ByteBuffer");
    _direct_buffer_clear = _env->GetMethodID(_direct_buffer_class, "clear", "()Ljava/nio/Buffer;");
    DCHECK(_batch_call);
    DCHECK(_batch_call_no_args);
    DCHECK(_direct_buffer_clear);
}

// https://stackoverflow.com/questions/45232522/how-to-set-classpath-of-a-running-jvm-in-cjni
void JVMFunctionHelper::_add_class_path(const std::string& path) {
    const std::string urlPath = "file://" + path;
    LOG(INFO) << "add class path:" << urlPath;
    jclass classLoaderCls = _env->FindClass("java/lang/ClassLoader");
    jmethodID getSystemClassLoaderMethod =
            _env->GetStaticMethodID(classLoaderCls, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
    jobject classLoaderInstance = _env->CallStaticObjectMethod(classLoaderCls, getSystemClassLoaderMethod);
    jclass urlClassLoaderCls = _env->FindClass("java/net/URLClassLoader");
    jmethodID addUrlMethod = _env->GetMethodID(urlClassLoaderCls, "addURL", "(Ljava/net/URL;)V");
    jclass urlCls = _env->FindClass("java/net/URL");
    jmethodID urlConstructor = _env->GetMethodID(urlCls, "<init>", "(Ljava/lang/String;)V");
    jobject urlInstance = _env->NewObject(urlCls, urlConstructor, _env->NewStringUTF(urlPath.c_str()));
    _env->CallVoidMethod(classLoaderInstance, addUrlMethod, urlInstance);
}

jobjectArray JVMFunctionHelper::_build_object_array(jclass clazz, jobject* arr, int sz) {
    jobjectArray res_arr = _env->NewObjectArray(sz, _object_array_class, nullptr);
    for (int i = 0; i < sz; ++i) {
        _env->SetObjectArrayElement(res_arr, i, arr[i]);
    }
    return res_arr;
}

void JVMFunctionHelper::_check_call_exception(FunctionContext* ctx) {
    if (auto e = _env->ExceptionOccurred()) {
        std::string msg = this->dumpExceptionString(e);
        LOG(WARNING) << "Exception: " << msg;
        ctx->set_error(msg.c_str());
        _env->ExceptionClear();
    }
}

std::string JVMFunctionHelper::array_to_string(jobject object) {
    std::string value;
    jmethodID arrayToStringMethod =
            _env->GetStaticMethodID(_jarrays_class, "toString", "([Ljava/lang/Object;)Ljava/lang/String;");
    DCHECK(arrayToStringMethod != nullptr);
    jobject jstr = _env->CallStaticObjectMethod(_jarrays_class, arrayToStringMethod, object);
    value = to_cxx_string((jstring)jstr);
    _env->DeleteLocalRef(jstr);
    return value;
}

std::string JVMFunctionHelper::to_string(jobject obj) {
    std::string value;
    auto method = getToStringMethod(_object_class);
    auto res = _env->CallObjectMethod(obj, method);
    value = to_cxx_string((jstring)res);
    _env->DeleteLocalRef(res);
    return value;
}

std::string JVMFunctionHelper::to_cxx_string(jstring str) {
    std::string res;
    const char* charflow = _env->GetStringUTFChars((jstring)str, nullptr);
    res = charflow;
    _env->ReleaseStringUTFChars((jstring)str, charflow);
    return res;
}

std::string JVMFunctionHelper::dumpExceptionString(jthrowable throwable) {
    std::stringstream ss;
    // toString
    jmethodID toString = getToStringMethod(_throwable_class);
    CHECK(toString != nullptr) << "Not Found JNI method toString";
    ss << to_string(throwable);

    // e.getStackTrace()
    jmethodID getStackTrace = _env->GetMethodID(_throwable_class, "getStackTrace", "()[Ljava/lang/StackTraceElement;");
    CHECK(getStackTrace != nullptr) << "Not Found JNI method getStackTrace";
    jobject stack_traces = _env->CallObjectMethod((jobject)throwable, getStackTrace);
    ss << array_to_string(stack_traces);
    _env->DeleteLocalRef(stack_traces);
    return ss.str();
}

jmethodID JVMFunctionHelper::getToStringMethod(jclass clazz) {
    return _env->GetMethodID(clazz, "toString", "()Ljava/lang/String;");
}

jstring JVMFunctionHelper::to_jstring(const std::string& str) {
    return _env->NewStringUTF(str.c_str());
}

jmethodID JVMFunctionHelper::getMethod(jclass clazz, const std::string& method, const std::string& sig) {
    return _env->GetMethodID(clazz, method.c_str(), sig.c_str());
}

jmethodID JVMFunctionHelper::getStaticMethod(jclass clazz, const std::string& method, const std::string& sig) {
    return _env->GetStaticMethodID(clazz, method.c_str(), sig.c_str());
}

jobject JVMFunctionHelper::create_array(int sz) {
    return _env->NewObjectArray(sz, _object_class, nullptr);
}

jobject JVMFunctionHelper::create_boxed_array(int type, int num_rows, bool nullable, DirectByteBuffer* buffs, int sz) {
    jobjectArray input_arr = _env->NewObjectArray(sz, _direct_buffer_class, nullptr);
    for (int i = 0; i < sz; ++i) {
        _env->SetObjectArrayElement(input_arr, i, buffs[i].handle());
    }
    jobject res =
            _env->CallStaticObjectMethod(_udf_helper_class, _create_boxed_array, type, num_rows, nullable, input_arr);
    if (_env->ExceptionCheck()) {
        LOG(WARNING) << "fail to create array " << this->dumpExceptionString(_env->ExceptionOccurred());
        _env->ExceptionClear();
    }
    _env->DeleteLocalRef(input_arr);
    return res;
}

jobject JVMFunctionHelper::create_object_array(jobject o, int num_rows) {
    jobjectArray res_arr = _env->NewObjectArray(num_rows, _object_array_class, o);
    return res_arr;
}

void JVMFunctionHelper::batch_update_single(FunctionContext* ctx, jobject udaf, jobject update, jobject state,
                                            jobject* input, int cols) {
    jobjectArray input_arr = _build_object_array(_object_array_class, input, cols);
    _env->CallStaticVoidMethod(_udf_helper_class, _batch_update_single, udaf, update, state, input_arr);
    _check_call_exception(ctx);
    _env->DeleteLocalRef(input_arr);
}

void JVMFunctionHelper::batch_update(FunctionContext* ctx, jobject udaf, jobject update, jobject* input, int cols) {
    jobjectArray input_arr = _build_object_array(_object_array_class, input, cols);
    _env->CallStaticVoidMethod(_udf_helper_class, _batch_update, udaf, update, input_arr);
    _check_call_exception(ctx);
    _env->DeleteLocalRef(input_arr);
}

jobject JVMFunctionHelper::batch_call(FunctionContext* ctx, jobject udf, jobject evaluate, jobject* input, int cols,
                                      int rows) {
    jobjectArray input_arr = _build_object_array(_object_array_class, input, cols);
    auto res = _env->CallStaticObjectMethod(_udf_helper_class, _batch_call, udf, evaluate, rows, input_arr);
    _check_call_exception(ctx);
    _env->DeleteLocalRef(input_arr);
    return res;
}

jobject JVMFunctionHelper::batch_call(FunctionContext* ctx, jobject caller, jobject method, int rows) {
    auto res = _env->CallStaticObjectMethod(_udf_helper_class, _batch_call_no_args, caller, method, rows);
    _check_call_exception(ctx);
    return res;
}

jobject JVMFunctionHelper::int_batch_call(FunctionContext* ctx, jobject callers, jobject method, int rows) {
    auto res = _env->CallStaticObjectMethod(_udf_helper_class, _int_batch_call, callers, method, rows);
    _check_call_exception(ctx);
    return res;
}

DEFINE_NEW_BOX(boolean, uint8_t, Boolean, Boolean);
DEFINE_NEW_BOX(byte, int8_t, Byte, Byte);
DEFINE_NEW_BOX(short, int16_t, Short, Short);
DEFINE_NEW_BOX(int, int32_t, Integer, Int);
DEFINE_NEW_BOX(long, int64_t, Long, Long);
DEFINE_NEW_BOX(float, float, Float, Float);
DEFINE_NEW_BOX(double, double, Double, Double);

// TODO:
jobject JVMFunctionHelper::newString(const char* data, size_t size) {
    auto bytesArr = _env->NewByteArray(size);
    _env->SetByteArrayRegion(bytesArr, 0, size, reinterpret_cast<const jbyte*>(data));
    jobject nstr = _env->NewObject(_string_class, _string_construct_with_bytes, bytesArr, _utf8_charsets);
    _env->DeleteLocalRef(bytesArr);
    return nstr;
}

size_t JVMFunctionHelper::string_length(jstring jstr) {
    return _env->GetStringUTFLength(jstr);
}

Slice JVMFunctionHelper::sliceVal(jstring jstr, std::string* buffer) {
    size_t length = this->string_length(jstr);
    buffer->resize(length);
    _env->GetStringUTFRegion(jstr, 0, length, buffer->data());
    return Slice(buffer->data(), buffer->length());
}

Slice JVMFunctionHelper::sliceVal(jstring jstr) {
    return Slice(_env->GetStringUTFChars(jstr, NULL));
}

std::string JVMFunctionHelper::to_jni_class_name(const std::string& name) {
    std::string jni_class_name;
    auto inserter = std::inserter(jni_class_name, jni_class_name.end());
    std::transform(name.begin(), name.end(), inserter, [](auto ele) {
        if (ele == '.') {
            return '/';
        }
        return ele;
    });
    return jni_class_name;
}

void JVMFunctionHelper::clear(DirectByteBuffer* buffer) {
    _env->CallNonvirtualVoidMethod(buffer->handle(), _direct_buffer_class, _direct_buffer_clear);
}

DirectByteBuffer::DirectByteBuffer(void* ptr, int capacity) {
    auto& helper = JVMFunctionHelper::getInstance();
    auto* env = helper.getEnv();
    _handle = env->NewDirectByteBuffer(ptr, capacity);
    _capacity = capacity;
    _data = ptr;
}

DirectByteBuffer::~DirectByteBuffer() {
    if (_handle != nullptr) {
        auto& helper = JVMFunctionHelper::getInstance();
        JNIEnv* env = helper.getEnv();
        env->DeleteLocalRef(_handle);
    }
}

JVMClass::~JVMClass() {
    if (_clazz) {
        JNIEnv* env = getJNIEnv();
        env->DeleteLocalRef(_clazz);
    }
}

Status JVMClass::newInstance(jobject* object) const {
    JNIEnv* env = getJNIEnv();
    // get default constructor
    jmethodID constructor = env->GetMethodID((jclass)_clazz, "<init>", "()V");
    if (constructor == nullptr) {
        return Status::InternalError("couldn't found default constructor for Java Object");
    }
    *object = env->NewObject((jclass)_clazz, constructor);
    return Status::OK();
}

ClassLoader::~ClassLoader() {
    JNIEnv* env = getJNIEnv();
    env->DeleteLocalRef(_handle);
}

constexpr const char* CLASS_LOADER_NAME = "com.starrocks.udf.UDFClassLoader";
constexpr const char* CLASS_ANALYZER_NAME = "com.starrocks.udf.UDFClassAnalyzer";

Status ClassLoader::init() {
    JNIEnv* env = JVMFunctionHelper::getInstance().getEnv();
    if (env == nullptr) {
        return Status::InternalError("Init JNIEnv fail");
    }
    std::string name = JVMFunctionHelper::to_jni_class_name(CLASS_LOADER_NAME);
    jclass clazz = env->FindClass(name.c_str());
    if (clazz == nullptr) {
        return Status::InternalError(fmt::format("ClassLoader Not Found: {}", CLASS_LOADER_NAME));
    }

    jmethodID udf_loader_contructor = env->GetMethodID(clazz, "<init>", "(Ljava/lang/String;)V");
    if (udf_loader_contructor == nullptr) {
        return Status::InternalError("ClassLoader constructor Not Found");
    }

    // create class loader instance
    jstring jstr = env->NewStringUTF(_path.c_str());
    _handle = env->NewObject(clazz, udf_loader_contructor, jstr);
    env->DeleteLocalRef(jstr);

    if (_handle == nullptr) {
        jthrowable throwable = env->ExceptionOccurred();
        std::string err_msg = fmt::format("Error: couldn't create class loader {} ", CLASS_LOADER_NAME);
        if (throwable) {
            err_msg = JVMFunctionHelper::getInstance().dumpExceptionString(throwable);
            env->DeleteLocalRef(throwable);
        }
        LOG(WARNING) << err_msg;
        return Status::InternalError(err_msg);
    }

    // init method id
    _get_class = env->GetMethodID(clazz, "findClass", "(Ljava/lang/String;)Ljava/lang/Class;");

    // init method
    if (_get_class == nullptr) {
        return Status::InternalError("couldn't get method for classloader");
    }

    return Status::OK();
}

JVMClass ClassLoader::getClass(const std::string& className) {
    auto& helper = JVMFunctionHelper::getInstance();
    JNIEnv* env = helper.getEnv();
    CHECK(env != nullptr) << "couldn't got a JNIEnv";

    // class Name java.lang.Object -> java/lang/Object
    std::string jni_class_name = JVMFunctionHelper::to_jni_class_name(className);
    // invoke class loader
    jstring jstr_name = helper.to_jstring(jni_class_name);

    auto loaded_clazz = env->CallObjectMethod(_handle, _get_class, jstr_name);

    env->DeleteLocalRef(jstr_name);
    // check exception
    jthrowable jthr = env->ExceptionOccurred();
    if (jthr) {
        LOG(WARNING) << "exception happened when get class:" << helper.dumpExceptionString(jthr);
        env->ExceptionClear();
        return JVMClass(nullptr);
    }
    // no exception happened, class exists
    DCHECK(loaded_clazz != nullptr);
    return loaded_clazz;
}

JavaMethodDescriptor::~JavaMethodDescriptor() {
    if (method) {
        JVMFunctionHelper::getInstance().getEnv()->DeleteLocalRef(method);
    }
}

jmethodID JavaMethodDescriptor::get_method_id() const {
    return JVMFunctionHelper::getInstance().getEnv()->FromReflectedMethod(method);
}

Status ClassAnalyzer::has_method(jclass clazz, const std::string& method, bool* has) {
    DCHECK(clazz != nullptr);
    DCHECK(has != nullptr);

    auto& helper = JVMFunctionHelper::getInstance();
    JNIEnv* env = getJNIEnv();

    std::string anlyzer_clazz_name = JVMFunctionHelper::to_jni_class_name(CLASS_ANALYZER_NAME);
    jclass class_analyzer = env->FindClass(anlyzer_clazz_name.c_str());
    if (class_analyzer == nullptr) {
        return Status::InternalError(fmt::format("ClassAnalyzer Not Found: {}", CLASS_ANALYZER_NAME));
    }

    jmethodID hasMethod =
            env->GetStaticMethodID(class_analyzer, "hasMemberMethod", "(Ljava/lang/String;Ljava/lang/Class;)Z");
    if (hasMethod == nullptr) {
        return Status::InternalError("couldn't found hasMethod method");
    }

    jstring method_name = helper.to_jstring(method.c_str());
    *has = env->CallStaticBooleanMethod(class_analyzer, hasMethod, method_name, (jobject)clazz);

    if (jthrowable jthr = env->ExceptionOccurred(); jthr) {
        std::string err = helper.dumpExceptionString(jthr);
        env->ExceptionClear();
        env->DeleteLocalRef(jthr);
        return Status::InternalError(fmt::format("call hasMemberMethod failed: {} err:{}", method, err));
    }

    return Status::OK();
}

Status ClassAnalyzer::get_signature(jclass clazz, const std::string& method, std::string* sign) {
    DCHECK(clazz != nullptr);
    DCHECK(sign != nullptr);
    auto& helper = JVMFunctionHelper::getInstance();
    JNIEnv* env = helper.getEnv();
    std::string anlyzer_clazz_name = JVMFunctionHelper::to_jni_class_name(CLASS_ANALYZER_NAME);
    jclass class_analyzer = env->FindClass(anlyzer_clazz_name.c_str());
    if (class_analyzer == nullptr) {
        return Status::InternalError(fmt::format("ClassAnalyzer Not Found: {}", CLASS_ANALYZER_NAME));
    }
    jmethodID getSign = env->GetStaticMethodID(class_analyzer, "getSignature",
                                               "(Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/String;");
    if (getSign == nullptr) {
        return Status::InternalError("couldn't found getSignature method");
    }

    jstring method_name = helper.to_jstring(method.c_str());
    jobject result_sign = env->CallStaticObjectMethod(class_analyzer, getSign, method_name, (jobject)clazz);
    if (jthrowable thr = env->ExceptionOccurred(); thr != nullptr) {
        std::string err = helper.dumpExceptionString(thr);
        env->ExceptionClear();
        env->DeleteLocalRef(thr);
        return Status::InternalError(fmt::format("could't found method {} err:{}", method, err));
    }
    if (result_sign == nullptr) {
        return Status::InternalError(fmt::format("couldn't found method:{}", method));
    }
    *sign = helper.to_string(result_sign);
    return Status::OK();
}

StatusOr<jobject> ClassAnalyzer::get_method_object(jclass clazz, const std::string& method) {
    auto& helper = JVMFunctionHelper::getInstance();
    JNIEnv* env = helper.getEnv();
    std::string anlyzer_clazz_name = JVMFunctionHelper::to_jni_class_name(CLASS_ANALYZER_NAME);
    jclass class_analyzer = env->FindClass(anlyzer_clazz_name.c_str());
    DCHECK(class_analyzer);
    jmethodID getMethodObject = env->GetStaticMethodID(
            class_analyzer, "getMethodObject", "(Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/reflect/Method;");
    DCHECK(getMethodObject);
    jstring method_name = helper.to_jstring(method.c_str());
    jobject method_object = env->CallStaticObjectMethod(class_analyzer, getMethodObject, method_name, (jobject)clazz);
    if (method_object == nullptr) {
        return Status::InternalError(fmt::format("couldn't found method:{}", method));
    }
    return method_object;
}

Status ClassAnalyzer::get_method_desc(const std::string& sign, std::vector<MethodTypeDescriptor>* desc) {
    RETURN_IF_ERROR(get_udaf_method_desc(sign, desc));
    // return type may be a void type
    for (int i = 1; i < desc->size(); ++i) {
        if (desc->at(i).type == INVALID_TYPE) {
            return Status::InternalError(fmt::format("unknown type sign:{}", sign));
        }
    }
    return Status::OK();
}

// clang-format off
#define ADD_BOXED_METHOD_TYPE_DESC(STR, TYPE) \
    } else if (type == STR) {                 \
      desc->emplace_back(MethodTypeDescriptor{TYPE, true});

#define ADD_PRIM_METHOD_TYPE_DESC(STR, TYPE) \
    } else if (sign[i] == STR) {             \
      desc->emplace_back(MethodTypeDescriptor{TYPE, false});
// clang-format on

Status ClassAnalyzer::get_udaf_method_desc(const std::string& sign, std::vector<MethodTypeDescriptor>* desc) {
    for (int i = 0; i < sign.size(); ++i) {
        if (sign[i] == '(' || sign[i] == ')') {
            continue;
        }
        if (sign[i] == '[') {
            while (sign[i] != ';') {
                i++;
            }
            // return Status::NotSupported("Not support Array Type");
            desc->emplace_back(MethodTypeDescriptor{INVALID_TYPE, true});
        }
        if (sign[i] == 'L') {
            int st = i + 1;
            while (sign[i] != ';') {
                i++;
            }
            std::string type = sign.substr(st, i - st);
            if (false) {
                // clang-format off
            ADD_BOXED_METHOD_TYPE_DESC("java/lang/Boolean", TYPE_BOOLEAN)
            ADD_BOXED_METHOD_TYPE_DESC("java/lang/Byte", TYPE_TINYINT)
            ADD_BOXED_METHOD_TYPE_DESC("java/lang/Short", TYPE_SMALLINT)
            ADD_BOXED_METHOD_TYPE_DESC("java/lang/Integer", TYPE_INT)
            ADD_BOXED_METHOD_TYPE_DESC("java/lang/Long", TYPE_BIGINT)
            ADD_BOXED_METHOD_TYPE_DESC("java/lang/Float", TYPE_FLOAT)
            ADD_BOXED_METHOD_TYPE_DESC("java/lang/Double", TYPE_DOUBLE)
                // clang-format on
            } else if (type == "java/lang/String") {
                desc->emplace_back(MethodTypeDescriptor{TYPE_VARCHAR, true});
            } else {
                desc->emplace_back(MethodTypeDescriptor{INVALID_TYPE, true});
            }
            continue;
        }
        if (false) {
            // clang-format off
        ADD_PRIM_METHOD_TYPE_DESC('Z', TYPE_BOOLEAN)
        ADD_PRIM_METHOD_TYPE_DESC('B', TYPE_TINYINT)
        ADD_PRIM_METHOD_TYPE_DESC('S', TYPE_SMALLINT)
        ADD_PRIM_METHOD_TYPE_DESC('I', TYPE_INT)
        ADD_PRIM_METHOD_TYPE_DESC('J', TYPE_BIGINT)
        ADD_PRIM_METHOD_TYPE_DESC('F', TYPE_FLOAT)
        ADD_PRIM_METHOD_TYPE_DESC('D', TYPE_DOUBLE)
            // clang-format on
        } else if (sign[i] == 'V') {
            desc->emplace_back(MethodTypeDescriptor{INVALID_TYPE, false});
        } else {
            desc->emplace_back(MethodTypeDescriptor{INVALID_TYPE, false});
        }
    }

    if (desc->size() > 1) {
        desc->insert(desc->begin(), (*desc)[desc->size() - 1]);
        desc->erase(desc->begin() + desc->size() - 1);
    }
    return Status::OK();
}

JavaUDFContext::~JavaUDFContext() {
    auto& helper = JVMFunctionHelper::getInstance();
    if (udf_handle) {
        helper.getEnv()->DeleteLocalRef(udf_handle);
    }
}

jobject UDAFFunction::create() {
    JNIEnv* env = getJNIEnv();
    jmethodID create = _ctx->create->get_method_id();
    return env->CallObjectMethod(_udaf_handle, create);
}

void UDAFFunction::destroy(jobject state) {
    JNIEnv* env = getJNIEnv();
    jmethodID destory = _ctx->destory->get_method_id();
    env->CallVoidMethod(_udaf_handle, destory, state);
    env->DeleteLocalRef(state);
}

jvalue UDAFFunction::finalize(jobject state) {
    auto& helper = JVMFunctionHelper::getInstance();
    JNIEnv* env = helper.getEnv();
    jmethodID finalize = _ctx->finalize->get_method_id();
    jvalue res;
    res.l = env->CallObjectMethod(_udaf_handle, finalize, state);
    return res;
}

void UDAFFunction::update(jvalue* val) {
    auto& helper = JVMFunctionHelper::getInstance();
    JNIEnv* env = helper.getEnv();
    jmethodID update = _ctx->update->get_method_id();
    env->CallVoidMethodA(_udaf_handle, update, val);
}

void UDAFFunction::merge(jobject state, jobject buffer) {
    JNIEnv* env = getJNIEnv();
    jmethodID merge = _ctx->merge->get_method_id();
    env->CallVoidMethod(_udaf_handle, merge, state, buffer);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
}

void UDAFFunction::serialize(jobject state, jobject buffer) {
    JNIEnv* env = getJNIEnv();
    jmethodID merge = _ctx->serialize->get_method_id();
    env->CallVoidMethod(_udaf_handle, merge, state, buffer);
}

int UDAFFunction::serialize_size(jobject state) {
    JNIEnv* env = getJNIEnv();
    jmethodID serialize_size = _ctx->serialize_size->get_method_id();
    return env->CallIntMethod(state, serialize_size);
}

void UDAFFunction::reset(jobject state) {
    JNIEnv* env = getJNIEnv();
    jmethodID reset = _ctx->reset->get_method_id();
    env->CallVoidMethod(_udaf_handle, reset, state);
}

jobject UDAFFunction::get_values(jobject state, int start, int end) {
    JNIEnv* env = getJNIEnv();
    jmethodID get_values = _ctx->get_values->get_method_id();
    return env->CallObjectMethod(_udaf_handle, get_values, state, start, end);
}

jobject UDAFFunction::window_update_batch(jobject state, int64_t peer_group_start, int64_t peer_group_end,
                                          int64_t frame_start, int64_t frame_end, int col_sz, jobject* cols) {
    JNIEnv* env = getJNIEnv();
    jmethodID window_update = _ctx->window_update->get_method_id();
    jvalue jvalues[5 + col_sz];
    jvalues[0].l = state;
    jvalues[1].j = peer_group_start;
    jvalues[2].j = peer_group_end;
    jvalues[3].j = frame_start;
    jvalues[4].j = frame_end;

    for (int i = 0; i < col_sz; ++i) {
        jvalues[i + 5].l = cols[i];
    }

    return env->CallObjectMethodA(_udaf_handle, window_update, jvalues);
}

} // namespace starrocks::vectorized