#include <memory>
#include <stdexcept>

#include <Python.h>
#include <llvm/IR/Verifier.h>

#include "translator.h"


using namespace std;
using namespace llvm;


void *Translator::operator()(Compiler &compiler, PyCodeObject *cpy_ir) {
    parseCFG(cpy_ir);
    for (auto &b : Range(block_num, &*blocks)) {
        b.llvm_block = BasicBlock::Create(context, "", func);
    }
    unwind_block = BasicBlock::Create(context, name("unwind"), func);

    builder.SetInsertPoint(blocks[0].llvm_block);
    py_stack = builder.CreateAlloca(types.get<PyObject *>(),
            asValue(cpy_ir->co_stacksize),
            name("stack")
    );
    stack_height_value = builder.CreateAlloca(types.get<decltype(stack_height)>());
    builder.CreateBr(blocks[1].llvm_block);

    for (auto &i : Range(block_num - 1, 1U)) {
        emitBlock(i);
    }

    builder.SetInsertPoint(unwind_block);
    builder.CreateRet(do_Call(&SymbolTable::unwindFrame, py_stack,
            builder.CreateLoad(types.get<ptrdiff_t>(), stack_height_value)));

    assert(!verifyFunction(*func, &errs()));
    auto result = compiler(mod);

    func->dropAllReferences();

    return result;
}

void Translator::parseCFG(PyCodeObject *cpy_ir) {
    py_instructions = reinterpret_cast<PyInstr *>(PyBytes_AS_STRING(cpy_ir->co_code));
    auto size = PyBytes_GET_SIZE(cpy_ir->co_code) / sizeof(PyInstr);

    BitArray is_boundary(size + 1);

    for (QuickPyInstrIter instr(py_instructions, size); instr.next();) {
        switch (instr.opcode) {
        case JUMP_ABSOLUTE:
        case JUMP_IF_TRUE_OR_POP:
        case JUMP_IF_FALSE_OR_POP:
        case POP_JUMP_IF_TRUE:
        case POP_JUMP_IF_FALSE:
        case JUMP_IF_NOT_EXC_MATCH:
            is_boundary.set(instr.offset);
            is_boundary.set(instr.getOparg());
            break;
        case JUMP_FORWARD:
            is_boundary.set(instr.offset);
            is_boundary.set(instr.offset + instr.getOparg());
            break;
        case FOR_ITER:
            is_boundary.set(instr.offset + instr.getOparg());
            break;
        case RETURN_VALUE:
        case RAISE_VARARGS:
        case RERAISE:
            is_boundary.set(instr.offset);
        default:
            break;
        }
    }

    is_boundary.set(0);
    assert(is_boundary.get(size));

    blocks.reserve(block_num = is_boundary.count(size));

    unsigned current_num = 0;
    for (auto i : Range(BitArray::chunkNumber(size))) {
        auto bits = is_boundary[i];
        BitArray::ValueType tester{1};
        for (unsigned j = 0; j < BitArray::BitsPerValue; j++) {
            if (tester & bits) {
                blocks[current_num++].end = i * BitArray::BitsPerValue + j;
            }
            tester <<= 1;
        }
    }
    assert(current_num == block_num);
}


void Translator::do_Py_INCREF(Value *py_obj) {
    auto *const_1 = asValue(decltype(PyObject::ob_refcnt){1});
    auto ref = getPointer(py_obj, &PyObject::ob_refcnt);
    auto old_value = builder.CreateLoad(const_1->getType(), ref);
    builder.CreateStore(builder.CreateAdd(old_value, const_1), ref);
}

void Translator::do_Py_DECREF(Value *py_obj) {
    auto *const_1 = asValue(decltype(PyObject::ob_refcnt){1});
    auto ref = getPointer(py_obj, &PyObject::ob_refcnt);
    auto old_value = builder.CreateLoad(const_1->getType(), ref);
    builder.CreateStore(builder.CreateSub(old_value, const_1), ref);
}

Value *Translator::do_GETLOCAL(PyOparg oparg) {
    return readData<PyObject *>(py_fast_locals, oparg);
}

void Translator::do_SETLOCAL(PyOparg oparg, llvm::Value *value, bool check_null) {
    auto ptr = getPointer<PyObject *>(py_fast_locals, oparg);
    auto old_value = builder.CreateLoad(types.get<PyObject *>(), ptr);
    builder.CreateStore(value, ptr);
    if (check_null) {
        auto not_null = builder.CreateICmpNE(old_value, null);
        do_if(not_null, [&]() {
            do_Py_DECREF(old_value);
        });
    } else {
        do_Py_DECREF(old_value);
    }
}

llvm::Value *Translator::do_POP() {
    auto value = readData<PyObject *>(py_stack, --stack_height);
    builder.CreateStore(asValue(stack_height), stack_height_value);
    return value;
}

