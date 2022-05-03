#include <memory>
#include <stdexcept>

#include <Python.h>

#include "translator.h"


using namespace std;
using namespace llvm;

static auto createTBAANode(MDBuilder &builder, MDNode *root, const char *name, bool is_const = false) {
    // TODO: 为啥name为空就会被合并
    // if constexpr(!debug_build) {
    //     name = "";
    // }
    auto scalar_node = builder.createTBAANode(name, root);
    return builder.createTBAAStructTagNode(scalar_node, scalar_node, 0, is_const);
}

Translator::Translator(const DataLayout &dl) : data_layout{dl} {
    mod.setDataLayout(data_layout);

    auto md_builder = MDBuilder(context);
    likely_true = md_builder.createBranchWeights(INT32_MAX, 0);
    auto tbaa_root = md_builder.createTBAARoot("TBAA root for CPython");
    tbaa_refcnt = createTBAANode(md_builder, tbaa_root, "refcnt");
    tbaa_frame_slot = createTBAANode(md_builder, tbaa_root, "frame slot");
    tbaa_frame_cells = createTBAANode(md_builder, tbaa_root, "frame cells");
    tbaa_code_const = createTBAANode(md_builder, tbaa_root, "code const", true);
    tbaa_frame_status = createTBAANode(md_builder, tbaa_root, "stack pointer");
    tbaa_symbols = createTBAANode(md_builder, tbaa_root, "symbols", true);

    auto attr_builder = AttrBuilder(context);
    attr_builder.addAttribute(Attribute::NoReturn);
    attr_noreturn = AttributeList::get(context, AttributeList::FunctionIndex, attr_builder);
    attr_builder.clear();
    attr_builder
            .addAttribute(Attribute::WillReturn)
            .addAttribute(Attribute::ArgMemOnly);
    attr_return = AttributeList::get(context, AttributeList::FunctionIndex, attr_builder);
    attr_builder.clear();
    attr_builder
            .addAttribute(Attribute::NoUnwind)
            .addAttribute(Attribute::InaccessibleMemOrArgMemOnly);
    attr_inaccessible_or_arg = AttributeList::get(context, AttributeList::FunctionIndex, attr_builder);

    attr_builder.clear();
    attr_builder
            .addAttribute(Attribute::NoUnwind)
            .addAttribute("tune-cpu", sys::getHostCPUName());
    func->setAttributes(AttributeList::get(context, AttributeList::FunctionIndex, attr_builder));
    func->getArg(0)->setName(useName("$symbols"));
    frame_obj->setName(useName("$frame"));
    // TODO: 下面的，不知是否有必要
    func->getArg(0)->addAttr(Attribute::NoAlias);
    func->getArg(1)->addAttr(Attribute::NoAlias);
}

void *Translator::operator()(Compiler &compiler, PyCodeObject *code) {
    py_code = code;
    // TODO: 重复了
    parseCFG();

    blocks[0].block = createBlock("$entry_block", func);
    auto start = blocks[0].end;
    for (auto &b : Range(block_num - 1, &blocks[1])) {
        b.initial_stack_height = -1;
        b.block = createBlock(useName("$instr.", start), nullptr);
        start = b.end;
    }
    blocks[1].initial_stack_height = 0;

    builder.SetInsertPoint(blocks[0].block);

    auto code_obj = readData(frame_obj, &PyFrameObject::f_code, useName("$code"));

    auto names_tuple = loadFieldValue(code_obj, &PyCodeObject::co_names, tbaa_code_const);
    auto names_array = getPointer(names_tuple, &PyTupleObject::ob_item, "$names");
    py_names.reserve(PyTuple_GET_SIZE(code->co_names));
    for (auto i : Range(PyTuple_GET_SIZE(code->co_names))) {
        PyObject *repr{};
        if constexpr(debug_build) {
            repr = PyTuple_GET_ITEM(code->co_names, i);
        }
        py_names[i] = loadElementValue<PyObject *>(names_array, i, tbaa_code_const, useName(repr));
    }

    auto consts_tuple = loadFieldValue(code_obj, &PyCodeObject::co_consts, tbaa_code_const);
    auto consts_array = getPointer(consts_tuple, &PyTupleObject::ob_item, "$consts");
    py_consts.reserve(PyTuple_GET_SIZE(code->co_consts));
    for (auto i : Range(PyTuple_GET_SIZE(code->co_consts))) {
        PyObject *repr{};
        if constexpr(debug_build) {
            repr = PyObject_Repr(PyTuple_GET_ITEM(code->co_consts, i));
        }
        py_consts[i] = loadElementValue<PyObject *>(consts_array, i, tbaa_code_const, useName(repr));
    }

    auto localsplus = getPointer(frame_obj, &PyFrameObject::f_localsplus, useName("$localsplus"));
    auto nlocals = code->co_nlocals;
    auto start_cells = nlocals;
    auto ncells = PyTuple_GET_SIZE(code->co_cellvars);
    auto start_frees = start_cells + ncells;
    auto nfrees = PyTuple_GET_SIZE(code->co_freevars);
    auto start_stack = start_frees + nfrees;
    auto nstack = code->co_stacksize;
    py_locals.reserve(nlocals);
    for (auto i : Range(nlocals)) {
        auto name = PyTuple_GET_ITEM(code->co_varnames, i);
        py_locals[i] = getPointer<PyObject *>(localsplus, i, useName("$local.", name));
    }
    py_freevars.reserve(ncells + nfrees);
    for (auto i : Range(ncells)) {
        auto name = PyTuple_GET_ITEM(code->co_cellvars, i);
        // TODO: 直接read出来，反正tbaa是const
        py_freevars[i] = getPointer<PyObject *>(localsplus, start_cells + i, useName("$cell.", name));
    }
    for (auto i : Range(nfrees)) {
        auto name = PyTuple_GET_ITEM(code->co_freevars, i);
        py_freevars[ncells + i] = getPointer<PyObject *>(localsplus, start_frees + i, useName("$free.", name));
    }
    py_stack.reserve(nstack);
    for (auto i : Range(nstack)) {
        py_stack[i] = getPointer<PyObject *>(localsplus, start_stack + i, useName("$stack.", i));
    }
    rt_stack_height_pointer = getPointer(frame_obj, &PyFrameObject::f_stackdepth, "$stack_height");
    rt_lasti = getPointer(frame_obj, &PyFrameObject::f_lasti, "$lasti");

    error_block = createBlock("$error_here", nullptr);

    for (auto &i : Range(block_num - 1, 1U)) {
        emitBlock(i);
    }

    error_block->insertInto(func);
    builder.SetInsertPoint(error_block);
    do_CallSymbol<raiseException, &Translator::attr_noreturn>();
    builder.CreateUnreachable();

    builder.SetInsertPoint(blocks[0].block);
    builder.CreateIndirectBr(
            builder.CreateInBoundsGEP(types.get<char>(), BlockAddress::get(blocks[1].block), func->getArg(2)));

    auto result = compiler(mod);

    // TODO: 可能需要考虑mod.dropAllReferences();不复用函数了，改成复用module，甚至都不复用
    func->dropAllReferences();

    return result;
}

