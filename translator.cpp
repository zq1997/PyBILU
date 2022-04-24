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

    auto attr_builder = AttrBuilder(context);
    attr_builder.addAttribute(Attribute::NoReturn);
    attr_noreturn = AttributeList::get(context, AttributeList::FunctionIndex, attr_builder);
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
    simple_frame->setName(useName("$frame"));
    // TODO: 下面的，不知是否有必要
    for (auto &arg : func->args()) {
        arg.addAttr(Attribute::NoAlias);
    }
}

void *Translator::operator()(Compiler &compiler, PyCodeObject *code) {
    py_code = code;
    // TODO: 重复了
    parseCFG();

    blocks[0].llvm_block = createBlock("$entry_block", func);
    auto start = blocks[0].end;
    for (auto &b : Range(block_num - 1, &blocks[1])) {
        b.llvm_block = createBlock(useName("$instr.", start), nullptr);
        start = b.end;
    }

    builder.SetInsertPoint(blocks[0].llvm_block);

    auto code_obj = readData(simple_frame, &PyFrameObject::f_code, useName("$code"));

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

    auto localsplus = getPointer(simple_frame, &PyFrameObject::f_localsplus, useName("$localsplus"));
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
        // TODO: 直接read出来
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
    rt_stack_height_pointer = getPointer(simple_frame, &PyFrameObject::f_stackdepth, "$stack_height");
    rt_lasti = getPointer(simple_frame, &PyFrameObject::f_lasti, "$lasti");

    error_block = createBlock("$error_here", nullptr);
    unwind_block = createBlock("$unwind_block", nullptr);

    for (auto &i : Range(block_num - 1, 1U)) {
        emitBlock(i);
    }

    error_block->insertInto(func);
    builder.SetInsertPoint(error_block);
    do_CallSymbol<raiseException, &Translator::attr_noreturn>();
    builder.CreateUnreachable();

    unwind_block->insertInto(func);
    builder.SetInsertPoint(unwind_block);
    builder.CreateRet(c_null);

    builder.SetInsertPoint(blocks[0].llvm_block);
    builder.CreateBr(blocks[1].llvm_block);

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

    for (QuickPyInstrIter instr(py_instructions, 0, size); instr.next();) {
        switch (instr.opcode) {
        case RETURN_VALUE:
            is_boundary.set(instr.offset);
            break;
        case JUMP_FORWARD:
            is_boundary.set(instr.offset);
            is_boundary.set(instr.offset + instr.getOparg());
            break;
        case JUMP_ABSOLUTE:
        case JUMP_IF_TRUE_OR_POP:
        case JUMP_IF_FALSE_OR_POP:
        case POP_JUMP_IF_TRUE:
        case POP_JUMP_IF_FALSE:
            is_boundary.set(instr.offset);
            is_boundary.set(instr.getOparg());
            break;
        case FOR_ITER:
            is_boundary.set(instr.offset + instr.getOparg());
            break;
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
    auto ref = getPointer(py_obj, &PyObject::ob_refcnt);
    auto old_value = loadValue<decltype(PyObject::ob_refcnt)>(ref, tbaa_refcnt);
    auto *delta_1 = asValue(decltype(PyObject::ob_refcnt){1});
    auto new_value = builder.CreateAdd(old_value, delta_1);
    storeValue<decltype(PyObject::ob_refcnt)>(new_value, ref, tbaa_refcnt);
}

void Translator::do_Py_DECREF(Value *py_obj) {
    auto ref = getPointer(py_obj, &PyObject::ob_refcnt);
    auto old_value = loadValue<decltype(PyObject::ob_refcnt)>(ref, tbaa_refcnt);
    auto *delta_1 = asValue(decltype(PyObject::ob_refcnt){1});
    auto new_value = builder.CreateSub(old_value, delta_1);
    storeValue<decltype(PyObject::ob_refcnt)>(new_value, ref, tbaa_refcnt);
}

void Translator::do_Py_XDECREF(Value *py_obj) {
    auto is_null = builder.CreateICmpEQ(py_obj, c_null);
    auto b_decref = createBlock("decref");
    auto b_end = createBlock("decref.end");
    builder.CreateCondBr(is_null, b_decref, b_end);
    builder.SetInsertPoint(b_decref);
    do_Py_DECREF(py_obj);
    builder.CreateBr(b_end);
    builder.SetInsertPoint(b_end);
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
    return loadValue<PyObject *>(py_stack[--stack_height], tbaa_frame_slot);
}

void Translator::do_PUSH(llvm::Value *value) {
    storeValue<PyObject *>(value, py_stack[stack_height++], tbaa_frame_slot);
}

[[noreturn]] inline void unimplemented() {
    throw runtime_error("unimplemented opcode");
}

void Translator::emitBlock(unsigned index) {
    blocks[index].llvm_block->insertInto(func);
    builder.SetInsertPoint(blocks[index].llvm_block);
    bool fall_through = true;

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
            auto top = do_PEAK(1);
            auto dest = py_stack[stack_height - instr.oparg - 1];
            auto src = py_stack[stack_height - instr.oparg];
            // TODO: 添加属性，willreturn argonly
            do_CallSymbol<memmove>(dest, src, asValue((instr.oparg - 1) * sizeof(PyObject *)));
            do_SET_PEAK(instr.oparg, top);
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
        case STORE_DEREF: {
            auto cell = loadValue<PyObject *>(py_freevars[instr.oparg], tbaa_code_const);
            auto cell_slot = getPointer(cell, &PyCellObject::ob_ref);
            auto old_value = loadValue<PyObject *>(cell_slot, tbaa_frame_cells);
            auto value = do_POP();
            storeValue<PyObject *>(value, cell_slot, tbaa_frame_cells);
            do_Py_XDECREF(old_value);
            break;
        }
        case LOAD_CLASSDEREF: {
            auto value = do_CallSymbol<handle_LOAD_CLASSDEREF>(simple_frame, asValue(instr.oparg));
            do_PUSH(value);
            break;
        }
        case LOAD_GLOBAL: {
            auto value = do_CallSymbol<handle_LOAD_GLOBAL>(simple_frame, py_names[instr.oparg]);
            do_PUSH(value);
            break;
        }
        case STORE_GLOBAL: {
            auto value = do_POP();
            do_CallSymbol<handle_STORE_GLOBAL>(simple_frame, py_names[instr.oparg], value);
            do_Py_DECREF(value);
            break;
        }
        case LOAD_NAME: {
            auto value = do_CallSymbol<handle_LOAD_NAME>(simple_frame, py_names[instr.oparg]);
            do_PUSH(value);
            break;
        }
        case STORE_NAME: {
            auto value = do_POP();
            do_CallSymbol<handle_STORE_NAME>(simple_frame, py_names[instr.oparg], value);
            do_Py_DECREF(value);
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
            auto attr = do_CallSymbol<handle_LOAD_METHOD>(obj, py_names[instr.oparg], py_stack[stack_height]);
            do_PUSH(attr);
            do_Py_DECREF(obj);
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
        // todo: 继续
        case DELETE_DEREF: {
            auto ok = do_CallSymbol<handle_DELETE_DEREF>(simple_frame, asValue(instr.oparg));
            auto is_ok = builder.CreateICmpNE(ok, asValue(0));
            auto bb = createBlock("DELETE_DEREF.OK");
            builder.CreateCondBr(is_ok, bb, unwind_block, likely_true);
            builder.SetInsertPoint(bb);
            break;
        }
        case DELETE_GLOBAL: {
            auto ok = do_CallSymbol<handle_DELETE_GLOBAL>(simple_frame, asValue(instr.oparg));
            auto is_ok = builder.CreateICmpNE(ok, asValue(0));
            auto bb = createBlock("DELETE_GLOBAL.OK");
            builder.CreateCondBr(is_ok, bb, unwind_block, likely_true);
            builder.SetInsertPoint(bb);
            break;
        }
        case DELETE_NAME: {
            auto ok = do_CallSymbol<handle_DELETE_NAME>(simple_frame, asValue(instr.oparg));
            auto is_ok = builder.CreateICmpNE(ok, asValue(0));
            auto bb = createBlock("DELETE_NAME.OK");
            builder.CreateCondBr(is_ok, bb, unwind_block, likely_true);
            builder.SetInsertPoint(bb);
            break;
        }
        case DELETE_ATTR: {
            // auto owner = do_POP();
            // auto err = do_CallSymbol<PyObject_SetAttr>(owner, py_names[instr.oparg], c_null);
            // do_Py_DECREF(owner);
            // auto no_err = builder.CreateICmpSGE(err, asValue(0));
            // auto bb = createBlock("DELETE_ATTR.OK");
            // builder.CreateCondBr(no_err, bb, unwind_block, likely_true);
            // builder.SetInsertPoint(bb);
            // break;
        }
        case DELETE_SUBSCR: {
            // auto sub = do_POP();
            // auto container = do_POP();
            // auto err = do_CallSymbol<PyObject_SetAttr>(container, sub, c_null);
            // do_Py_DECREF(container);
            // do_Py_DECREF(sub);
            // auto no_err = builder.CreateICmpSGE(err, asValue(0));
            // auto bb = createBlock("DELETE_SUBSCR.OK");
            // builder.CreateCondBr(no_err, bb, unwind_block, likely_true);
            // builder.SetInsertPoint(bb);
            break;
        }
        case UNARY_POSITIVE:
            handle_UNARY_OP(searchSymbol<PyNumber_Positive>());
            break;
        case UNARY_NEGATIVE:
            handle_UNARY_OP(searchSymbol<PyNumber_Negative>());
            break;
        case UNARY_NOT:
            handle_UNARY_OP(searchSymbol<handle_UNARY_NOT>());
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
        case BINARY_FLOOR_DIVIDE:
            handle_BINARY_OP(searchSymbol<PyNumber_FloorDivide>());
            break;
        case BINARY_TRUE_DIVIDE:
            handle_BINARY_OP(searchSymbol<PyNumber_TrueDivide>());
            break;
        case BINARY_MODULO:
            handle_BINARY_OP(searchSymbol<PyNumber_Remainder>());
            break;
        case BINARY_POWER:
            handle_BINARY_OP(searchSymbol<handle_BINARY_POWER>());
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
        case INPLACE_FLOOR_DIVIDE:
            handle_BINARY_OP(searchSymbol<PyNumber_InPlaceFloorDivide>());
            break;
        case INPLACE_TRUE_DIVIDE:
            handle_BINARY_OP(searchSymbol<PyNumber_InPlaceTrueDivide>());
            break;
        case INPLACE_MODULO:
            handle_BINARY_OP(searchSymbol<PyNumber_InPlaceRemainder>());
            break;
        case INPLACE_POWER:
            handle_BINARY_OP(searchSymbol<handle_INPLACE_POWER>());
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
        case IS_OP:
            unimplemented();
        case CONTAINS_OP:
            unimplemented();
        case RETURN_VALUE: {
            auto retval = do_POP();
            builder.CreateRet(retval);
            fall_through = false;
            break;
        }
        case CALL_FUNCTION:
            unimplemented();
        case CALL_FUNCTION_KW:
            unimplemented();
        case CALL_FUNCTION_EX:
            unimplemented();
        case CALL_METHOD:
            unimplemented();
        case LOAD_CLOSURE:
            unimplemented();
        case MAKE_FUNCTION:
            unimplemented();
        case LOAD_BUILD_CLASS:
            unimplemented();

        case IMPORT_NAME:
            unimplemented();
        case IMPORT_FROM:
            unimplemented();
        case IMPORT_STAR:
            unimplemented();

        case JUMP_FORWARD: {
            auto jmp_target = findBlock(instr.oparg + instr.oparg);
            builder.CreateBr(jmp_target);
            fall_through = false;
            break;
        }
        case JUMP_ABSOLUTE: {
            auto jmp_target = findBlock(instr.oparg);
            builder.CreateBr(jmp_target);
            fall_through = false;
            break;
        }
        case JUMP_IF_FALSE_OR_POP:
            unimplemented();
        case JUMP_IF_TRUE_OR_POP:
            unimplemented();
        case POP_JUMP_IF_FALSE: {
            auto cond = do_POP();
            auto err = do_CallSymbol<PyObject_IsTrue>(cond);
            do_Py_DECREF(cond);
            auto cmp = builder.CreateICmpEQ(err, asValue(0));
            auto jmp_target = findBlock(instr.oparg);
            builder.CreateCondBr(cmp, jmp_target, blocks[index + 1].llvm_block);
            fall_through = false;
            // auto cond = do_POP();
            // auto err = WrappedValue{do_CallSymbol<PyObject_IsTrue>(cond), *this};
            // do_Py_DECREF(cond);
            // auto zero = WrappedValue{asValue(0), *this};
            // auto cmp = err == zero;
            // auto jmp_target = findBlock(instr.oparg);
            // builder.CreateCondBr(cmp.value, jmp_target, blocks[index + 1].llvm_block);
            // fall_through = false;
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
            auto cmp = builder.CreateICmpEQ(next, c_null);
            auto b_continue = createBlock("FOR_ITER.continue");
            auto b_break = createBlock("FOR_ITER.break");
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

        case BUILD_TUPLE:
            unimplemented();
        case BUILD_LIST:
            unimplemented();
        case BUILD_SET:
            unimplemented();
        case BUILD_MAP:
            unimplemented();
        case BUILD_CONST_KEY_MAP:
            unimplemented();
        case LIST_APPEND:
            unimplemented();
        case SET_ADD:
            unimplemented();
        case MAP_ADD:
            unimplemented();
        case LIST_EXTEND:
            unimplemented();
        case SET_UPDATE:
            unimplemented();
        case DICT_MERGE:
            unimplemented();
        case DICT_UPDATE:
            unimplemented();
        case LIST_TO_TUPLE:
            unimplemented();

        case FORMAT_VALUE:
            unimplemented();
        case BUILD_STRING:
            unimplemented();

        case UNPACK_SEQUENCE:
            unimplemented();
        case UNPACK_EX:
            unimplemented();

        case GET_LEN:
            unimplemented();
        case MATCH_MAPPING:
            unimplemented();
        case MATCH_SEQUENCE:
            unimplemented();
        case MATCH_KEYS:
            unimplemented();
        case MATCH_CLASS:
            unimplemented();
        case COPY_DICT_WITHOUT_KEYS:
            unimplemented();

        case BUILD_SLICE:
            unimplemented();
        case LOAD_ASSERTION_ERROR:
            unimplemented();
        case RAISE_VARARGS:
            unimplemented();
        case SETUP_ANNOTATIONS:
            unimplemented();
        case PRINT_EXPR:
            unimplemented();

        case SETUP_FINALLY:
            unimplemented();
        case POP_BLOCK:
            unimplemented();
        case JUMP_IF_NOT_EXC_MATCH:
            unimplemented();
        case POP_EXCEPT:
            unimplemented();
        case RERAISE:
        case SETUP_WITH:
            unimplemented();
        case WITH_EXCEPT_START:
            unimplemented();

        case GEN_START:
            unimplemented();
        case YIELD_VALUE:
            unimplemented();
        case GET_YIELD_FROM_ITER:
            unimplemented();
        case YIELD_FROM:
            unimplemented();
        case GET_AWAITABLE:
            unimplemented();
        case GET_AITER:
            unimplemented();
        case GET_ANEXT:
            unimplemented();
        case END_ASYNC_FOR:
            unimplemented();
        case SETUP_ASYNC_WITH:
            unimplemented();
        case BEFORE_ASYNC_WITH:
            unimplemented();
        default:
            throw runtime_error("illegal opcode");
        }
        // instr.fetch_next();
    }
    if (fall_through) {
        assert(index < block_num - 1);
        builder.CreateBr(blocks[index + 1].llvm_block);
    }
}

BasicBlock *Translator::findBlock(unsigned offset) {
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
        //TODO: 重命名或者两个tbaa
        // TODO: 合并为READ
        auto ptr = getPointer<FunctionPointer>(func->getArg(0), i, useName("$symbol.", i));
        symbol = loadValue<FunctionPointer>(ptr, tbaa_code_const, useName("$symbol.", name));
        builder.SetInsertPoint(block);
    }
    return symbol;
}
