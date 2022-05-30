#ifndef PYNIC_SHARED_SYMBOLS
#define PYNIC_SHARED_SYMBOLS

#include <csetjmp>

#include <Python.h>
#include <frameobject.h>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DataLayout.h>

struct ExtendedCFrame : CFrame {
    jmp_buf frame_jmp_buf;
};

using PyInstr = const _Py_CODEUNIT;
using PyOpcode = decltype(_Py_OPCODE(PyInstr{}));
using PyOparg = decltype(_Py_OPCODE(PyInstr{}));
constexpr auto EXTENDED_ARG_BITS = 8;

void handle_dealloc(PyObject *obj);
void handle_INCREF(PyObject *obj);
void handle_DECREF(PyObject *obj);
void handle_XDECREF(PyObject *obj);
void raiseException();

PyObject *handle_LOAD_CLASSDEREF(PyFrameObject *f, PyOparg oparg);
PyObject *handle_LOAD_GLOBAL(PyFrameObject *f, PyObject *name);
void handle_STORE_GLOBAL(PyFrameObject *f, PyObject *name, PyObject *value);
void handle_DELETE_GLOBAL(PyFrameObject *f, PyObject *name);
PyObject *handle_LOAD_NAME(PyFrameObject *f, PyObject *name);
void handle_STORE_NAME(PyFrameObject *f, PyObject *name, PyObject *value);
void handle_DELETE_NAME(PyFrameObject *f, PyObject *name);
PyObject *handle_LOAD_ATTR(PyObject *owner, PyObject *name);
void handle_LOAD_METHOD(PyObject *obj, PyObject *name, PyObject **sp);
void handle_STORE_ATTR(PyObject *owner, PyObject *name, PyObject *value);
PyObject *handle_BINARY_SUBSCR(PyObject *container, PyObject *sub);
void handle_STORE_SUBSCR(PyObject *container, PyObject *sub, PyObject *value);

PyObject *handle_UNARY_NOT(PyObject *value);
PyObject *handle_UNARY_POSITIVE(PyObject *value);
PyObject *handle_UNARY_NEGATIVE(PyObject *value);
PyObject *handle_UNARY_INVERT(PyObject *value);