void Translator::do_PUSH(llvm::Value *value) {
    writeData<PyObject *>(py_stack, stack_height++, value);
    builder.CreateStore(asValue(stack_height), stack_height_value);
}

void Translator::emitBlock(unsigned index) {
    builder.SetInsertPoint(blocks[index].llvm_block);
    bool fall_through = true;

    assert(index);
    auto first = blocks[index - 1].end;
    auto last = blocks[index].end;
    for (PyInstrIter instr(py_instructions + first, last - first); instr.next();) {
        switch (instr.opcode) {
        case EXTENDED_ARG:
            instr.extend_current_oparg();
        case NOP: {
            break;
        }
        case POP_TOP: {
            auto value = do_POP();
            do_Py_DECREF(value);
            break;
        }
        case LOAD_CONST: {
            auto value = readData<PyObject *>(py_consts, instr.oparg);
            do_Py_INCREF(value);
            do_PUSH(value);
            break;
        }
        case LOAD_FAST: {
            auto value = do_GETLOCAL(instr.oparg);
            auto is_not_null = builder.CreateICmpNE(value, null);
            auto bb = BasicBlock::Create(context, "", func);
            // TODO: 看看下一个参数什么意思，Unpredictable
            builder.CreateCondBr(is_not_null, bb, unwind_block, likely_true);
            builder.SetInsertPoint(bb);
            do_Py_INCREF(value);
            do_PUSH(value);
            break;
        }
        case STORE_FAST: {
            auto value = do_POP();
            do_SETLOCAL(instr.oparg, value);
            break;
        };
        case DELETE_FAST: {
            auto value = do_GETLOCAL(instr.oparg);
            auto is_not_null = builder.CreateICmpNE(value, null);
            auto bb = BasicBlock::Create(context, "", func);
            builder.CreateCondBr(is_not_null, bb, unwind_block, likely_true);
            do_SETLOCAL(instr.oparg, null, false);
            do_PUSH(value);
            break;
        }
        case UNARY_NOT:
            handle_UNARY_OP(&SymbolTable::calcUnaryNot);
            break;
        case UNARY_POSITIVE:
            handle_UNARY_OP(&SymbolTable::PyNumber_Positive);
            break;
        case UNARY_NEGATIVE:
            handle_UNARY_OP(&SymbolTable::PyNumber_Negative);
            break;
        case UNARY_INVERT:
            handle_UNARY_OP(&SymbolTable::PyNumber_Invert);
            break;
        case BINARY_ADD:
            handle_BINARY_OP(&SymbolTable::PyNumber_Add);
            break;
        case BINARY_SUBTRACT:
            handle_BINARY_OP(&SymbolTable::PyNumber_Subtract);
            break;
        case BINARY_MULTIPLY:
            handle_BINARY_OP(&SymbolTable::PyNumber_Multiply);
            break;
        case BINARY_TRUE_DIVIDE:
            handle_BINARY_OP(&SymbolTable::PyNumber_TrueDivide);
            break;
        case BINARY_FLOOR_DIVIDE:
            handle_BINARY_OP(&SymbolTable::PyNumber_FloorDivide);
            break;
        case BINARY_MODULO:
            handle_BINARY_OP(&SymbolTable::PyNumber_Remainder);
            break;
        case BINARY_POWER:
            handle_BINARY_OP(&SymbolTable::calcBinaryPower);
            break;
        case BINARY_MATRIX_MULTIPLY:
            handle_BINARY_OP(&SymbolTable::PyNumber_MatrixMultiply);
            break;
        case BINARY_LSHIFT:
            handle_BINARY_OP(&SymbolTable::PyNumber_Lshift);
            break;
        case BINARY_RSHIFT:
            handle_BINARY_OP(&SymbolTable::PyNumber_Rshift);
            break;
        case BINARY_AND:
            handle_BINARY_OP(&SymbolTable::PyNumber_And);
            break;
        case BINARY_OR:
            handle_BINARY_OP(&SymbolTable::PyNumber_Or);
            break;
        case BINARY_XOR:
            handle_BINARY_OP(&SymbolTable::PyNumber_Xor);
            break;
        case INPLACE_ADD:
            handle_BINARY_OP(&SymbolTable::PyNumber_InPlaceAdd);
            break;
        case INPLACE_SUBTRACT:
            handle_BINARY_OP(&SymbolTable::PyNumber_InPlaceSubtract);
            break;
        case INPLACE_MULTIPLY:
            handle_BINARY_OP(&SymbolTable::PyNumber_InPlaceMultiply);
            break;
        case INPLACE_TRUE_DIVIDE:
            handle_BINARY_OP(&SymbolTable::PyNumber_InPlaceTrueDivide);
            break;
        case INPLACE_FLOOR_DIVIDE:
            handle_BINARY_OP(&SymbolTable::PyNumber_InPlaceFloorDivide);
            break;
        case INPLACE_MODULO:
            handle_BINARY_OP(&SymbolTable::PyNumber_InPlaceRemainder);
            break;
        case INPLACE_POWER:
            handle_BINARY_OP(&SymbolTable::calcInPlacePower);
            break;
        case INPLACE_MATRIX_MULTIPLY:
            handle_BINARY_OP(&SymbolTable::PyNumber_InPlaceMatrixMultiply);
            break;
        case INPLACE_LSHIFT:
            handle_BINARY_OP(&SymbolTable::PyNumber_InPlaceLshift);
            break;
        case INPLACE_RSHIFT:
            handle_BINARY_OP(&SymbolTable::PyNumber_InPlaceRshift);
            break;
        case INPLACE_AND:
            handle_BINARY_OP(&SymbolTable::PyNumber_InPlaceAnd);
            break;
        case INPLACE_OR:
            handle_BINARY_OP(&SymbolTable::PyNumber_InPlaceOr);
            break;
        case INPLACE_XOR:
            handle_BINARY_OP(&SymbolTable::PyNumber_InPlaceXor);
            break;
        case COMPARE_OP: {
            auto right = do_POP();
            auto left = do_POP();
            auto res = do_Call(&SymbolTable::PyObject_RichCompare, left, right, asValue(instr.oparg));
            do_Py_DECREF(left);
            do_Py_DECREF(right);
            do_PUSH(res);
            break;
        }
        case JUMP_ABSOLUTE: {
            auto jmp_target = findBlock(instr.oparg);
            builder.CreateBr(jmp_target);
            fall_through = false;
            break;
        }
        case JUMP_FORWARD: {
            auto jmp_target = findBlock(instr.oparg + instr.oparg);
            builder.CreateBr(jmp_target);
            fall_through = false;
            break;
        }
        case POP_JUMP_IF_TRUE: {
            auto cond = do_POP();
            auto err = do_Call(&SymbolTable::PyObject_IsTrue, cond);
            do_Py_DECREF(cond);
            auto cmp = builder.CreateICmpNE(err, ConstantInt::get(err->getType(), 0));
            auto jmp_target = findBlock(instr.oparg);
            builder.CreateCondBr(cmp, jmp_target, blocks[index + 1].llvm_block);
            fall_through = false;
            break;
        }
        case POP_JUMP_IF_FALSE: {
            auto cond = do_POP();
            auto err = do_Call(&SymbolTable::PyObject_IsTrue, cond);
            do_Py_DECREF(cond);
            auto cmp = builder.CreateICmpEQ(err, ConstantInt::get(err->getType(), 0));
            auto jmp_target = findBlock(instr.oparg);
            builder.CreateCondBr(cmp, jmp_target, blocks[index + 1].llvm_block);
            fall_through = false;
            break;
        }
        case GET_ITER: {
            auto iterable = do_POP();
            auto iter = do_Call(&SymbolTable::PyObject_GetIter, iterable);
            do_Py_DECREF(iterable);
            do_PUSH(iter);
            break;
        }
        case FOR_ITER: {
            auto iter = do_POP();
            auto the_type = readData(iter, &PyObject::ob_type);
            auto tp_iternext = readData(the_type, &PyTypeObject::tp_iternext);
            auto next = do_Call<decltype(PyTypeObject::tp_iternext)>(tp_iternext, iter);
            auto cmp = builder.CreateICmpEQ(next, ConstantPointerNull::get(types.get<PyObject *>()));
            auto b_continue = BasicBlock::Create(context, "", func);
            auto b_break = BasicBlock::Create(context, "", func);
            builder.CreateCondBr(cmp, b_break, b_continue);
            builder.SetInsertPoint(b_break);
            do_Py_DECREF(iter);
            builder.CreateBr(findBlock(instr.offset + instr.oparg));
            builder.SetInsertPoint(b_continue);
            do_PUSH(iter);
            do_PUSH(next);
            fall_through = false;
            break;
        }
        case RETURN_VALUE: {
            auto retval = do_POP();
            do_Py_INCREF(retval);
            builder.CreateRet(retval);
            fall_through = false;
            break;
        }
        default:
            throw runtime_error("unsupported opcode");
        }
    }
    if (fall_through) {
        assert(index < block_num - 1);
        builder.CreateBr(blocks[index + 1].llvm_block);
    }
}

BasicBlock *Translator::findBlock(unsigned instr_offset) {
    assert(block_num > 1);
    decltype(block_num) left = 0;
    auto right = block_num - 1;
    while (left <= right) {
        auto mid = left + (right - left) / 2;
        auto v = blocks[mid].end;
        if (v < instr_offset) {
            left = mid + 1;
        } else if (v > instr_offset) {
            assert(mid > 0);
            right = mid - 1;
        } else {
            return blocks[mid].llvm_block;
        }
    }
    assert(false);
    return nullptr;
}