void Translator::parseCFG() {
    py_instructions = reinterpret_cast<PyInstr *>(PyBytes_AS_STRING(py_code->co_code));
    auto size = PyBytes_GET_SIZE(py_code->co_code) / sizeof(PyInstr);

    BitArray is_boundary(size + 1);
    DynamicArray<int> instr_stack_height(size);
    block_num = 0;

    for (QuickPyInstrIter instr(py_instructions, 0, size); instr.next();) {
        switch (instr.opcode) {
        case JUMP_FORWARD:
        case FOR_ITER:
        case SETUP_FINALLY:
        case SETUP_WITH:
        case SETUP_ASYNC_WITH:
            block_num += is_boundary.set(instr.offset + instr.getOparg());
            break;
        case JUMP_ABSOLUTE:
        case JUMP_IF_TRUE_OR_POP:
        case JUMP_IF_FALSE_OR_POP:
        case POP_JUMP_IF_TRUE:
        case POP_JUMP_IF_FALSE:
        case JUMP_IF_NOT_EXC_MATCH:
            block_num += is_boundary.set(instr.getOparg());
            break;
        default:
            break;
        }
    }

    block_num += is_boundary.set(0);
    block_num += is_boundary.set(size);

    blocks.reserve(block_num);
    unsigned current_num = 0;
    for (auto i : Range(BitArray::chunkNumber(size))) {
        unsigned j = 0;
        for (auto bits = is_boundary[i]; bits; bits >>= 1) {
            if (bits & 1) {
                blocks[current_num++].end = i * BitArray::BitsPerValue + j;
            }
            j++;
        }
    }
    assert(current_num == block_num);
}


void Translator::do_Py_INCREF(Value *py_obj) {
    do_CallSymbol<handle_INCREF, &Translator::attr_return>(py_obj);
    // auto ref = getPointer(py_obj, &PyObject::ob_refcnt);
    // auto old_value = loadValue<decltype(PyObject::ob_refcnt)>(ref, tbaa_refcnt);
    // auto *delta_1 = asValue(decltype(PyObject::ob_refcnt){1});
    // auto new_value = builder.CreateAdd(old_value, delta_1);
    // storeValue<decltype(PyObject::ob_refcnt)>(new_value, ref, tbaa_refcnt);
}

void Translator::do_Py_DECREF(Value *py_obj) {
    do_CallSymbol<handle_DECREF, &Translator::attr_return>(py_obj);
    // auto ref = getPointer(py_obj, &PyObject::ob_refcnt);
    // auto old_value = loadValue<decltype(PyObject::ob_refcnt)>(ref, tbaa_refcnt);
    // auto *delta_1 = asValue(decltype(PyObject::ob_refcnt){1});
    // auto *zero = asValue(decltype(PyObject::ob_refcnt){0});
    // auto new_value = builder.CreateSub(old_value, delta_1);
    // storeValue<decltype(PyObject::ob_refcnt)>(new_value, ref, tbaa_refcnt);
    // auto b_dealloc = createBlock("dealloc");
    // auto b_end = createBlock("dealloc.end");
    // builder.CreateCondBr(builder.CreateICmpEQ(new_value, zero), b_dealloc, b_end);
    // builder.SetInsertPoint(b_dealloc);
    // do_CallSymbol<deallocObject>(py_obj);
    // builder.CreateBr(b_end);
    // builder.SetInsertPoint(b_end);
}

void Translator::do_Py_XDECREF(Value *py_obj) {
    do_CallSymbol<handle_XDECREF, &Translator::attr_return>(py_obj);
    // auto b_decref = createBlock("decref");
    // auto b_end = createBlock("decref.end");
    // builder.CreateCondBr(builder.CreateICmpNE(py_obj, c_null), b_decref, b_end);
    // builder.SetInsertPoint(b_decref);
    // do_Py_DECREF(py_obj);
    // builder.CreateBr(b_end);
    // builder.SetInsertPoint(b_end);
}

Value *Translator::do_GETLOCAL(PyOparg oparg) {
    auto varname = PyTuple_GET_ITEM(py_code->co_varnames, oparg);
    return loadValue<PyObject *>(py_locals[oparg], tbaa_frame_slot, useName(varname));
}

void Translator::do_SETLOCAL(PyOparg oparg, llvm::Value *value) {
    auto old_value = do_GETLOCAL(oparg);
    storeValue<PyObject *>(value, py_locals[oparg], tbaa_frame_slot);
    do_Py_XDECREF(old_value);
}

