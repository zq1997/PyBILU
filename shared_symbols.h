#include <cstdlib>

#include <Python.h>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>

using PyInstr = const _Py_CODEUNIT;
using PyOpcode = decltype(_Py_OPCODE(PyInstr{}));
using PyOparg = decltype(_Py_OPCODE(PyInstr{}));
constexpr auto EXTENDED_ARG_BITS = 8;

struct SymbolTable {
    decltype(::PyNumber_Add) *PyNumber_Add{::PyNumber_Add};
    decltype(::PyNumber_Subtract) *PyNumber_Subtract{::PyNumber_Subtract};
    decltype(::PyNumber_Multiply) *PyNumber_Multiply{::PyNumber_Multiply};
    decltype(::PyNumber_TrueDivide) *PyNumber_TrueDivide{::PyNumber_TrueDivide};
    decltype(::PyNumber_FloorDivide) *PyNumber_FloorDivide{::PyNumber_FloorDivide};
    decltype(::PyNumber_Remainder) *PyNumber_Remainder{::PyNumber_Remainder};
    decltype(::PyObject_RichCompare) *PyObject_RichCompare{::PyObject_RichCompare};
    decltype(::PyObject_GetIter) *PyObject_GetIter{::PyObject_GetIter};
    decltype(::PyObject_IsTrue) *PyObject_IsTrue{::PyObject_IsTrue};
} extern const shared_symbol_table;

template <typename T>
struct TypeWrapper {
    using type = T;
};

template <typename T>
constexpr auto NormalizeType();
template <typename T>
using NormalizedType = typename decltype(NormalizeType<T>())::type;

template <typename Ret, typename... Args>
constexpr auto NormalizeType(TypeWrapper<Ret(Args...)>) {
    return TypeWrapper<NormalizedType<Ret>(NormalizedType<Args>...)>{};
}

template <typename T>
constexpr auto NormalizeType() {
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
            return TypeWrapper<std::add_pointer_t<NormalizedType<std::remove_pointer_t<T>>>>{};
        }
        if constexpr(std::is_function_v<T>) {
            return NormalizeType(TypeWrapper<T>{});
        }
        if constexpr(std::is_class_v<T>) {
            return TypeWrapper<unsigned char>{};
        }
    }
}


template <typename T>
struct LLVMType {
private:
    template <typename Ret, typename... Args>
    static auto createTypeForFunction(llvm::LLVMContext &context, TypeWrapper<Ret(Args...)>) {
        return llvm::FunctionType::get(
                LLVMType<Ret>(context)(),
                {LLVMType<Args>(context)()...},
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
            return LLVMType<std::remove_pointer_t<T>>(context)()->getPointerTo();
        }
        if constexpr(std::is_function_v<T>) {
            return createTypeForFunction(context, TypeWrapper<T>{});
        }
    }

    std::invoke_result_t<decltype(createType), llvm::LLVMContext &> const data;
public:
    explicit LLVMType(llvm::LLVMContext &context) : data(createType(context)) {}

    auto &operator()() const { return data; }
};


template <typename T, typename...>
struct UniqueTuple;

template <typename... Ts>
struct UniqueTuple<std::tuple<Ts...>> {
    using type = std::tuple<Ts...>;

    template <typename... Args>
    static auto create(Args &&... args) {
        return std::tuple{Ts{std::forward<Args>(args)...}...};
    }
};

template <typename... Ts, typename T, typename... Us>
struct UniqueTuple<std::tuple<Ts...>, T, Us...> :
        std::conditional_t<(std::is_same_v<Ts, T> || ...),
                UniqueTuple<std::tuple<Ts...>, Us...>,
                UniqueTuple<std::tuple<T, Ts...>, Us...>> {
};

template <template <typename> typename W, typename... Ts>
class WrappedUniqueTuple {
    using maker = UniqueTuple<std::tuple<>, W<Ts>...>;
public:
    typename maker::type data;

    template <typename... Args>
    explicit WrappedUniqueTuple(Args &&... args) : data{maker::create(std::forward<Args>(args)...)} {}

    template <typename T>
    [[nodiscard]] auto &get() const { return std::get<W<T>>(data)(); }
};

template <typename T>
using NormalizedLLVMType = LLVMType<NormalizedType<T>>;
using RegisteredLLVMTypes = WrappedUniqueTuple<NormalizedLLVMType,
        PyOparg,
        ptrdiff_t,
        PyObject,
        PyObject *,
        int(PyObject *),
        PyObject *(PyObject *),
        PyObject *(PyObject *, PyObject *),
        PyObject *(PyObject *, PyObject *, int)
>;


template <typename T>
std::enable_if_t<std::is_integral_v<T>, llvm::Value *>
castToLLVMValue(T t, RegisteredLLVMTypes &types) {
    return llvm::ConstantInt::get(types.get<T>(), t);
}

template <typename T>
std::enable_if_t<std::is_base_of_v<llvm::Value, std::remove_pointer_t<T>>, llvm::Value *>
castToLLVMValue(T t, RegisteredLLVMTypes &types) {
    return t;
}

