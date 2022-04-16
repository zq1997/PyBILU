#ifndef PYNIC_SHARED_SYMBOLS
#define PYNIC_SHARED_SYMBOLS

#include <Python.h>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>


struct SimplePyFrame {
    PyObject **consts;
    PyObject **localsplus;
    PyObject *builtins;
    PyObject *globals;
    PyObject *locals;
    PyCodeObject *code;
    PyObject **names;
    PyThreadState *tstate;
};

using PyInstr = const _Py_CODEUNIT;
using PyOpcode = decltype(_Py_OPCODE(PyInstr{}));
using PyOparg = decltype(_Py_OPCODE(PyInstr{}));
constexpr auto EXTENDED_ARG_BITS = 8;

PyObject *calcUnaryNot(PyObject *value);
PyObject *calcBinaryPower(PyObject *base, PyObject *exp);
PyObject *calcInPlacePower(PyObject *base, PyObject *exp);
PyObject *handle_LOAD_CLASSDEREF(SimplePyFrame *f, PyOparg oparg);
PyObject *handle_LOAD_GLOBAL(SimplePyFrame *f, PyOparg oparg);
PyObject *handle_LOAD_NAME(SimplePyFrame *f, PyOparg oparg);
PyObject *handle_LOAD_ATTR(SimplePyFrame *f, PyOparg oparg, PyObject *owner);
PyObject *handle_LOAD_METHOD(SimplePyFrame *f, PyOparg oparg, PyObject *obj, PyObject **sp);
PyObject *unwindFrame(PyObject **stack, ptrdiff_t stack_height);

constexpr std::tuple external_symbols{
        std::pair{&calcUnaryNot, "calcUnaryNot"},
        std::pair{&PyNumber_Positive, "PyNumber_Positive"},
        std::pair{&PyNumber_Negative, "PyNumber_Negative"},
        std::pair{&PyNumber_Invert, "PyNumber_Invert"},

        std::pair{&PyNumber_Add, "PyNumber_Add"},
        std::pair{&PyNumber_Subtract, "PyNumber_Subtract"},
        std::pair{&PyNumber_Multiply, "PyNumber_Multiply"},
        std::pair{&PyNumber_TrueDivide, "PyNumber_TrueDivide"},
        std::pair{&PyNumber_FloorDivide, "PyNumber_FloorDivide"},
        std::pair{&PyNumber_Remainder, "PyNumber_Remainder"},
        std::pair{&calcBinaryPower, "calcBinaryPower"},
        std::pair{&PyNumber_MatrixMultiply, "PyNumber_MatrixMultiply"},
        std::pair{&PyNumber_Lshift, "PyNumber_Lshift"},
        std::pair{&PyNumber_Rshift, "PyNumber_Rshift"},
        std::pair{&PyNumber_And, "PyNumber_And"},
        std::pair{&PyNumber_Or, "PyNumber_Or"},
        std::pair{&PyNumber_Xor, "PyNumber_Xor"},

        std::pair{&PyNumber_InPlaceAdd, "PyNumber_InPlaceAdd"},
        std::pair{&PyNumber_InPlaceSubtract, "PyNumber_InPlaceSubtract"},
        std::pair{&PyNumber_InPlaceMultiply, "PyNumber_InPlaceMultiply"},
        std::pair{&PyNumber_InPlaceTrueDivide, "PyNumber_InPlaceTrueDivide"},
        std::pair{&PyNumber_InPlaceFloorDivide, "PyNumber_InPlaceFloorDivide"},
        std::pair{&PyNumber_InPlaceRemainder, "PyNumber_InPlaceRemainder"},
        std::pair{&calcInPlacePower, "calcInPlacePower"},
        std::pair{&PyNumber_InPlaceMatrixMultiply, "PyNumber_InPlaceMatrixMultiply"},
        std::pair{&PyNumber_InPlaceLshift, "PyNumber_InPlaceLshift"},
        std::pair{&PyNumber_InPlaceRshift, "PyNumber_InPlaceRshift"},
        std::pair{&PyNumber_InPlaceAnd, "PyNumber_InPlaceAnd"},
        std::pair{&PyNumber_InPlaceOr, "PyNumber_InPlaceOr"},
        std::pair{&PyNumber_InPlaceXor, "PyNumber_InPlaceXor"},

        std::pair{&PyObject_GetIter, "PyObject_GetIter"},
        std::pair{&PyObject_IsTrue, "PyObject_IsTrue"},
        std::pair{&PyObject_RichCompare, "PyObject_RichCompare"},

        std::pair{&handle_LOAD_CLASSDEREF, "handle_LOAD_CLASSDEREF"},
        std::pair{&handle_LOAD_GLOBAL, "handle_LOAD_GLOBAL"},
        std::pair{&handle_LOAD_NAME, "handle_LOAD_NAME"},
        std::pair{&handle_LOAD_ATTR, "handle_LOAD_ATTR"},
        std::pair{&handle_LOAD_METHOD, "handle_LOAD_METHOD"},
        std::pair{&PyObject_GetItem, "PyObject_GetItem"},
        std::pair{&PyDict_SetItem, "PyDict_SetItem"},
        std::pair{&unwindFrame, "unwindFrame"},

        std::pair{&memmove, "memmove"}
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

template <typename T>
struct LLVMTypeImpl {
private:
    template <typename Ret, typename... Args>
    static auto createTypeForFunction(llvm::LLVMContext &context, TypeWrapper<Ret(Args...)>) {
        return llvm::FunctionType::get(
                LLVMTypeImpl<Ret>(context),
                {LLVMTypeImpl<Args>(context)...},
                false
        );
    }

    static auto createType(llvm::LLVMContext &context) {
        if constexpr(std::is_void<T>::value) {
            return llvm::Type::getVoidTy(context);
        }
        if constexpr(std::is_integral_v<T>) {
            return llvm::Type::getIntNTy(context, CHAR_BIT * sizeof(T));
        }
        if constexpr(std::is_same_v<T, double>) {
            return llvm::Type::getDoubleTy(context);
        }
        if constexpr(std::is_pointer_v<T>) {
            return llvm::PointerType::getUnqual(context);
        }
        if constexpr(std::is_function_v<T>) {
            return createTypeForFunction(context, TypeWrapper<T>{});
        }
    }

public:
    using DataType = std::invoke_result_t<decltype(createType), llvm::LLVMContext &>;
    DataType const data;
public:
    explicit LLVMTypeImpl(llvm::LLVMContext &context) : data(createType(context)) {}

    operator DataType() const { return data; }
};

template <typename T>
using LLVMType = LLVMTypeImpl<NormalizedType<T>>;

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

class RegisteredLLVMTypes {
    template <typename...>
    struct TypeFilterHelper;

    template <typename... A, typename... B, typename ...C>
    struct TypeFilterHelper<std::tuple<A...>, const std::tuple<B...>, C...> : TypeFilter<
            std::tuple<>,
            LLVMType<A>...,
            LLVMType<std::remove_pointer_t<typename B::first_type>>...,
            LLVMType<std::remove_pointer_t<C>>...
    > {
    };

    using filtered_types = TypeFilterHelper<
            std::tuple<void *, char, short, int, long, long long>,
            decltype(external_symbols),
            decltype(PyTypeObject::tp_iternext)
    >;
    filtered_types::type data;

public:
    explicit RegisteredLLVMTypes(llvm::LLVMContext &context) : data{filtered_types::create(context)} {}

    template <typename T>
    [[nodiscard]] typename LLVMType<T>::DataType get() const { return std::get<LLVMType<T>>(data); }
};

#endif
