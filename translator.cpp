#include <memory>

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
        b.is_handler = false;
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
    auto indirect_br = builder.CreateIndirectBr(
            builder.CreateInBoundsGEP(types.get<char>(), BlockAddress::get(blocks[1].block), func->getArg(2)));
    blocks[1].is_handler = true;
    for (auto &b : Range(block_num - 1, &blocks[1])) {
        if (b.is_handler) {
            indirect_br->addDestination(b.block);
        }
    }

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
    Py_UNREACHABLE();
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
    auto py_true = getSymbol(searchSymbol<_Py_TrueStruct>());
    builder.CreateCondBr(builder.CreateICmpEQ(obj, py_true), true_block, false_cmp_block);
    builder.SetInsertPoint(false_cmp_block);
    auto py_false = getSymbol(searchSymbol<_Py_FalseStruct>());
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
