#include <memory>
#include <stdexcept>

#include <Python.h>

#include "translator.h"


using namespace std;
using namespace llvm;

Translator::Translator() {
    auto attr_builder = AttrBuilder(context);
    external_func_attr = AttributeList::get(context, AttributeList::FunctionIndex, attr_builder
            .addAttribute(Attribute::InaccessibleMemOnly)
            .addAttribute(Attribute::WillReturn)
            .addAttribute(Attribute::NoUnwind)
    );
    attr_builder.clear();

    func->setAttributes(
            AttributeList::get(context, AttributeList::FunctionIndex, attr_builder
                    .addAttribute(Attribute::NoUnwind)
                    .addAttribute("tune-cpu", sys::getHostCPUName())));
    func->getArg(0)->setName(useName("$symbols"));
    func->getArg(1)->setName(useName("$locals"));
    func->getArg(2)->setName(useName("$consts"));
    for (auto &arg : func->args()) {
        arg.addAttr(Attribute::NoAlias);
    }
}

void *Translator::operator()(Compiler &compiler, PyCodeObject *code) {
    py_code = code;
    parseCFG();

    blocks[0].llvm_block = createBlock("$entry_block", func);
    unwind_block = createBlock("$unwind_block", nullptr);
    auto start = blocks[0].end;
    for (auto &b : Range(block_num - 1, &blocks[1])) {
        b.llvm_block = createBlock(start);
        start = b.end;
    }

    builder.SetInsertPoint(blocks[0].llvm_block);
    py_locals.reserve(code->co_nlocals);
    for (auto i : Range(code->co_nlocals)) {
        auto name = PyTuple_GET_ITEM(code->co_varnames, i);
        py_locals[i] = getPointer<PyObject *>(func->getArg(1), i, useName("$local.", name));
    }

    py_consts.reserve(PyTuple_GET_SIZE(code->co_consts));
    for (auto i : Range(PyTuple_GET_SIZE(code->co_consts))) {
        py_consts[i] = getPointer<PyObject *>(func->getArg(2), i, useName("$const.", i));
    }

    py_stack.reserve(code->co_stacksize);
    auto stack_space = builder.CreateAlloca(types.get<PyObject *>(),
            asValue(code->co_stacksize),
            useName("$stack")
    );
    for (auto i : Range(code->co_stacksize)) {
        py_stack[i] = getPointer<PyObject *>(stack_space, i, useName("$stack.", i));
    }
    stack_height_value = builder.CreateAlloca(types.get<decltype(stack_height)>());

    for (auto &i : Range(block_num - 1, 1U)) {
        emitBlock(i);
    }


    unwind_block->insertInto(func);
    builder.SetInsertPoint(unwind_block);
    auto sp = builder.CreateLoad(types.get<size_t>(), stack_height_value);
    auto b_check_empty = createBlock(useName("check_stack_empty"), func);
    builder.CreateBr(b_check_empty);

    builder.SetInsertPoint(b_check_empty);
    auto phi_node = builder.CreatePHI(sp->getType(), 2);
    phi_node->addIncoming(sp, unwind_block);
    auto stack_is_empty = builder.CreateICmpEQ(phi_node, ConstantInt::get(types.get<size_t>(), 0));
    auto b_pop_stack = createBlock(useName("pop_stack"), func);
    auto end_block = createBlock(useName("return_error"), func);
    builder.CreateCondBr(stack_is_empty, end_block, b_pop_stack);

    builder.SetInsertPoint(b_pop_stack);
    auto new_sp = builder.CreateSub(sp, ConstantInt::get(types.get<size_t>(), 1));
    auto obj_ref = builder.CreateInBoundsGEP(types.get<PyObject *>(), py_stack[0], new_sp);
    auto obj = builder.CreateLoad(types.get<PyObject *>(), obj_ref);
    do_Py_DECREF(obj);
    phi_node->addIncoming(new_sp, b_pop_stack);
    builder.CreateBr(b_check_empty);

    builder.SetInsertPoint(end_block);
    builder.CreateRet(null);

    builder.SetInsertPoint(blocks[0].llvm_block);
    builder.CreateBr(blocks[1].llvm_block);

    auto result = compiler(mod);

    // 可能需要考虑mod.dropAllReferences();不复用函数了，改成复用module，甚至都不复用
    func->dropAllReferences();

    return result;
}

