#include <memory>
#include <stdexcept>

#include <Python.h>
#include <llvm/IR/Verifier.h>

#include "translator.h"


using namespace std;
using namespace llvm;


void *Translator::operator()(Compiler &compiler, PyCodeObject *cpy_ir) {
    parseCFG(cpy_ir);
    entry_block = BasicBlock::Create(context, name("entry"), func);
    unwind_block = BasicBlock::Create(context, name("unwind"), func);
    for (auto &b : Range(block_num, &*ir_blocks)) {
        b = BasicBlock::Create(context, "", func);
    }

    builder.SetInsertPoint(entry_block);
    py_stack = builder.CreateAlloca(types.get<PyObject *>(),
            castToLLVMValue(cpy_ir->co_stacksize, types),
            name("stack")
    );
    stack_height_value = builder.CreateAlloca(types.get<decltype(stack_height)>(), castToLLVMValue(0, types));
    builder.CreateBr(ir_blocks[0]);

    for (auto &i : Range(block_num)) {
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

    PyInstrIter iter(py_instructions, size);
    BitArray is_boundary(size + 1);

    while (auto instr = iter.next()) {
        switch (instr.opcode) {
        case JUMP_ABSOLUTE:
        case JUMP_IF_TRUE_OR_POP:
        case JUMP_IF_FALSE_OR_POP:
        case POP_JUMP_IF_TRUE:
        case POP_JUMP_IF_FALSE:
        case JUMP_IF_NOT_EXC_MATCH:
            is_boundary.set(iter.getOffset());
            is_boundary.set(instr.oparg);
            break;
        case JUMP_FORWARD:
            is_boundary.set(iter.getOffset());
            is_boundary.set(iter.getOffset() + instr.oparg);
            break;
        case FOR_ITER:
            is_boundary.set(iter.getOffset() + instr.oparg);
            break;
        case RETURN_VALUE:
        case RAISE_VARARGS:
        case RERAISE:
            is_boundary.set(iter.getOffset());
        default:
            break;
        }
    }

    is_boundary.set(0, false);
    assert(is_boundary.get(size));

    block_num = is_boundary.count(size);
    boundaries.reserve(block_num);
    ir_blocks.reserve(block_num);

    unsigned current_num = 0;
    for (auto i : Range(BitArray::chunkNumber(size))) {
        auto bits = is_boundary[i];
        BitArray::ValueType tester{1};
        for (unsigned j = 0; j < BitArray::BitsPerValue; j++) {
            if (tester & bits) {
                boundaries[current_num++] = i * BitArray::BitsPerValue + j;
            }
            tester <<= 1;
        }
    }
    assert(current_num == block_num);
}


void Translator::do_Py_INCREF(Value *py_obj) {
    auto *const_1 = castToLLVMValue(decltype(PyObject::ob_refcnt){1}, types);
    auto ref = getPointer(py_obj, &PyObject::ob_refcnt);
    auto old_value = builder.CreateLoad(const_1->getType(), ref);
    builder.CreateStore(builder.CreateAdd(old_value, const_1), ref);
}

void Translator::do_Py_DECREF(Value *py_obj) {
    auto *const_1 = castToLLVMValue(decltype(PyObject::ob_refcnt){1}, types);
    auto ref = getPointer(py_obj, &PyObject::ob_refcnt);
    auto old_value = builder.CreateLoad(const_1->getType(), ref);
    builder.CreateStore(builder.CreateSub(old_value, const_1), ref);
}

Value *Translator::do_GETLOCAL(PyOparg oparg) {
    return readData<PyObject *>(py_fast_locals, oparg);
}

void Translator::do_SETLOCAL(PyOparg oparg, llvm::Value *value) {
    auto ptr = getPointer<PyObject *>(py_fast_locals, oparg);
    auto old_value = builder.CreateLoad(types.get<PyObject *>(), ptr);
    builder.CreateStore(value, ptr);
    auto not_null = builder.CreateICmpNE(old_value, null);
    do_if(not_null, [&]() {
        do_Py_DECREF(old_value);
    });
}

llvm::Value *Translator::do_POP() {
    auto value = readData<PyObject *>(py_stack, --stack_height);
    builder.CreateStore(castToLLVMValue(stack_height, types), stack_height_value);
    return value;
}

void Translator::do_PUSH(llvm::Value *value) {
    writeData<PyObject *>(py_stack, stack_height++, value);
    builder.CreateStore(castToLLVMValue(stack_height, types), stack_height_value);
}

void Translator::emitBlock(unsigned index) {
    builder.SetInsertPoint(ir_blocks[index]);
    bool fall_through = true;

    auto first = index ? boundaries[index - 1] : 0;
    auto last = boundaries[index];
    PyInstrIter instr_iter(py_instructions + first, last - first);
    while (auto instr = instr_iter.next()) {
        switch (instr.opcode) {
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
            builder.CreateCondBr(is_not_null, bb, unwind_block)
                    ->setMetadata(LLVMContext::MD_prof, likely_true);
            builder.SetInsertPoint(bb);
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
            auto res = do_Call(&SymbolTable::PyNumber_Multiply, left, right);
            do_Py_DECREF(left);
            do_Py_DECREF(right);
            do_PUSH(res);
            break;
        }
        case BINARY_ADD: {
            auto right = do_POP();
            auto left = do_POP();
            auto res = do_Call(&SymbolTable::PyNumber_Add, left, right);
            do_Py_DECREF(left);
            do_Py_DECREF(right);
            do_PUSH(res);
            break;
        }
        case BINARY_SUBTRACT: {
            auto right = do_POP();
            auto left = do_POP();
            auto res = do_Call(&SymbolTable::PyNumber_Subtract, left, right);
            do_Py_DECREF(left);
            do_Py_DECREF(right);
            do_PUSH(res);
            break;
        }
        case BINARY_TRUE_DIVIDE: {
            auto right = do_POP();
            auto left = do_POP();
            auto res = do_Call(&SymbolTable::PyNumber_TrueDivide, left, right);
            do_Py_DECREF(left);
            do_Py_DECREF(right);
            do_PUSH(res);
            break;
        }
        case BINARY_FLOOR_DIVIDE: {
            auto right = do_POP();
            auto left = do_POP();
            auto res = do_Call(&SymbolTable::PyNumber_FloorDivide, left, right);
            do_Py_DECREF(left);
            do_Py_DECREF(right);
            do_PUSH(res);
            break;
        }
        case BINARY_MODULO: {
            auto right = do_POP();
            auto left = do_POP();
            auto res = do_Call(&SymbolTable::PyNumber_Remainder, left, right);
            do_Py_DECREF(left);
            do_Py_DECREF(right);
            do_PUSH(res);
            break;
        }
        case COMPARE_OP: {
            auto right = do_POP();
            auto left = do_POP();
            auto res = do_Call(&SymbolTable::PyObject_RichCompare, left, right, castToLLVMValue(instr.oparg, types));
            do_Py_DECREF(left);
            do_Py_DECREF(right);
            do_PUSH(res);
            break;
        }
        case JUMP_ABSOLUTE: {
            auto dest = 0;
            auto i = distance(&*boundaries, lower_bound(&*boundaries, &boundaries[block_num], dest));
            builder.CreateBr(ir_blocks[i + 1]);
            fall_through = false;
            break;
        }
        case JUMP_FORWARD: {
            auto dest = instr_iter.getOffset() + instr.oparg / sizeof(PyInstr);
            auto i = distance(&*boundaries, lower_bound(&*boundaries, &boundaries[block_num], dest));
            builder.CreateBr(ir_blocks[i + i]);
            fall_through = false;
            break;
        }
        case POP_JUMP_IF_FALSE: {
            auto cond = do_POP();
            auto err = do_Call(&SymbolTable::PyObject_IsTrue, cond);
            do_Py_DECREF(cond);
            auto dest = instr_iter.getOffset() + instr.oparg / sizeof(PyInstr);
            auto i = distance(&*boundaries, lower_bound(&*boundaries, &boundaries[block_num], dest));
            auto cmp = builder.CreateICmpEQ(err, ConstantInt::get(err->getType(), 0));
            builder.CreateCondBr(cmp, ir_blocks[i + i], ir_blocks[index + 1]);
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
            auto dest = instr_iter.getOffset() + instr.oparg;
            auto i = distance(&*boundaries, lower_bound(&*boundaries, &boundaries[block_num], dest));
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
            builder.CreateBr(ir_blocks[i + 1]);
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
        builder.CreateBr(ir_blocks[index + 1]);
    }
}