PyObject *handle_BINARY_ADD(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_ADD(PyObject *v, PyObject *w);
PyObject *handle_BINARY_SUBTRACT(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_SUBTRACT(PyObject *v, PyObject *w);
PyObject *handle_BINARY_MULTIPLY(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_MULTIPLY(PyObject *v, PyObject *w);
PyObject *handle_BINARY_FLOOR_DIVIDE(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_FLOOR_DIVIDE(PyObject *v, PyObject *w);
PyObject *handle_BINARY_TRUE_DIVIDE(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_TRUE_DIVIDE(PyObject *v, PyObject *w);
PyObject *handle_BINARY_MODULO(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_MODULO(PyObject *v, PyObject *w);
PyObject *handle_BINARY_POWER(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_POWER(PyObject *v, PyObject *w);
PyObject *handle_BINARY_MATRIX_MULTIPLY(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_MATRIX_MULTIPLY(PyObject *v, PyObject *w);
PyObject *handle_BINARY_LSHIFT(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_LSHIFT(PyObject *v, PyObject *w);
PyObject *handle_BINARY_RSHIFT(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_RSHIFT(PyObject *v, PyObject *w);
PyObject *handle_BINARY_AND(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_AND(PyObject *v, PyObject *w);
PyObject *handle_BINARY_OR(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_OR(PyObject *v, PyObject *w);
PyObject *handle_BINARY_XOR(PyObject *v, PyObject *w);
PyObject *handle_INPLACE_XOR(PyObject *v, PyObject *w);
PyObject *handle_COMPARE_OP(PyObject *v, PyObject *w, int op);
bool handle_CONTAINS_OP(PyObject *container, PyObject *value);

PyObject *handle_CALL_FUNCTION(PyObject **func_args, Py_ssize_t nargs);
PyObject *handle_CALL_FUNCTION_KW(PyObject **func_args, Py_ssize_t nargs);
PyObject *handle_CALL_FUNCTION_EX(PyObject *func, PyObject *args, PyObject *kwargs);
PyObject *handle_LOAD_BUILD_CLASS(PyObject *f);

PyObject *handle_IMPORT_NAME(PyFrameObject *f, PyObject *name, PyObject *fromlist, PyObject *level);
PyObject *handle_IMPORT_FROM(PyObject *from, PyObject *name);
void handle_IMPORT_STAR(PyFrameObject *f, PyObject *from);

PyObject *handle_BUILD_TUPLE(PyObject **arr, Py_ssize_t num);
PyObject *handle_BUILD_LIST(PyObject **arr, Py_ssize_t num);
PyObject *handle_BUILD_SET(PyObject **arr, Py_ssize_t num);
PyObject *handle_BUILD_MAP(PyObject **arr, Py_ssize_t num);
PyObject *handle_BUILD_CONST_KEY_MAP(PyObject **arr, Py_ssize_t num);
void handle_LIST_APPEND(PyObject *list, PyObject *value);
void handle_SET_ADD(PyObject *set, PyObject *value);
void handle_MAP_ADD(PyObject *map, PyObject *key, PyObject *value);
void handle_LIST_EXTEND(PyObject *list, PyObject *iterable);
void handle_SET_UPDATE(PyObject *set, PyObject *iterable);
void handle_DICT_UPDATE(PyObject *dict, PyObject *update);
void handle_DICT_MERGE(PyObject *func, PyObject *dict, PyObject *update);
PyObject *handle_LIST_TO_TUPLE(PyObject *list);

PyObject *handle_FORMAT_VALUE(PyObject *value, PyObject *fmt_spec, int which_conversion);
PyObject *handle_BUILD_STRING(PyObject **arr, Py_ssize_t num);

void handle_POP_EXCEPT(PyFrameObject *f);
bool handle_JUMP_IF_NOT_EXC_MATCH(PyObject *left, PyObject *right);
void handle_RERAISE(PyFrameObject *f, bool restore_lasti);
void handle_SETUP_WITH(PyFrameObject *f, PyObject **sp, int handler);
PyObject *handle_WITH_EXCEPT_START(PyObject *exc, PyObject *val, PyObject *tb, PyObject *exit_func);

bool castPyObjectToBool(PyObject *o);
PyObject *handle_GET_ITER(PyObject *o);

#define ENTRY(X) std::pair{&(X), #X}

// TODO: 命名规范，看看要不要大写
constexpr std::tuple external_symbols{
        ENTRY(handle_dealloc),
        ENTRY(handle_INCREF),
        ENTRY(handle_DECREF),
        ENTRY(handle_XDECREF),
        ENTRY(raiseException),
        ENTRY(handle_LOAD_CLASSDEREF),
        ENTRY(handle_LOAD_GLOBAL),
        ENTRY(handle_STORE_GLOBAL),
        ENTRY(handle_DELETE_GLOBAL),
        ENTRY(handle_LOAD_NAME),
        ENTRY(handle_STORE_NAME),
        ENTRY(handle_DELETE_NAME),
        ENTRY(handle_LOAD_ATTR),
        ENTRY(handle_LOAD_METHOD),
        ENTRY(handle_STORE_ATTR),
        ENTRY(handle_BINARY_SUBSCR),
        ENTRY(handle_STORE_SUBSCR),
        ENTRY(handle_UNARY_NOT),
        ENTRY(handle_UNARY_POSITIVE),
        ENTRY(handle_UNARY_NEGATIVE),
        ENTRY(handle_UNARY_INVERT),

        ENTRY(handle_BINARY_ADD),
        ENTRY(handle_INPLACE_ADD),
        ENTRY(handle_BINARY_SUBTRACT),
        ENTRY(handle_INPLACE_SUBTRACT),
        ENTRY(handle_BINARY_MULTIPLY),
        ENTRY(handle_INPLACE_MULTIPLY),
        ENTRY(handle_BINARY_FLOOR_DIVIDE),
        ENTRY(handle_INPLACE_FLOOR_DIVIDE),
        ENTRY(handle_BINARY_TRUE_DIVIDE),
        ENTRY(handle_INPLACE_TRUE_DIVIDE),
        ENTRY(handle_BINARY_MODULO),
        ENTRY(handle_INPLACE_MODULO),
        ENTRY(handle_BINARY_POWER),
        ENTRY(handle_INPLACE_POWER),
        ENTRY(handle_BINARY_MATRIX_MULTIPLY),
        ENTRY(handle_INPLACE_MATRIX_MULTIPLY),
        ENTRY(handle_BINARY_LSHIFT),
        ENTRY(handle_INPLACE_LSHIFT),
        ENTRY(handle_BINARY_RSHIFT),
        ENTRY(handle_INPLACE_RSHIFT),
        ENTRY(handle_BINARY_AND),
        ENTRY(handle_INPLACE_AND),
        ENTRY(handle_BINARY_OR),
        ENTRY(handle_INPLACE_OR),
        ENTRY(handle_BINARY_XOR),
        ENTRY(handle_INPLACE_XOR),
        ENTRY(handle_COMPARE_OP),
        ENTRY(handle_CONTAINS_OP),

        ENTRY(handle_CALL_FUNCTION),
        ENTRY(handle_CALL_FUNCTION_KW),
        ENTRY(handle_CALL_FUNCTION_EX),
        ENTRY(PyFunction_NewWithQualName),
        ENTRY(handle_LOAD_BUILD_CLASS),

        ENTRY(handle_IMPORT_NAME),
        ENTRY(handle_IMPORT_FROM),
        ENTRY(handle_IMPORT_STAR),

        ENTRY(handle_BUILD_STRING),
        ENTRY(handle_BUILD_TUPLE),
        ENTRY(handle_BUILD_LIST),
        ENTRY(handle_BUILD_SET),
        ENTRY(handle_BUILD_MAP),
        ENTRY(handle_BUILD_CONST_KEY_MAP),
        ENTRY(handle_LIST_APPEND),
        ENTRY(handle_SET_ADD),
        ENTRY(handle_MAP_ADD),
        ENTRY(handle_LIST_EXTEND),
        ENTRY(handle_SET_UPDATE),
        ENTRY(handle_DICT_UPDATE),
        ENTRY(handle_DICT_MERGE),
        ENTRY(handle_LIST_TO_TUPLE),

        ENTRY(handle_FORMAT_VALUE),

        ENTRY(PyFrame_BlockSetup),
        ENTRY(PyFrame_BlockPop),
        ENTRY(handle_POP_EXCEPT),
        ENTRY(handle_JUMP_IF_NOT_EXC_MATCH),
        ENTRY(handle_RERAISE),
        ENTRY(handle_SETUP_WITH),
        ENTRY(handle_WITH_EXCEPT_START),

        ENTRY(castPyObjectToBool),

        ENTRY(handle_GET_ITER),

        ENTRY(_Py_FalseStruct),
        ENTRY(_Py_TrueStruct)
};

constexpr auto external_symbol_count = std::tuple_size_v<decltype(external_symbols)>;

template <auto &A, auto &B>
struct IsSameSymbol {
    static constexpr bool value = false;
};

template <auto &A>
struct IsSameSymbol<A, A> {
    static constexpr bool value = true;
};

template <auto &V, size_t I = 0>
constexpr auto searchSymbol() {
    if constexpr (I < external_symbol_count) {
        if constexpr (IsSameSymbol<*std::get<I>(external_symbols).first, V>::value) {
            return I;
        } else {
            return searchSymbol<V, I + 1>();
        }
    }
}

extern const std::array<const char *, external_symbol_count> symbol_names;
extern const std::array<void *, external_symbol_count> symbol_addresses;
using CompiledFunction = PyObject *(void *const[], PyFrameObject *, ptrdiff_t);

template <typename T, typename = void>
struct Normalizer;

template <typename T>
using NormalizedType = typename Normalizer<std::remove_cv_t<T>>::type;

template <typename T>
struct Normalizer<T, std::enable_if_t<std::is_same_v<T, bool>>> {
    using type = bool;
};

template <typename T>
struct Normalizer<T, std::enable_if_t<!std::is_same_v<T, bool> && std::is_integral_v<T>>> {
    using type = std::make_signed_t<T>;
};

template <typename T>
struct Normalizer<T, std::enable_if_t<!std::is_scalar_v<T> && !std::is_function_v<T>>> {
    using type = void;
};

template <typename T>
struct Normalizer<T *> {
    using type = void *;
};

template <typename Ret, typename... Args>
struct Normalizer<Ret(Args...)> {
    using type = NormalizedType<Ret>(NormalizedType<Args>...);
};

template <typename T>
static auto createType(llvm::LLVMContext &context) {
    if constexpr(std::is_void<T>::value) {
        return llvm::Type::getVoidTy(context);
    }
    if constexpr(std::is_integral_v<T>) {
        if (std::is_same_v<T, bool>) {
            return llvm::Type::getInt1Ty(context);
        } else {
            return llvm::Type::getIntNTy(context, CHAR_BIT * sizeof(T));
        }
    }
    if constexpr(std::is_pointer_v<T>) {
        return llvm::PointerType::getUnqual(context);
    }
}

template <typename T, typename = void>
struct LLVMType {
    llvm::Type *type;

    LLVMType(llvm::LLVMContext &context, const llvm::DataLayout &dl) : type{createType<T>(context)} {}
};

template <typename T>
struct LLVMType<T, std::enable_if_t<std::is_scalar_v<T>>> {
    decltype(createType<T>(std::declval<llvm::LLVMContext &>())) type;
    llvm::Align align;

    LLVMType(llvm::LLVMContext &context, const llvm::DataLayout &dl) :
            type{createType<T>(context)}, align{dl.getABITypeAlign(type)} {}
};

template <typename Ret, typename... Args>
struct LLVMType<Ret(Args...)> {
    llvm::FunctionType *type;

    LLVMType(llvm::LLVMContext &context, const llvm::DataLayout &dl) : type{
            llvm::FunctionType::get(
                    createType<Ret>(context),
                    {createType<Args>(context)...},
                    false
            )
    } {}
};

template <typename T>
using NormalizedLLVMType = LLVMType<NormalizedType<T>>;


template <typename...>
struct TypeDeduplicator;

template <typename... Ts>
struct TypeDeduplicator<std::tuple<Ts...>> {
    using type = std::tuple<Ts...>;

    template <typename... Args>
    static auto create(Args &&... args) {
        return std::tuple{Ts{std::forward<Args>(args)...}...};
    }
};

template <typename... Ts, typename T, typename... Us>
struct TypeDeduplicator<std::tuple<Ts...>, T, Us...> : std::conditional_t<(
        std::is_same_v<Ts, T> || ...),
        TypeDeduplicator<std::tuple<Ts...>, Us...>,
        TypeDeduplicator<std::tuple<Ts..., T>, Us...>> {
};

template <typename...>
struct TypeDeduplicatorHelper;
template <typename... A, typename... B, typename... C>
struct TypeDeduplicatorHelper<std::tuple<A...>, const std::tuple<B...>, C...> : TypeDeduplicator<
        std::tuple<>,
        NormalizedLLVMType<A>...,
        NormalizedLLVMType<std::remove_pointer_t<typename B::first_type>>...,
        NormalizedLLVMType<std::remove_pointer_t<C>>...,
        NormalizedLLVMType<CompiledFunction>> {
};
using RegisteredTypes = TypeDeduplicatorHelper<
        std::tuple<void *, bool, char, short, int, long, long long>,
        decltype(external_symbols),
        decltype(PyTypeObject::tp_iternext)>;
#endif
