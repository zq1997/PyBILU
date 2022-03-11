#include <iostream>

#include "JIT.h"

using namespace std;
using namespace llvm;

const SymbolTable global_symbol_table{};


llvm::CallInst *Translator::do_Call(FuncPtr SymbolTable::* entry, llvm::Type *result,
        llvm::ArrayRef<llvm::Type *> arg_types,
        std::initializer_list<llvm::Value *> args) {
    auto callee_type = FunctionType::get(result, arg_types, false);
    auto offset = calcDistance(global_symbol_table, global_symbol_table.*entry);
    auto func_ptr = readData(py_symbol_table, offset, callee_type->getPointerTo());
    return builder.CreateCall(callee_type, func_ptr, args);
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

Value *Translator::do_GETLOCAL(PyOparg oparg) {
    auto ptr = builder.CreateConstInBoundsGEP1_64(ctype_ptr, py_fast_locals, oparg);
    return builder.CreateLoad(ctype_ptr, ptr);
}

void Translator::do_SETLOCAL(PyOparg oparg, llvm::Value *value) {
    auto ptr = builder.CreateConstInBoundsGEP1_64(ctype_ptr, py_fast_locals, oparg);
    auto old_value = builder.CreateLoad(ctype_ptr, ptr);
    builder.CreateStore(value, ptr);
    auto b_decref = BasicBlock::Create(context, "", func);
    auto b_end = BasicBlock::Create(context, "", func);
    auto old_is_not_empty = builder.CreateICmpNE(old_value, ConstantPointerNull::get(ctype_ptr));
    builder.CreateCondBr(old_is_not_empty, b_decref, b_end);
    builder.SetInsertPoint(b_decref);
    do_Py_DECREF(old_value);
    builder.CreateBr(b_end);
    builder.SetInsertPoint(b_end);
}

llvm::Value *Translator::do_POP() {
    auto ptr = builder.CreateConstInBoundsGEP1_64(ctype_ptr, py_stack, --stack_height);
    return builder.CreateLoad(ctype_ptr, ptr);
}

void Translator::do_PUSH(llvm::Value *value) {
    auto ptr = builder.CreateConstInBoundsGEP1_64(ctype_ptr, py_stack, stack_height++);
    builder.CreateStore(value, ptr);
}

void Translator::emitBlock(unsigned index) {
    auto first = index ? boundaries[index - 1] : 0;
    auto last = boundaries[index];
    PyInstrIter iter(py_instructions + first, last - first);
    Type *arg_types[]{ctype_ptr, ctype_ptr};
    while (auto instr = iter.next()) {
        switch (instr.opcode) {
        case LOAD_FAST: {
            auto value = do_GETLOCAL(instr.oparg);
            do_Py_INCREF(value);
            do_PUSH(value);
            break;
        }
        case STORE_FAST: {
            auto value = do_POP();
            do_SETLOCAL(instr.oparg, value);
            break;
        }
        case BINARY_MULTIPLY: {
            auto right = do_POP();
            auto left = do_POP();
            auto res = do_Call(&SymbolTable::PyNumber_Multiply, ctype_ptr, arg_types, {left, right});
            do_Py_DECREF(left);
            do_Py_DECREF(right);
            do_PUSH(res);
            break;
        }
        case BINARY_ADD: {
            auto right = do_POP();
            auto left = do_POP();
            auto res = do_Call(&SymbolTable::PyNumber_Add, ctype_ptr, arg_types, {left, right});
            do_Py_DECREF(left);
            do_Py_DECREF(right);
            do_PUSH(res);
            break;
        }
        case RETURN_VALUE: {
            auto retval = do_POP();
            do_Py_INCREF(retval);
            builder.CreateRet(retval);
            break;
        }
        default:
            throw runtime_error("unsupported opcode");
        }
    }
}