llvm::Value *Translator::do_POP() {
    assert(stack_height > 0);
    return loadValue<PyObject *>(py_stack[--stack_height], tbaa_frame_slot);
}

void Translator::do_PUSH(llvm::Value *value) {
    storeValue<PyObject *>(value, py_stack[stack_height++], tbaa_frame_slot);
}

[[noreturn]] inline void unimplemented() {
    throw runtime_error("unimplemented opcode");
}

void Translator::emitBlock(unsigned index) {
    blocks[index].block->insertInto(func);
    builder.SetInsertPoint(blocks[index].block);
    bool fall_through = true;
    // TODO: 这种可能会有问题，会出现一个block没有被设置initial_stack_height但是被遍历，会assert失败
    stack_height = blocks[index].initial_stack_height;
    assert(stack_height >= 0);

    assert(index);
    auto first = blocks[index - 1].end;
    auto last = blocks[index].end;
    for (PyInstrIter instr(py_instructions, first, last); instr;) {
        instr.next();
        lasti = instr.offset - 1;
        // TODO：不如合并到cframe中去，一次性写入
        storeValue<decltype(lasti)>(asValue(lasti), rt_lasti, tbaa_frame_status);
        // 注意stack_height记录于此，这就意味着在调用”风险函数“之前不允许DECREF，否则可能DEC两次
        storeValue<decltype(stack_height)>(asValue(stack_height), rt_stack_height_pointer, tbaa_frame_status);

        switch (instr.opcode) {
        case EXTENDED_ARG: {
            instr.extend_current_oparg();
            break;
        }
        case NOP: {
            break;
        }
        case ROT_TWO: {
            auto top = do_PEAK(1);
            auto second = do_PEAK(2);;
            do_SET_PEAK(1, second);
            do_SET_PEAK(2, top);
            break;
        }
        case ROT_THREE: {
            auto top = do_PEAK(1);
            auto second = do_PEAK(2);
            auto third = do_PEAK(3);
            do_SET_PEAK(1, second);
            do_SET_PEAK(2, third);
            do_SET_PEAK(3, top);
            break;
        }
        case ROT_FOUR: {
            auto top = do_PEAK(1);
            auto second = do_PEAK(2);
            auto third = do_PEAK(3);
            auto fourth = do_PEAK(4);
            do_SET_PEAK(1, second);
            do_SET_PEAK(2, third);
            do_SET_PEAK(3, fourth);
            do_SET_PEAK(4, top);
            break;
        }
        case ROT_N: {
            auto dest_start = py_stack[stack_height - 1];
            auto src_start = py_stack[stack_height - 2];
            auto top = loadValue<PyObject *>(dest_start, tbaa_frame_cells);
            auto last_cell = py_stack[stack_height - instr.oparg];
            auto entry_block = builder.GetInsertBlock();
            auto loop_block = createBlock("ROT_N.loop");
            auto end_block = createBlock("ROT_N.end");
            builder.CreateBr(loop_block);
            builder.SetInsertPoint(loop_block);
            auto dest = builder.CreatePHI(types.get<PyObject *>(), 2);
            auto src = builder.CreatePHI(types.get<PyObject *>(), 2);
            auto value = loadValue<PyObject *>(src, tbaa_frame_cells);
            storeValue<PyObject *>(value, dest, tbaa_frame_cells);
            auto src_update = getPointer<PyObject *>(src, -1);
            auto should_break = builder.CreateICmpEQ(dest, last_cell);
            builder.CreateCondBr(should_break, end_block, loop_block);
            dest->addIncoming(dest_start, entry_block);
            dest->addIncoming(src, loop_block);
            src->addIncoming(src_start, entry_block);
            src->addIncoming(src_update, loop_block);
            builder.SetInsertPoint(entry_block);
            storeValue<PyObject *>(top, last_cell, tbaa_frame_cells);
        }
        case DUP_TOP: {
            auto top = do_PEAK(1);
            do_Py_INCREF(top);
            do_PUSH(top);
            break;
        }
        case DUP_TOP_TWO: {
            auto top = do_PEAK(1);
            auto second = do_PEAK(2);
            do_Py_INCREF(second);
            do_PUSH(second);
            do_Py_INCREF(top);
            do_PUSH(top);
            break;
        }
        case POP_TOP: {
            auto value = do_POP();
            do_Py_DECREF(value);
            break;
        }
        case LOAD_CONST: {
            // TODO: 添加non null属性
            auto value = py_consts[instr.oparg];
            do_Py_INCREF(value);
            do_PUSH(value);
            break;
        }
        case LOAD_FAST: {
            auto value = do_GETLOCAL(instr.oparg);
            auto ok_block = createBlock("LOAD_FAST.OK");
            builder.CreateCondBr(builder.CreateICmpNE(value, c_null), ok_block, error_block, likely_true);
            builder.SetInsertPoint(ok_block);
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
            auto ok_block = createBlock("DELETE_FAST.OK");
            builder.CreateCondBr(builder.CreateICmpNE(value, c_null), ok_block, error_block, likely_true);
            builder.SetInsertPoint(ok_block);
            storeValue<PyObject *>(c_null, py_locals[instr.oparg], tbaa_frame_slot);
            do_Py_DECREF(value);
            break;
        }
        case LOAD_DEREF: {
            auto cell = loadValue<PyObject *>(py_freevars[instr.oparg], tbaa_code_const);
            auto value = loadFieldValue(cell, &PyCellObject::ob_ref, tbaa_frame_cells);
            auto ok_block = createBlock("LOAD_DEREF.OK");
            builder.CreateCondBr(builder.CreateICmpNE(value, c_null), ok_block, error_block, likely_true);
            builder.SetInsertPoint(ok_block);
            do_Py_INCREF(value);
            do_PUSH(value);
            break;
        }
        case LOAD_CLASSDEREF: {
            auto value = do_CallSymbol<handle_LOAD_CLASSDEREF>(frame_obj, asValue(instr.oparg));
            do_PUSH(value);
            break;
        }
        case STORE_DEREF: {
            auto cell = loadValue<PyObject *>(py_freevars[instr.oparg], tbaa_code_const);
            auto cell_slot = getPointer(cell, &PyCellObject::ob_ref);
            auto old_value = loadValue<PyObject *>(cell_slot, tbaa_frame_cells);
            auto value = do_POP();
            storeValue<PyObject *>(value, cell_slot, tbaa_frame_cells);
            do_Py_XDECREF(old_value);
            break;
        }
        case DELETE_DEREF: {
            auto cell = loadValue<PyObject *>(py_freevars[instr.oparg], tbaa_code_const);
            auto cell_slot = getPointer(cell, &PyCellObject::ob_ref);
            auto old_value = loadValue<PyObject *>(cell_slot, tbaa_frame_cells);
            auto ok_block = createBlock("DELETE_DEREF.OK");
            builder.CreateCondBr(builder.CreateICmpNE(old_value, c_null), ok_block, error_block, likely_true);
            builder.SetInsertPoint(ok_block);
            storeValue<PyObject *>(c_null, cell_slot, tbaa_frame_cells);
            do_Py_DECREF(old_value);
            break;
        }
        case LOAD_GLOBAL: {
            auto value = do_CallSymbol<handle_LOAD_GLOBAL>(frame_obj, py_names[instr.oparg]);
            do_PUSH(value);
            break;
        }
        case STORE_GLOBAL: {
            auto value = do_POP();
            do_CallSymbol<handle_STORE_GLOBAL>(frame_obj, py_names[instr.oparg], value);
            do_Py_DECREF(value);
            break;
        }
        case DELETE_GLOBAL: {
            do_CallSymbol<handle_DELETE_GLOBAL>(frame_obj, py_names[instr.oparg]);
            break;
        }
        case LOAD_NAME: {
            auto value = do_CallSymbol<handle_LOAD_NAME>(frame_obj, py_names[instr.oparg]);
            do_PUSH(value);
            break;
        }
        case STORE_NAME: {
            auto value = do_POP();
            do_CallSymbol<handle_STORE_NAME>(frame_obj, py_names[instr.oparg], value);
            do_Py_DECREF(value);
            break;
        }
        case DELETE_NAME: {
            do_CallSymbol<handle_DELETE_NAME>(frame_obj, py_names[instr.oparg]);
            break;
        }
        case LOAD_ATTR: {
            auto owner = do_POP();
            auto attr = do_CallSymbol<handle_LOAD_ATTR>(owner, py_names[instr.oparg]);
            do_PUSH(attr);
            do_Py_DECREF(owner);
            break;
        }
        case LOAD_METHOD: {
            auto obj = do_POP();
            do_CallSymbol<handle_LOAD_METHOD>(obj, py_names[instr.oparg], py_stack[stack_height]);
            stack_height += 2;
            break;
        }
        case STORE_ATTR: {
            auto owner = do_POP();
            auto value = do_POP();
            do_CallSymbol<handle_STORE_ATTR>(owner, py_names[instr.oparg], value);
            do_Py_DECREF(value);
            do_Py_DECREF(owner);
            break;
        }
        case DELETE_ATTR: {
            auto owner = do_POP();
            do_CallSymbol<handle_STORE_ATTR>(owner, py_names[instr.oparg], c_null);
            do_Py_DECREF(owner);
            break;
        }
        case BINARY_SUBSCR: {
            handle_BINARY_OP(searchSymbol<handle_BINARY_SUBSCR>());
            break;
        }
        case STORE_SUBSCR: {
            auto sub = do_POP();
            auto container = do_POP();
            auto value = do_POP();
            do_CallSymbol<handle_STORE_SUBSCR>(container, sub, value);
            do_Py_DECREF(value);
            do_Py_DECREF(container);
            do_Py_DECREF(sub);
            break;
        }
        case DELETE_SUBSCR: {
            auto sub = do_POP();
            auto container = do_POP();
            do_CallSymbol<handle_STORE_SUBSCR>(container, sub, c_null);
            do_Py_DECREF(container);
            do_Py_DECREF(sub);
            break;
        }
        case UNARY_NOT: {
            handle_UNARY_OP(searchSymbol<handle_UNARY_NOT>());
            break;
        }
        case UNARY_POSITIVE: {
            handle_UNARY_OP(searchSymbol<handle_UNARY_POSITIVE>());
            break;
        }
        case UNARY_NEGATIVE: {
            handle_UNARY_OP(searchSymbol<handle_UNARY_NEGATIVE>());
            break;
        }
        case UNARY_INVERT: {
            handle_UNARY_OP(searchSymbol<handle_UNARY_INVERT>());
            break;
        }
        case BINARY_ADD: {
            handle_BINARY_OP(searchSymbol<handle_BINARY_ADD>());
            break;
        }
        case INPLACE_ADD: {
            handle_BINARY_OP(searchSymbol<handle_INPLACE_ADD>());
            break;
        }
        case BINARY_SUBTRACT: {
            handle_BINARY_OP(searchSymbol<handle_BINARY_SUBTRACT>());
            break;
        }
        case INPLACE_SUBTRACT: {
            handle_BINARY_OP(searchSymbol<handle_INPLACE_SUBTRACT>());
            break;
        }
        case BINARY_MULTIPLY: {
            handle_BINARY_OP(searchSymbol<handle_BINARY_MULTIPLY>());
            break;
        }
        case INPLACE_MULTIPLY: {
            handle_BINARY_OP(searchSymbol<handle_INPLACE_MULTIPLY>());
            break;
        }
        case BINARY_FLOOR_DIVIDE: {
            handle_BINARY_OP(searchSymbol<handle_BINARY_FLOOR_DIVIDE>());
            break;
        }
        case INPLACE_FLOOR_DIVIDE: {
            handle_BINARY_OP(searchSymbol<handle_INPLACE_FLOOR_DIVIDE>());
            break;
        }
        case BINARY_TRUE_DIVIDE: {
            handle_BINARY_OP(searchSymbol<handle_BINARY_TRUE_DIVIDE>());
            break;
        }
        case INPLACE_TRUE_DIVIDE: {
            handle_BINARY_OP(searchSymbol<handle_INPLACE_TRUE_DIVIDE>());
            break;
        }
        case BINARY_MODULO: {
            handle_BINARY_OP(searchSymbol<handle_BINARY_MODULO>());
            break;
        }
        case INPLACE_MODULO: {
            handle_BINARY_OP(searchSymbol<handle_INPLACE_MODULO>());
            break;
        }
        case BINARY_POWER: {
            handle_BINARY_OP(searchSymbol<handle_BINARY_POWER>());
            break;
        }
        case INPLACE_POWER: {
            handle_BINARY_OP(searchSymbol<handle_INPLACE_POWER>());
            break;
        }
        case BINARY_MATRIX_MULTIPLY: {
            handle_BINARY_OP(searchSymbol<handle_BINARY_MATRIX_MULTIPLY>());
            break;
        }
        case INPLACE_MATRIX_MULTIPLY: {
            handle_BINARY_OP(searchSymbol<handle_INPLACE_MATRIX_MULTIPLY>());
            break;
        }
        case BINARY_LSHIFT: {
            handle_BINARY_OP(searchSymbol<handle_BINARY_LSHIFT>());
            break;
        }
        case INPLACE_LSHIFT: {
            handle_BINARY_OP(searchSymbol<handle_INPLACE_LSHIFT>());
            break;
        }
        case BINARY_RSHIFT: {
            handle_BINARY_OP(searchSymbol<handle_BINARY_RSHIFT>());
            break;
        }
        case INPLACE_RSHIFT: {
            handle_BINARY_OP(searchSymbol<handle_INPLACE_RSHIFT>());
            break;
        }
        case BINARY_AND: {
            handle_BINARY_OP(searchSymbol<handle_BINARY_AND>());
            break;
        }
        case INPLACE_AND: {
            handle_BINARY_OP(searchSymbol<handle_INPLACE_AND>());
            break;
        }
        case BINARY_OR: {
            handle_BINARY_OP(searchSymbol<handle_BINARY_OR>());
            break;
        }
        case INPLACE_OR: {
            handle_BINARY_OP(searchSymbol<handle_INPLACE_OR>());
            break;
        }
        case BINARY_XOR: {
            handle_BINARY_OP(searchSymbol<handle_BINARY_XOR>());
            break;
        }
        case INPLACE_XOR: {
            handle_BINARY_OP(searchSymbol<handle_INPLACE_XOR>());
            break;
        }
        case COMPARE_OP: {
            auto right = do_POP();
            auto left = do_POP();
            // TODO: asValue应该是形参类型
            auto res = do_CallSymbol<handle_COMPARE_OP>(left, right, asValue<int>(instr.oparg));
            do_PUSH(res);
            do_Py_DECREF(left);
            do_Py_DECREF(right);
            break;
        }
        case IS_OP: {
            auto right = do_POP();
            auto left = do_POP();
            auto py_true = getSymbol(searchSymbol<&_Py_TrueStruct>());
            auto py_false = getSymbol(searchSymbol<&_Py_FalseStruct>());
            auto value_for_true = !instr.oparg ? py_true : py_false;
            auto value_for_false = !instr.oparg ? py_false : py_true;
            builder.CreateSelect(builder.CreateICmpEQ(left, right), value_for_true, value_for_false);
            break;
        }
        case CONTAINS_OP: {
            auto right = do_POP();
            auto left = do_POP();
            auto res = do_CallSymbol<handle_CONTAINS_OP>(left, right, asValue<bool>(instr.oparg));
            auto py_true = getSymbol(searchSymbol<&_Py_TrueStruct>());
            auto py_false = getSymbol(searchSymbol<&_Py_FalseStruct>());
            auto value_for_true = !instr.oparg ? py_true : py_false;
            auto value_for_false = !instr.oparg ? py_false : py_true;
            auto value = builder.CreateSelect(res, value_for_true, value_for_false);
            do_PUSH(value);
            do_Py_DECREF(left);
            do_Py_DECREF(right);
            break;
        }
        case RETURN_VALUE: {
            auto retval = do_POP();
            // 或者INCREF，防止DECREF两次
            storeValue<decltype(stack_height)>(asValue(stack_height), rt_stack_height_pointer, tbaa_frame_status);
            builder.CreateRet(retval);
            fall_through = false;
            break;
        }
        case CALL_FUNCTION: {
            // func_args重命名
            auto func_args = py_stack[stack_height -= instr.oparg + 1];
            auto ret = do_CallSymbol<handle_CALL_FUNCTION>(func_args, asValue<Py_ssize_t>(instr.oparg));
            do_PUSH(ret);
            break;
        }
        case CALL_FUNCTION_KW: {
            auto func_args = py_stack[stack_height -= instr.oparg + 2];
            auto ret = do_CallSymbol<handle_CALL_FUNCTION_KW>(func_args, asValue<Py_ssize_t>(instr.oparg));
            do_PUSH(ret);
            break;
        }
        case CALL_FUNCTION_EX: {
            llvm::Value *kwargs = c_null;
            if (instr.oparg) {
                kwargs = do_POP();
            }
            auto args = do_POP();
            auto callable = do_POP();
            auto ret = do_CallSymbol<handle_CALL_FUNCTION_EX>(callable, args, kwargs);
            do_PUSH(ret);
            if (instr.oparg) {
                do_Py_DECREF(kwargs);
            }
            do_Py_DECREF(args);
            do_Py_DECREF(callable);
            break;
        }
        case CALL_METHOD: {
            auto maybe_meth = do_PEAK(instr.oparg + 2);
            stack_height -= instr.oparg + 2;
            auto is_meth = builder.CreateICmpNE(maybe_meth, c_null);
            auto func_args = builder.CreateSelect(is_meth, py_stack[stack_height], py_stack[stack_height + 1]);
            auto nargs = builder.CreateSelect(is_meth,
                    asValue<Py_ssize_t>(instr.oparg + 1), asValue<Py_ssize_t>(instr.oparg));
            auto ret = do_CallSymbol<handle_CALL_FUNCTION>(func_args, nargs);
            do_PUSH(ret);
            break;
        }
        case LOAD_CLOSURE: {
            auto cell = loadValue<PyObject *>(py_freevars[instr.oparg], tbaa_code_const);
            do_Py_INCREF(cell);
            do_PUSH(cell);
            break;
        }
        case MAKE_FUNCTION: {
            auto qualname = do_POP();
            auto codeobj = do_POP();
            auto globals = loadFieldValue(frame_obj, &PyFrameObject::f_globals, tbaa_code_const);
            auto py_func = do_CallSymbol<PyFunction_NewWithQualName>(codeobj, globals, qualname);
            auto ok_block = createBlock("MAKE_FUNCTION.OK");
            builder.CreateCondBr(builder.CreateICmpNE(py_func, c_null), ok_block, error_block, likely_true);
            builder.SetInsertPoint(ok_block);
            // TODO: 一个新的tbaa
            if (instr.oparg & 0x08) {
                storeFiledValue(do_POP(), py_func, &PyFunctionObject::func_closure, tbaa_refcnt);
            }
            if (instr.oparg & 0x04) {
                storeFiledValue(do_POP(), py_func, &PyFunctionObject::func_annotations, tbaa_refcnt);
            }
            if (instr.oparg & 0x02) {
                storeFiledValue(do_POP(), py_func, &PyFunctionObject::func_kwdefaults, tbaa_refcnt);
            }
            if (instr.oparg & 0x01) {
                storeFiledValue(do_POP(), py_func, &PyFunctionObject::func_defaults, tbaa_refcnt);
            }
            do_PUSH(py_func);
            do_Py_DECREF(codeobj);
            do_Py_DECREF(qualname);
            break;
        }
        case LOAD_BUILD_CLASS: {
            auto builtins = loadFieldValue(frame_obj, &PyFrameObject::f_builtins, tbaa_code_const);
            auto bc = do_CallSymbol<handle_LOAD_BUILD_CLASS>(builtins);
            do_PUSH(bc);
            break;
        }

        case IMPORT_NAME: {
            auto name = py_names[instr.oparg];
            auto fromlist = do_POP();
            auto level = do_POP();
            auto res = do_CallSymbol<handle_IMPORT_NAME>(frame_obj, name, fromlist, level);
            do_PUSH(res);
            Py_DECREF(level);
            Py_DECREF(fromlist);
            break;
        }
        case IMPORT_FROM: {
            auto from = do_PEAK(1);
            auto res = do_CallSymbol<handle_IMPORT_FROM>(from, py_names[instr.oparg]);
            do_PUSH(res);
            break;
        }
        case IMPORT_STAR: {
            auto from = do_POP();
            do_CallSymbol<handle_IMPORT_STAR>(frame_obj, from);
            do_Py_DECREF(from);
            break;
        }

        case JUMP_FORWARD: {
            auto &jmp_block = findPyBlock(instr.offset + instr.oparg);
            jmp_block.initial_stack_height = stack_height;
            builder.CreateBr(jmp_block.block);
            fall_through = false;
            break;
        }
        case JUMP_ABSOLUTE: {
            // TODO: 如果有，则不要设置，要校验
            auto &jmp_block = findPyBlock(instr.oparg);
            jmp_block.initial_stack_height = stack_height;
            builder.CreateBr(jmp_block.block);
            fall_through = false;
            break;
        }
        case POP_JUMP_IF_TRUE: {
            pyJumpIF(instr.oparg, true, true);
            break;
        }
        case POP_JUMP_IF_FALSE: {
            pyJumpIF(instr.oparg, true, false);
            break;
        }
        case JUMP_IF_TRUE_OR_POP: {
            pyJumpIF(instr.oparg, false, true);
            break;
        }
        case JUMP_IF_FALSE_OR_POP: {
            pyJumpIF(instr.oparg, false, false);
            break;
        }
        case GET_ITER: {
            auto iterable = do_POP();
            auto iter = do_CallSymbol<handle_GET_ITER>(iterable);
            do_Py_DECREF(iterable);
            do_PUSH(iter);
            break;
        }
        case FOR_ITER: {
            auto sp = stack_height;
            auto iter = do_PEAK(1);
            auto the_type = readData(iter, &PyObject::ob_type);
            auto next = do_CallSlot(the_type, &PyTypeObject::tp_iternext, iter);
            auto b_continue = createBlock("FOR_ITER.continue");
            auto b_break = createBlock("FOR_ITER.break");
            builder.CreateCondBr(builder.CreateICmpEQ(next, c_null), b_break, b_continue);
            builder.SetInsertPoint(b_break);
            do_Py_DECREF(iter);
            auto &break_block = findPyBlock(instr.offset + instr.oparg);
            break_block.initial_stack_height = stack_height - 1;
            builder.CreateBr(break_block.block);
            builder.SetInsertPoint(b_continue);
            stack_height = sp;
            do_PUSH(next);
            break;
        }

        case BUILD_TUPLE: {
            auto values = py_stack[stack_height -= instr.oparg];
            auto map = do_CallSymbol<handle_BUILD_TUPLE>(values, asValue<Py_ssize_t>(instr.oparg));
            do_PUSH(map);
            break;
        }
        case BUILD_LIST: {
            auto values = py_stack[stack_height -= instr.oparg];
            auto map = do_CallSymbol<handle_BUILD_LIST>(values, asValue<Py_ssize_t>(instr.oparg));
            do_PUSH(map);
            break;
        }
        case BUILD_SET: {
            auto values = py_stack[stack_height -= instr.oparg];
            auto map = do_CallSymbol<handle_BUILD_SET>(values, asValue<Py_ssize_t>(instr.oparg));
            do_PUSH(map);
            break;
        }
        case BUILD_MAP: {
            auto values = py_stack[stack_height -= 2 * instr.oparg];
            auto map = do_CallSymbol<handle_BUILD_MAP>(values, asValue<Py_ssize_t>(instr.oparg));
            do_PUSH(map);
            break;
        }
        case BUILD_CONST_KEY_MAP: {
            auto values = py_stack[stack_height -= instr.oparg + 1];
            auto map = do_CallSymbol<handle_BUILD_CONST_KEY_MAP>(values, asValue<Py_ssize_t>(instr.oparg));
            do_PUSH(map);
            break;
        }
        case LIST_APPEND: {
            auto value = do_POP();
            auto list = do_PEAK(instr.oparg);
            do_CallSymbol<handle_LIST_APPEND>(list, value);
            do_Py_DECREF(value);
            break;
        }
        case SET_ADD: {
            auto value = do_POP();
            auto set = do_PEAK(instr.oparg);
            do_CallSymbol<handle_SET_ADD>(set, value);
            do_Py_DECREF(value);
            break;
        }
        case MAP_ADD: {
            auto value = do_POP();
            auto key = do_POP();
            auto map = do_PEAK(instr.oparg);
            do_CallSymbol<handle_MAP_ADD>(map, key, value);
            do_Py_DECREF(key);
            do_Py_DECREF(value);
            break;
        }
        case LIST_EXTEND: {
            auto iterable = do_POP();
            auto list = do_PEAK(instr.oparg);
            do_CallSymbol<handle_LIST_EXTEND>(list, iterable);
            do_Py_DECREF(iterable);
            break;
        }
        case SET_UPDATE: {
            auto iterable = do_POP();
            auto set = do_PEAK(instr.oparg);
            do_CallSymbol<handle_SET_UPDATE>(set, iterable);
            do_Py_DECREF(iterable);
            break;
        }
        case DICT_UPDATE: {
            auto update = do_POP();
            auto dict = do_PEAK(instr.oparg);
            do_CallSymbol<handle_DICT_UPDATE>(dict, update);
            do_Py_DECREF(update);
            break;
        }
        case DICT_MERGE: {
            auto update = do_POP();
            auto dict = do_PEAK(instr.oparg);
            auto callee = do_PEAK(instr.oparg + 2);
            do_CallSymbol<handle_DICT_MERGE>(callee, dict, update);
            do_Py_DECREF(update);
            break;
        }
        case LIST_TO_TUPLE: {
            auto list = do_POP();
            auto tuple = do_CallSymbol<handle_LIST_TO_TUPLE>(list);
            do_PUSH(tuple);
            do_Py_DECREF(list);
            break;
        }

        case FORMAT_VALUE: {
            auto fmt_spec = (instr.oparg & FVS_MASK) == FVS_HAVE_SPEC ? do_POP() : c_null;
            auto value = do_POP();
            int which_conversion = instr.oparg & FVC_MASK;
            auto result = do_CallSymbol<handle_FORMAT_VALUE>(value, fmt_spec, asValue(which_conversion));
            do_PUSH(result);
            do_Py_DECREF(value);
        }
        case BUILD_STRING: {
            auto values = py_stack[stack_height -= instr.oparg];
            auto str = do_CallSymbol<handle_BUILD_STRING>(values, asValue<Py_ssize_t>(instr.oparg));
            do_PUSH(str);
            break;
        }

        case UNPACK_SEQUENCE: {
            unimplemented();
            break;
        }
        case UNPACK_EX: {
            unimplemented();
            break;
        }

        case GET_LEN: {
            unimplemented();
            break;
        }
        case MATCH_MAPPING: {
            unimplemented();
            break;
        }
        case MATCH_SEQUENCE: {
            unimplemented();
            break;
        }
        case MATCH_KEYS: {
            unimplemented();
            break;
        }
        case MATCH_CLASS: {
            unimplemented();
            break;
        }
        case COPY_DICT_WITHOUT_KEYS: {
            unimplemented();
            break;
        }

        case BUILD_SLICE: {
            unimplemented();
            break;
        }
        case LOAD_ASSERTION_ERROR: {
            unimplemented();
            break;
        }
        case RAISE_VARARGS: {
            unimplemented();
            break;
        }
        case SETUP_ANNOTATIONS: {
            unimplemented();
            break;
        }
        case PRINT_EXPR: {
            unimplemented();
            break;
        }

        case SETUP_FINALLY: {
            auto &py_finally_block = findPyBlock(instr.offset + instr.oparg);
            py_finally_block.initial_stack_height = stack_height + 6;
            auto block_addr_diff = builder.CreateSub(
                    builder.CreatePtrToInt(BlockAddress::get(func, py_finally_block.block), types.get<uintptr_t>()),
                    builder.CreatePtrToInt(BlockAddress::get(func, blocks[1].block), types.get<uintptr_t>())
            );
            do_CallSymbol<PyFrame_BlockSetup>(frame_obj,
                    asValue<int>(SETUP_FINALLY),
                    builder.CreateIntCast(block_addr_diff, types.get<int>(), true),
                    asValue<int>(stack_height));
            break;
        }
        case POP_BLOCK: {
            do_CallSymbol<PyFrame_BlockPop>(frame_obj);
            break;
        }
        case POP_EXCEPT: {
            do_CallSymbol<handle_POP_EXCEPT>(frame_obj);
            break;
        }
        case JUMP_IF_NOT_EXC_MATCH: {
            auto right = do_POP();
            auto left = do_POP();
            auto match = do_CallSymbol<handle_JUMP_IF_NOT_EXC_MATCH>(left, right);
            do_Py_DECREF(left);
            do_Py_DECREF(right);
            auto &jump_target = findPyBlock(instr.oparg);
            jump_target.initial_stack_height = stack_height;
            auto fall_block = createBlock("");
            builder.CreateCondBr(match, fall_block, jump_target.block);
            builder.SetInsertPoint(fall_block);
            break;
        }
        case RERAISE: {
            do_CallSymbol<handle_RERAISE, &Translator::attr_noreturn>(frame_obj, asValue<bool>(instr.oparg));
            builder.CreateUnreachable();
            fall_through = false;
            break;
        }
        case SETUP_WITH: {
            auto &py_finally_block = findPyBlock(instr.offset + instr.oparg);
            py_finally_block.initial_stack_height = stack_height - 1 + 7;
            auto block_addr_diff = builder.CreateSub(
                    builder.CreatePtrToInt(BlockAddress::get(func, py_finally_block.block), types.get<uintptr_t>()),
                    builder.CreatePtrToInt(BlockAddress::get(func, blocks[1].block), types.get<uintptr_t>())
            );
            do_CallSymbol<handle_SETUP_WITH>(frame_obj, py_stack[stack_height],
                    builder.CreateIntCast(block_addr_diff, types.get<int>(), true));
            stack_height += 1;
            break;
        }
        case WITH_EXCEPT_START: {
            auto exc = do_PEAK(1);
            auto val = do_PEAK(2);
            auto tb = do_PEAK(3);
            auto exit_func = do_PEAK(7);
            auto res = do_CallSymbol<handle_WITH_EXCEPT_START>(exc, val, tb, exit_func);
            do_PUSH(res);
            break;
        }

        case GEN_START: {
            unimplemented();
            break;
        }
        case YIELD_VALUE: {
            unimplemented();
            break;
        }
        case GET_YIELD_FROM_ITER: {
            unimplemented();
            break;
        }
        case YIELD_FROM: {
            unimplemented();
            break;
        }
        case GET_AWAITABLE: {
            unimplemented();
            break;
        }
        case GET_AITER: {
            unimplemented();
            break;
        }
        case GET_ANEXT: {
            unimplemented();
            break;
        }
        case END_ASYNC_FOR: {
            unimplemented();
            break;
        }
        case SETUP_ASYNC_WITH: {
            unimplemented();
            break;
        }
        case BEFORE_ASYNC_WITH: {
            unimplemented();
            break;
        }
        default:
            throw runtime_error("illegal opcode");
        }
    }
    if (fall_through) {
        assert(index < block_num - 1);
        builder.CreateBr(blocks[index + 1].block);
        blocks[index + 1].initial_stack_height = stack_height;
    }
}

