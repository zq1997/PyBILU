#include <Python.h>

#include <llvm/IR/Function.h>


template <typename Ret, typename ...Args>
union SymbolEntry {
public:
    Ret (*func_addr)(Args...);
    llvm::FunctionType *func_type;

    explicit SymbolEntry(Ret(*addr)(Args...)) noexcept: func_addr{addr} {}

    auto &operator=(const SymbolEntry &other) noexcept {
        assert(this != &other);
        func_type = nullptr;
        return *this;
    }
};

struct SymbolTable {
    decltype(SymbolEntry{::PyNumber_Add}) PyNumber_Add{::PyNumber_Add};
    decltype(SymbolEntry{::PyNumber_Add}) PyNumber_Multiply{::PyNumber_Multiply};
    decltype(SymbolEntry{::PyObject_GetIter}) PyObject_GetIter{::PyObject_GetIter};
    decltype(SymbolEntry{::PyObject_IsTrue}) PyObject_IsTrue{::PyObject_IsTrue};
    decltype(SymbolEntry{::PyObject_RichCompare}) PyObject_RichCompare{::PyObject_RichCompare};
} extern const global_symbol_table;
