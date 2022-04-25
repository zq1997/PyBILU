#ifndef PYNIC_SHARED_SYMBOLS
#define PYNIC_SHARED_SYMBOLS

#include <csetjmp>

#include <Python.h>

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

void raiseException();
PyObject *handle_LOAD_CLASSDEREF(PyFrameObject *f, PyOparg oparg);
PyObject *handle_LOAD_GLOBAL(PyFrameObject *f, PyObject *name);
void handle_STORE_GLOBAL(PyFrameObject *f, PyObject *name, PyObject *value);
void handle_DELETE_GLOBAL(PyFrameObject *f, PyObject *name);
PyObject *handle_LOAD_NAME(PyFrameObject *f, PyObject *name);
void handle_STORE_NAME(PyFrameObject *f, PyObject *name, PyObject *value);
void handle_DELETE_NAME(PyFrameObject *f, PyObject *name);
PyObject *handle_LOAD_ATTR(PyObject *owner, PyObject *name);
PyObject *handle_LOAD_METHOD(PyObject *obj, PyObject *name, PyObject **sp);
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

PyObject *unwindFrame(PyObject **stack, ptrdiff_t stack_height);

#define ENTRY(X) std::pair{&(X), #X}

constexpr std::tuple external_symbols{
        ENTRY(raiseException),
        ENTRY(memmove),
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

        std::pair{&PyObject_GetIter, "PyObject_GetIter"},
        std::pair{&PyObject_IsTrue, "PyObject_IsTrue"},
        std::pair{&PyObject_RichCompare, "PyObject_RichCompare"},

        std::pair{&unwindFrame, "unwindFrame"},
};

constexpr auto external_symbol_count = std::tuple_size_v<decltype(external_symbols)>;

template <auto A, auto B>
constexpr auto isSameTypeAndValue() {
    if constexpr (std::is_same_v<decltype(A), decltype(B)>) {
        return A == B;
    } else {
        return false;
    }
}

template <auto &V, size_t I = 0>
constexpr auto searchSymbol() {
    static_assert(I < external_symbol_count, "invalid symbol");
    if constexpr (isSameTypeAndValue<std::get<I>(external_symbols).first, V>()) {
        return I;
    } else {
        return searchSymbol<V, I + 1>();
    }
}

using FunctionPointer = void (*)();
extern const std::array<const char *, external_symbol_count> symbol_names;
extern const std::array<FunctionPointer, external_symbol_count> symbol_addresses;
using TranslatedFunctionType = PyObject *(const FunctionPointer *, PyFrameObject *);

template <typename T>
struct TypeWrapper {
    using type = T;
};

template <typename T>
constexpr auto Normalizer();
template <typename T>
using NormalizedType = typename decltype(Normalizer<T>())::type;

template <typename Ret, typename... Args>
constexpr auto Normalizer(TypeWrapper<Ret(Args...)>) {
    return TypeWrapper<NormalizedType<Ret>(NormalizedType<Args>...)>{};
}

template <typename Ret, typename... Args>
constexpr auto Normalizer(TypeWrapper<Ret(Args...) noexcept>) {
    return TypeWrapper<NormalizedType<Ret>(NormalizedType<Args>...)>{};
}

template <typename T>
constexpr auto Normalizer() {
    if constexpr(std::is_const_v<T> || std::is_volatile_v<T>) {
        return TypeWrapper<NormalizedType<std::remove_cv_t<T>>>{};
    } else {
        if constexpr(std::is_fundamental_v<T>) {
            if constexpr(std::is_integral_v<T> && std::is_signed_v<T>) {
                return TypeWrapper<std::make_unsigned_t<T>>{};
            } else {
                return TypeWrapper<T>{};
            }
        }
        if constexpr(std::is_pointer_v<T>) {
            return TypeWrapper<void *>{}; // opaque pointer
        }
        if constexpr(std::is_function_v<T>) {
            return Normalizer(TypeWrapper<T>{});
        }
        // Return void here to raise a compile error for unsupported types
    }
}

template <typename...>
struct TypeFilter;

template <typename... Ts>
struct TypeFilter<std::tuple<Ts...>> {
    using type = std::tuple<Ts...>;

    template <typename... Args>
    static auto create(Args &&... args) {
        return std::tuple{Ts{std::forward<Args>(args)...}...};
    }
};

template <typename... Ts, typename T, typename... Us>
struct TypeFilter<std::tuple<Ts...>, T, Us...> : std::conditional_t<(
        std::is_same_v<Ts, T> || ...),
        TypeFilter<std::tuple<Ts...>, Us...>,
        TypeFilter<std::tuple<Ts..., T>, Us...>> {
};

template <typename T>
static auto createType(llvm::LLVMContext &context) {
    if constexpr(std::is_void<T>::value) {
        return llvm::Type::getVoidTy(context);
    }
    if constexpr(std::is_integral_v<T>) {
        return llvm::Type::getIntNTy(context, CHAR_BIT * sizeof(T));
    }
    if constexpr(std::is_pointer_v<T>) {
        return llvm::PointerType::getUnqual(context);
    }
}

template <typename Ret, typename... Args>
static auto createType(llvm::LLVMContext &context, TypeWrapper<Ret(Args...)>) {
    return llvm::FunctionType::get(
            createType<Ret>(context),
            {createType<Args>(context)...},
            false
    );
}

class RegisteredLLVMTypes {
    template <typename T, typename = void>
    struct LLVMType;

    template <typename T>
    struct LLVMType<T, std::enable_if_t<!std::is_function_v<T>>> {
        decltype(createType<T>(std::declval<llvm::LLVMContext &>())) type;
        llvm::Align align;

        LLVMType(llvm::LLVMContext &context, const llvm::DataLayout &dl) :
                type{createType<T>(context)}, align{dl.getABITypeAlign(type)} {}
    };

    template <typename T>
    struct LLVMType<T, std::enable_if_t<std::is_function_v<T>>> {
        llvm::FunctionType *type{};

        LLVMType(llvm::LLVMContext &context, const llvm::DataLayout &dl) : type{createType(context, TypeWrapper<T>{})} {}
    };

    template <typename T>
    using NormalizedLLVMType = LLVMType<NormalizedType<T>>;

    template <typename...>
    struct TypeFilterHelper;
    template <typename... A, typename... B, typename ...C>
    struct TypeFilterHelper<std::tuple<A...>, const std::tuple<B...>, C...> : TypeFilter<
            std::tuple<>,
            NormalizedLLVMType<A>...,
            NormalizedLLVMType<std::remove_pointer_t<typename B::first_type>>...,
            NormalizedLLVMType<std::remove_pointer_t<C>>...,
            NormalizedLLVMType<TranslatedFunctionType>> {
    };
    using FilteredTypes = TypeFilterHelper<
            std::tuple<void *, char, short, int, long, long long>,
            decltype(external_symbols),
            decltype(PyTypeObject::tp_iternext)
    >;

    FilteredTypes::type data;

public:
    template <typename... Args>
    explicit RegisteredLLVMTypes(Args &&... args) : data{FilteredTypes::create(std::forward<Args>(args)...)} {}

    template <typename T>
    [[nodiscard]] auto get() const { return std::get<NormalizedLLVMType<T>>(data).type; }

    template <typename T>
    [[nodiscard]] auto getAll() const { return std::get<NormalizedLLVMType<T>>(data); }
};

#endif
