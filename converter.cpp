#include <iostream>

#include "JIT.h"

using namespace std;
using namespace llvm;

const SymbolTable symbol_table{};

CallInst *createCall(Translator &jit, Value *table, void *const &func, Type *result,
                     ArrayRef<Type *> arg_types, initializer_list<Value *> args) {
    auto bf_type = FunctionType::get(result, arg_types, false);
    auto offset = reinterpret_cast<const char *>(&func) - reinterpret_cast<const char *>(&symbol_table);
    auto entry = jit.builder.CreateInBoundsGEP(jit.ctype_char, table,
            ConstantInt::get(jit.ctype_ptrdiff, offset));
    entry = jit.builder.CreatePointerCast(entry, bf_type->getPointerTo()->getPointerTo());
    entry = jit.builder.CreateLoad(bf_type->getPointerTo(), entry);
    return jit.builder.CreateCall(bf_type, entry, args);
}

void Translator::emitBlock(unsigned index) {
    auto first = index ? boundaries[index - 1] : 0;
    auto last = boundaries[index];
    PyInstrIter iter(py_instructions + first, last - first);

    auto l = builder.CreateLoad(ctype_ptr, builder.CreateConstInBoundsGEP1_32(ctype_ptr, py_fast_locals, 0));
    auto r = builder.CreateLoad(ctype_ptr, builder.CreateConstInBoundsGEP1_32(ctype_ptr, py_fast_locals, 1));
    Type *arg_types[]{ctype_ptr, ctype_ptr};
    auto ll = createCall(*this, py_symbol_table, symbol_table.PyNumber_Multiply, ctype_ptr, arg_types, {l, l});
    auto rr = createCall(*this, py_symbol_table, symbol_table.PyNumber_Multiply, ctype_ptr, arg_types, {r, r});
    auto ret = createCall(*this, py_symbol_table, symbol_table.PyNumber_Add, ctype_ptr, arg_types, {ll, rr});
    builder.CreateRet(ret);
}

