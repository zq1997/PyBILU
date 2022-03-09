#include <iostream>

#include "JIT.h"

using namespace std;
using namespace llvm;

const SymbolTable symbol_table{};


CallInst *createCall(Translator &jit, Value *table, void *const &func, Type *result,
        ArrayRef<Type *> arg_types, initializer_list<Value *> args) {
    auto bf_type = FunctionType::get(result, arg_types, false);
    auto offset = reinterpret_cast<const char *>(&func) - reinterpret_cast<const char *>(&symbol_table);
    auto entry = jit.builder.CreateInBoundsGEP(jit.ctype_data, table,
            ConstantInt::get(jit.ctype_ptrdiff, offset));
    entry = jit.builder.CreatePointerCast(entry, bf_type->getPointerTo()->getPointerTo());
    entry = jit.builder.CreateLoad(bf_type->getPointerTo(), entry);
    return jit.builder.CreateCall(bf_type, entry, args);
}

void Translator::do_Py_INCREF(Value *py_obj) {
    auto ref = builder.CreatePointerCast(
            getMemberPointer(&PyObject::ob_refcnt, py_obj),
            ctype_objref->getPointerTo()
    );
    builder.CreateStore(builder.CreateAdd(builder.CreateLoad(ctype_objref, ref), cvalue_objref_1), ref);
}

void Translator::do_Py_DECREF(Value *py_obj) {
    auto ref = builder.CreatePointerCast(
            getMemberPointer(&PyObject::ob_refcnt, py_obj),
            ctype_objref->getPointerTo()
    );
    builder.CreateStore(builder.CreateSub(builder.CreateLoad(ctype_objref, ref), cvalue_objref_1), ref);
}

void Translator::emitBlock(unsigned index) {
    auto first = index ? boundaries[index - 1] : 0;
    auto last = boundaries[index];
    PyInstrIter iter(py_instructions + first, last - first);
    int sp = 0;
    Type *arg_types[]{ctype_ptr, ctype_ptr};
    while (auto instr = iter.next()) {
        switch (instr.opcode) {
        case LOAD_FAST: {
            auto v = builder.CreateLoad(ctype_ptr,
                    builder.CreateConstInBoundsGEP1_32(ctype_ptr, py_fast_locals, instr.oparg));
            builder.CreateStore(v,
                    builder.CreateConstInBoundsGEP1_32(ctype_ptr, py_stack, sp++));
            do_Py_INCREF(v);
            break;
        }
        case STORE_FAST: {
            auto old_v = builder.CreateLoad(ctype_ptr,
                    builder.CreateConstInBoundsGEP1_32(ctype_ptr, py_fast_locals, instr.oparg));
            auto v = builder.CreateLoad(ctype_ptr,
                    builder.CreateConstInBoundsGEP1_32(ctype_ptr, py_stack, --sp));
            builder.CreateStore(v,
                    builder.CreateConstInBoundsGEP1_32(ctype_ptr, py_fast_locals, instr.oparg));
            do_Py_DECREF(old_v);
            break;
        }
        case BINARY_MULTIPLY: {
            auto r = builder.CreateLoad(ctype_ptr,
                    builder.CreateConstInBoundsGEP1_32(ctype_ptr, py_stack, --sp));
            auto l = builder.CreateLoad(ctype_ptr,
                    builder.CreateConstInBoundsGEP1_32(ctype_ptr, py_stack, --sp));
            auto v = createCall(*this, py_symbol_table, symbol_table.PyNumber_Multiply, ctype_ptr, arg_types, {l, r});
            do_Py_DECREF(l);
            do_Py_DECREF(r);
            builder.CreateStore(v,
                    builder.CreateConstInBoundsGEP1_32(ctype_ptr, py_stack, sp++));
            break;
        }
        case BINARY_ADD: {
            auto r = builder.CreateLoad(ctype_ptr,
                    builder.CreateConstInBoundsGEP1_32(ctype_ptr, py_stack, --sp));
            auto l = builder.CreateLoad(ctype_ptr,
                    builder.CreateConstInBoundsGEP1_32(ctype_ptr, py_stack, --sp));
            auto v = createCall(*this, py_symbol_table, symbol_table.PyNumber_Add, ctype_ptr, arg_types, {l, r});
            do_Py_DECREF(l);
            do_Py_DECREF(r);
            builder.CreateStore(v,
                    builder.CreateConstInBoundsGEP1_32(ctype_ptr, py_stack, sp++));
            break;
        }
        case RETURN_VALUE: {
            auto v = builder.CreateLoad(ctype_ptr,
                    builder.CreateConstInBoundsGEP1_32(ctype_ptr, py_stack, --sp));
            do_Py_INCREF(v);
            builder.CreateRet(v);
            break;
        }
        default:
            throw runtime_error("unsupported opcode");
        }
    }
}

