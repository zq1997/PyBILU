#include <cstdlib>

#include <Python.h>

#include <llvm/IR/Function.h>


template <typename Ret, typename ...Args>
union SymbolEntry {
public:
    Ret (*func_addr)(Args...);
    llvm::FunctionType *func_type;

    explicit SymbolEntry(Ret(*addr)(Args...)) noexcept: func_addr{addr} {}
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

    static auto getEmpty() {
        SymbolTable instance;
        memset(&instance, 0, sizeof(instance));
        return instance;
    }
} extern const global_symbol_table;
