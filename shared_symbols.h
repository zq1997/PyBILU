#ifndef PYNIC_SHARED_SYMBOLS
#define PYNIC_SHARED_SYMBOLS

#include <Python.h>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>

using PyInstr = const _Py_CODEUNIT;
using PyOpcode = decltype(_Py_OPCODE(PyInstr{}));
using PyOparg = decltype(_Py_OPCODE(PyInstr{}));
constexpr auto EXTENDED_ARG_BITS = 8;

PyObject *calcUnaryNot(PyObject *value);
PyObject *calcBinaryPower(PyObject *base, PyObject *exp);
PyObject *calcInPlacePower(PyObject *base, PyObject *exp);
PyObject *unwindFrame(PyObject **stack, ptrdiff_t stack_height);

#define DEFINE_SYMBOLS(F) \
    F(calcUnaryNot),\
    F(PyNumber_Positive),\
    F(PyNumber_Negative),\
    F(PyNumber_Invert),\
    \
    F(PyNumber_Add),\
    F(PyNumber_Subtract),\
    F(PyNumber_Multiply),\
    F(PyNumber_TrueDivide),\
    F(PyNumber_FloorDivide),\
    F(PyNumber_Remainder),\
    F(calcBinaryPower),\
    F(PyNumber_MatrixMultiply),\
    F(PyNumber_Lshift),\
    F(PyNumber_Rshift),\
    F(PyNumber_And),\
    F(PyNumber_Or),\
    F(PyNumber_Xor),\
    \
    F(PyNumber_InPlaceAdd),\
    F(PyNumber_InPlaceSubtract),\
    F(PyNumber_InPlaceMultiply),\
    F(PyNumber_InPlaceTrueDivide),\
    F(PyNumber_InPlaceFloorDivide),\
    F(PyNumber_InPlaceRemainder),\
    F(calcInPlacePower),\
    F(PyNumber_InPlaceMatrixMultiply),\
    F(PyNumber_InPlaceLshift),\
    F(PyNumber_InPlaceRshift),\
    F(PyNumber_InPlaceAnd),\
    F(PyNumber_InPlaceOr),\
    F(PyNumber_InPlaceXor),\
    \
    F(PyObject_GetIter),\
    F(PyObject_IsTrue),\
    F(PyObject_RichCompare),\
    \
    F(unwindFrame)

enum ExternalSymbol {
#define F(X) sym_##X
    DEFINE_SYMBOLS(F),
#undef F
    _symbol_count
};
constexpr size_t symbol_count = _symbol_count;

using SymbolTypeTuple = std::tuple<DEFINE_SYMBOLS(decltype)>;
template <ExternalSymbol S>
using SymbolType = typename std::tuple_element_t<S, SymbolTypeTuple>;

using FunctionPointer = void (*)();
extern const char *const symbol_names[symbol_count];
extern FunctionPointer const symbol_addresses[symbol_count];

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
                LLVMTypeImpl<Ret>(context)(),
                {LLVMTypeImpl<Args>(context)()...},
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

    std::invoke_result_t<decltype(createType), llvm::LLVMContext &> const data;
public:
    explicit LLVMTypeImpl(llvm::LLVMContext &context) : data(createType(context)) {}

    auto &operator()() const { return data; }
};

template <typename T>
using LLVMType = LLVMTypeImpl<NormalizedType<T>>;

template <typename T, typename...>
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
struct TypeFilter<std::tuple<Ts...>, T, Us...> :
        std::conditional_t<(std::is_same_v<Ts, T> || ...),
                TypeFilter<std::tuple<Ts...>, Us...>,
                TypeFilter<std::tuple<T, Ts...>, Us...>> {
};

template <template <typename> typename W, typename... Ts>
class TypeRegisister {
    using filter = TypeFilter<std::tuple<>, W<Ts>...>;
public:
    typename filter::type data;

    template <typename... Args>
    explicit TypeRegisister(Args &&... args) : data{filter::create(std::forward<Args>(args)...)} {}

    template <typename T>
    [[nodiscard]] auto &get() const { return std::get<W<T>>(data)(); }
};

using RegisteredLLVMTypes = TypeRegisister<LLVMType,
        void *,
        char,
        int,
        ptrdiff_t,
        PyOparg,
        Py_ssize_t,
        int(PyObject *),
        PyObject *(PyObject *),
        PyObject *(PyObject *, PyObject *),
        PyObject *(PyObject *, PyObject *, PyObject *),
        PyObject *(PyObject *, PyObject *, int),
        decltype(unwindFrame)
>;

#endif