void Translator::parseCFG() {
    py_instructions = reinterpret_cast<PyInstr *>(PyBytes_AS_STRING(py_code->co_code));
    auto size = PyBytes_GET_SIZE(py_code->co_code) / sizeof(PyInstr);

    BitArray is_boundary(size + 1);

    for (QuickPyInstrIter instr(py_instructions, 0, size); instr.next();) {
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
    auto varname = PyTuple_GET_ITEM(py_code->co_varnames, oparg);
    return builder.CreateLoad(types.get<PyObject *>(), py_locals[oparg], useName(varname));
}

void Translator::do_SETLOCAL(PyOparg oparg, llvm::Value *value, bool check_null) {
    auto old_value = do_GETLOCAL(oparg);
    builder.CreateStore(value, py_locals[oparg]);
    if (check_null) {
        auto not_null = builder.CreateICmpNE(old_value, null);
        auto b_decref = createBlock(useName("$decref"), func);
        auto b_end = createBlock(useName("$decref.end"), func);
        builder.CreateCondBr(not_null, b_decref, b_end);
        builder.SetInsertPoint(b_decref);
        do_Py_DECREF(old_value);
        builder.CreateBr(b_end);
        builder.SetInsertPoint(b_end);
    } else {
        do_Py_DECREF(old_value);
    }
}

llvm::Value *Translator::do_POP() {
    auto value = builder.CreateLoad(types.get<PyObject *>(), py_stack[--stack_height]);
    builder.CreateStore(asValue(stack_height), stack_height_value);
    return value;
}

void Translator::do_PUSH(llvm::Value *value) {
    builder.CreateStore(value, py_stack[stack_height++]);
    builder.CreateStore(asValue(stack_height), stack_height_value);
}

void Translator::emitBlock(unsigned index) {
    blocks[index].llvm_block->insertInto(func);
    builder.SetInsertPoint(blocks[index].llvm_block);
    bool fall_through = true;

    assert(index);
    auto first = blocks[index - 1].end;
    auto last = blocks[index].end;
    for (PyInstrIter instr(py_instructions, first, last); instr.next();) {
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
            auto value = builder.CreateLoad(types.get<PyObject *>(), py_consts[instr.oparg]);
            do_Py_INCREF(value);
            do_PUSH(value);
            break;
        }
        case LOAD_FAST: {
            auto value = do_GETLOCAL(instr.oparg);
            auto is_not_null = builder.CreateICmpNE(value, null);
            auto bb = createBlock(instr.offset, "LOAD_FAST.OK");
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
        }
        case DELETE_FAST: {
            auto value = do_GETLOCAL(instr.oparg);
            auto is_not_null = builder.CreateICmpNE(value, null);
            auto bb = createBlock(instr.offset, "DELETE_FAST.OK");
            builder.CreateCondBr(is_not_null, bb, unwind_block, likely_true);
            do_SETLOCAL(instr.oparg, null, false);
            do_PUSH(value);
            break;
        }
        case UNARY_NOT:
            handle_UNARY_OP(searchSymbol<calcUnaryNot>());
            break;
        case UNARY_POSITIVE:
            handle_UNARY_OP(searchSymbol<PyNumber_Positive>());
            break;
        case UNARY_NEGATIVE:
            handle_UNARY_OP(searchSymbol<PyNumber_Negative>());
            break;
        case UNARY_INVERT:
            handle_UNARY_OP(searchSymbol<PyNumber_Invert>());
            break;
        case BINARY_ADD:
            handle_BINARY_OP(searchSymbol<PyNumber_Add>());
            break;
        case BINARY_SUBTRACT:
            handle_BINARY_OP(searchSymbol<PyNumber_Subtract>());
            break;
        case BINARY_MULTIPLY:
            handle_BINARY_OP(searchSymbol<PyNumber_Multiply>());
            break;
        case BINARY_TRUE_DIVIDE:
            handle_BINARY_OP(searchSymbol<PyNumber_TrueDivide>());
            break;
        case BINARY_FLOOR_DIVIDE:
            handle_BINARY_OP(searchSymbol<PyNumber_FloorDivide>());
            break;
        case BINARY_MODULO:
            handle_BINARY_OP(searchSymbol<PyNumber_Remainder>());
            break;
        case BINARY_POWER:
            handle_BINARY_OP(searchSymbol<calcBinaryPower>());
            break;
        case BINARY_MATRIX_MULTIPLY:
            handle_BINARY_OP(searchSymbol<PyNumber_MatrixMultiply>());
            break;
        case BINARY_LSHIFT:
            handle_BINARY_OP(searchSymbol<PyNumber_Lshift>());
            break;
        case BINARY_RSHIFT:
            handle_BINARY_OP(searchSymbol<PyNumber_Rshift>());
            break;
        case BINARY_AND:
            handle_BINARY_OP(searchSymbol<PyNumber_And>());
            break;
        case BINARY_OR:
            handle_BINARY_OP(searchSymbol<PyNumber_Or>());
            break;
        case BINARY_XOR:
            handle_BINARY_OP(searchSymbol<PyNumber_Xor>());
            break;
        case INPLACE_ADD:
            handle_BINARY_OP(searchSymbol<PyNumber_InPlaceAdd>());
            break;
        case INPLACE_SUBTRACT:
            handle_BINARY_OP(searchSymbol<PyNumber_InPlaceSubtract>());
            break;
        case INPLACE_MULTIPLY:
            handle_BINARY_OP(searchSymbol<PyNumber_InPlaceMultiply>());
            break;
        case INPLACE_TRUE_DIVIDE:
            handle_BINARY_OP(searchSymbol<PyNumber_InPlaceTrueDivide>());
            break;
        case INPLACE_FLOOR_DIVIDE:
            handle_BINARY_OP(searchSymbol<PyNumber_InPlaceFloorDivide>());
            break;
        case INPLACE_MODULO:
            handle_BINARY_OP(searchSymbol<PyNumber_InPlaceRemainder>());
            break;
        case INPLACE_POWER:
            handle_BINARY_OP(searchSymbol<calcInPlacePower>());
            break;
        case INPLACE_MATRIX_MULTIPLY:
            handle_BINARY_OP(searchSymbol<PyNumber_InPlaceMatrixMultiply>());
            break;
        case INPLACE_LSHIFT:
            handle_BINARY_OP(searchSymbol<PyNumber_InPlaceLshift>());
            break;
        case INPLACE_RSHIFT:
            handle_BINARY_OP(searchSymbol<PyNumber_InPlaceRshift>());
            break;
        case INPLACE_AND:
            handle_BINARY_OP(searchSymbol<PyNumber_InPlaceAnd>());
            break;
        case INPLACE_OR:
            handle_BINARY_OP(searchSymbol<PyNumber_InPlaceOr>());
            break;
        case INPLACE_XOR:
            handle_BINARY_OP(searchSymbol<PyNumber_InPlaceXor>());
            break;
        case COMPARE_OP: {
            auto right = do_POP();
            auto left = do_POP();
            auto res = do_CallSymbol<PyObject_RichCompare>(left, right, asValue(instr.oparg));
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
            auto err = do_CallSymbol<PyObject_IsTrue>(cond);
            do_Py_DECREF(cond);
            auto cmp = builder.CreateICmpNE(err, asValue(0));
            auto jmp_target = findBlock(instr.oparg);
            builder.CreateCondBr(cmp, jmp_target, blocks[index + 1].llvm_block);
            fall_through = false;
            break;
        }
        case POP_JUMP_IF_FALSE: {
            auto cond = do_POP();
            auto err = do_CallSymbol<PyObject_IsTrue>(cond);
            do_Py_DECREF(cond);
            auto cmp = builder.CreateICmpEQ(err, asValue(0));
            auto jmp_target = findBlock(instr.oparg);
            builder.CreateCondBr(cmp, jmp_target, blocks[index + 1].llvm_block);
            fall_through = false;
            break;
        }
        case GET_ITER: {
            auto iterable = do_POP();
            auto iter = do_CallSymbol<PyObject_GetIter>(iterable);
            do_Py_DECREF(iterable);
            do_PUSH(iter);
            break;
        }
        case FOR_ITER: {
            auto iter = do_POP();
            auto the_type = readData(iter, &PyObject::ob_type);
            auto next = do_CallSlot(the_type, &PyTypeObject::tp_iternext, iter);
            auto cmp = builder.CreateICmpEQ(next, ConstantPointerNull::get(types.get<PyObject *>()));
            auto b_continue = createBlock(instr.offset, "FOR_ITER.continue");
            auto b_break = createBlock(instr.offset, "FOR_ITER.break");
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
        } else
            if (v > instr_offset) {
                right = mid - 1;
            } else {
                return blocks[mid + 1].llvm_block;
            }
    }
    assert(false);
    return nullptr;
}

llvm::Value *Translator::getSymbol(size_t i) {
    auto &symbol = py_symbols[i];
    if (!symbol) {
        auto block = builder.GetInsertBlock();
        builder.SetInsertPoint(blocks[0].llvm_block);
        const char *name = nullptr;
        if constexpr (debug_build) {
            name = symbol_names[i];
        }
        symbol = getPointer<FunctionPointer>(func->getArg(0), i, useName("$symbol.", name));
        builder.SetInsertPoint(block);
    }
    return builder.CreateLoad(types.get<FunctionPointer>(), symbol);
}
