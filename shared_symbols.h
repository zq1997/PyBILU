#include <cstdlib>

#include <Python.h>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>

template <typename T>
auto getType(llvm::LLVMContext &context) {
    if constexpr(std::is_pointer_v<T>) {
        return llvm::Type::getIntNPtrTy(context, CHAR_BIT);
    } else {
        if constexpr(std::is_integral_v<T>) {
            return llvm::Type::getIntNTy(context, CHAR_BIT * sizeof(T));
        } else {
            return;
        }
    }
}

template <typename Ret, typename ...Args>
auto getFunctionType(Ret (*)(Args...), llvm::LLVMContext &context) {
    return llvm::FunctionType::get(
            getType<Ret>(context),
            {getType<Args>(context)...},
            false
    );
}

template <typename T>
union SymbolEntry {
private:
    friend struct SymbolTable;
    T func_pointer;
    llvm::FunctionType *func_type;
public:
    explicit SymbolEntry(T pointer) noexcept: func_pointer{pointer} {}
};

struct SymbolTable {
    decltype(SymbolEntry{::PyNumber_Add}) PyNumber_Add{::PyNumber_Add};
    decltype(SymbolEntry{::PyNumber_Multiply}) PyNumber_Multiply{::PyNumber_Multiply};
    decltype(SymbolEntry{::PyObject_GetIter}) PyObject_GetIter{::PyObject_GetIter};
    decltype(SymbolEntry{::PyObject_IsTrue}) PyObject_IsTrue{::PyObject_IsTrue};
    decltype(SymbolEntry{::PyObject_RichCompare}) PyObject_RichCompare{::PyObject_RichCompare};
    decltype(SymbolEntry{::PyNumber_Subtract}) PyNumber_Substract{::PyNumber_Subtract};
    decltype(SymbolEntry{::PyNumber_TrueDivide}) PyNumber_TrueDivide{::PyNumber_TrueDivide};
    decltype(SymbolEntry{::PyNumber_FloorDivide}) PyNumber_FloorDivide{::PyNumber_FloorDivide};
    decltype(SymbolEntry{::PyNumber_Remainder}) PyNumber_Remainder{::PyNumber_Remainder};

    template <typename T>
    auto getType(SymbolEntry<T> SymbolTable::* entry, SymbolTable &cache, llvm::LLVMContext &context) const {
        auto &cache_entry = cache.*entry;
        if (cache_entry.func_type) {
            return cache_entry.func_type;
        } else {
            return cache_entry.func_type = getFunctionType((this->*entry).func_pointer, context);
        }
    }

    static auto getEmpty() {
        SymbolTable instance;
        memset(&instance, 0, sizeof(instance));
        return instance;
    }
} extern const global_symbol_table;


template <typename T>
std::enable_if_t<std::is_integral_v<T>, llvm::Value *>
castToLLVMValue(T t, llvm::LLVMContext &context) {
    return llvm::ConstantInt::get(getType<T>(context), t);
}

template <typename T>
std::enable_if_t<std::is_base_of_v<llvm::Value, std::remove_pointer_t<T>>, llvm::Value *>
castToLLVMValue(T t, llvm::LLVMContext &context) {
    return t;
}
