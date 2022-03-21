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

struct SymbolTable {
    decltype(::calcUnaryNot) *calcUnaryNot{::calcUnaryNot};
    decltype(::PyNumber_Positive) *PyNumber_Positive{::PyNumber_Positive};
    decltype(::PyNumber_Negative) *PyNumber_Negative{::PyNumber_Negative};
    decltype(::PyNumber_Invert) *PyNumber_Invert{::PyNumber_Invert};

    decltype(::PyNumber_Add) *PyNumber_Add{::PyNumber_Add};
    decltype(::PyNumber_Subtract) *PyNumber_Subtract{::PyNumber_Subtract};
    decltype(::PyNumber_Multiply) *PyNumber_Multiply{::PyNumber_Multiply};
    decltype(::PyNumber_TrueDivide) *PyNumber_TrueDivide{::PyNumber_TrueDivide};
    decltype(::PyNumber_FloorDivide) *PyNumber_FloorDivide{::PyNumber_FloorDivide};
    decltype(::PyNumber_Remainder) *PyNumber_Remainder{::PyNumber_Remainder};
    decltype(::calcBinaryPower) *calcBinaryPower{::calcBinaryPower};
    decltype(::PyNumber_MatrixMultiply) *PyNumber_MatrixMultiply{::PyNumber_MatrixMultiply};
    decltype(::PyNumber_Lshift) *PyNumber_Lshift{::PyNumber_Lshift};
    decltype(::PyNumber_Rshift) *PyNumber_Rshift{::PyNumber_Rshift};
    decltype(::PyNumber_And) *PyNumber_And{::PyNumber_And};
    decltype(::PyNumber_Or) *PyNumber_Or{::PyNumber_Or};
    decltype(::PyNumber_Xor) *PyNumber_Xor{::PyNumber_Xor};

    decltype(::PyNumber_InPlaceAdd) *PyNumber_InPlaceAdd{::PyNumber_InPlaceAdd};
    decltype(::PyNumber_InPlaceSubtract) *PyNumber_InPlaceSubtract{::PyNumber_InPlaceSubtract};
    decltype(::PyNumber_InPlaceMultiply) *PyNumber_InPlaceMultiply{::PyNumber_InPlaceMultiply};
    decltype(::PyNumber_InPlaceTrueDivide) *PyNumber_InPlaceTrueDivide{::PyNumber_InPlaceTrueDivide};
    decltype(::PyNumber_InPlaceFloorDivide) *PyNumber_InPlaceFloorDivide{::PyNumber_InPlaceFloorDivide};
    decltype(::PyNumber_InPlaceRemainder) *PyNumber_InPlaceRemainder{::PyNumber_InPlaceRemainder};
    decltype(::calcInPlacePower) *calcInPlacePower{::calcInPlacePower};
    decltype(::PyNumber_InPlaceMatrixMultiply) *PyNumber_InPlaceMatrixMultiply{::PyNumber_InPlaceMatrixMultiply};
    decltype(::PyNumber_InPlaceLshift) *PyNumber_InPlaceLshift{::PyNumber_InPlaceLshift};
    decltype(::PyNumber_InPlaceRshift) *PyNumber_InPlaceRshift{::PyNumber_InPlaceRshift};
    decltype(::PyNumber_InPlaceAnd) *PyNumber_InPlaceAnd{::PyNumber_InPlaceAnd};
    decltype(::PyNumber_InPlaceOr) *PyNumber_InPlaceOr{::PyNumber_InPlaceOr};
    decltype(::PyNumber_InPlaceXor) *PyNumber_InPlaceXor{::PyNumber_InPlaceXor};

    decltype(::PyObject_GetIter) *PyObject_GetIter{::PyObject_GetIter};
    decltype(::PyObject_IsTrue) *PyObject_IsTrue{::PyObject_IsTrue};
    decltype(::PyObject_RichCompare) *PyObject_RichCompare{::PyObject_RichCompare};

    decltype(::unwindFrame) *unwindFrame{::unwindFrame};
};

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