PyBasicBlock &Translator::findPyBlock(unsigned offset) {
    assert(block_num > 1);
    decltype(block_num) left = 0;
    auto right = block_num - 1;
    while (left <= right) {
        auto mid = left + (right - left) / 2;
        auto v = blocks[mid].end;
        if (v < offset) {
            left = mid + 1;
        } else if (v > offset) {
            right = mid - 1;
        } else {
            return blocks[mid + 1];
        }
    }
    assert(false);
    return blocks[0];
}

llvm::Value *Translator::getSymbol(size_t offset) {
    auto &symbol = py_symbols[offset];
    if (!symbol) {
        auto block = builder.GetInsertBlock();
        builder.SetInsertPoint(blocks[0].block);
        const char *name = nullptr;
        if constexpr (debug_build) {
            name = symbol_names[offset];
        }
        // TODO: 重命名或者两个tbaa
        // TODO: 合并为READ
        auto ptr = getPointer<void *>(func->getArg(0), offset, useName("$symbol.", offset));
        symbol = loadValue<void *>(ptr, tbaa_code_const, useName("$symbol.", name));
        builder.SetInsertPoint(block);
    }
    return symbol;
}

void Translator::pyJumpIF(unsigned offset, bool pop_if_jump, bool jump_cond) {
    auto &jump_target = findPyBlock(offset);
    jump_target.initial_stack_height = stack_height - pop_if_jump;
    auto false_cmp_block = createBlock("");
    auto slow_cmp_block = createBlock("");
    auto fall_block = createBlock("");
    auto jump_block = pop_if_jump ? createBlock("") : jump_target.block;
    auto true_block = jump_cond ? jump_block : fall_block;
    auto false_block = jump_cond ? fall_block : jump_block;

    auto obj = do_PEAK(1);
    auto py_true = getSymbol(searchSymbol<&_Py_TrueStruct>());
    builder.CreateCondBr(builder.CreateICmpEQ(obj, py_true), true_block, false_cmp_block);
    builder.SetInsertPoint(false_cmp_block);
    auto py_false = getSymbol(searchSymbol<&_Py_FalseStruct>());
    builder.CreateCondBr(builder.CreateICmpEQ(obj, py_false), false_block, slow_cmp_block);
    builder.SetInsertPoint(slow_cmp_block);
    builder.CreateCondBr(do_CallSymbol<castPyObjectToBool>(obj), true_block, false_block);
    if (pop_if_jump) {
        builder.SetInsertPoint(jump_block);
        do_Py_DECREF(obj);
        builder.CreateBr(jump_target.block);
    }
    builder.SetInsertPoint(fall_block);
    do_Py_DECREF(obj);
    stack_height--;
}
