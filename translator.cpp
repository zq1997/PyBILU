#include <memory>

#include <Python.h>

#include "translator.h"
#include "memory_manager.h"


using namespace std;
using namespace llvm;


Translator::Translator(const DataLayout &dl) : data_layout{dl} {
    mod.setDataLayout(data_layout);

    auto md_builder = MDBuilder(context);
    auto tbaa_root = md_builder.createTBAARoot("TBAA root for CPython");
    const auto &createTBAA = [&](const char *name, bool is_const = false) {
        // TODO: 为啥name为空就会被合并
        // if constexpr(!debug_build) {
        //     name = "";
        // }
        auto scalar_node = md_builder.createTBAANode(name, tbaa_root);
        return md_builder.createTBAAStructTagNode(scalar_node, scalar_node, 0, is_const);
    };
    tbaa_refcnt = createTBAA("refcnt");
    tbaa_frame_slot = createTBAA("frame slot");
    tbaa_frame_cells = createTBAA("frame cells");
    tbaa_code_const = createTBAA("code const", true);

    likely_true = md_builder.createBranchWeights(INT32_MAX, 0);

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
    func->getArg(0)->setName(useName("symbols"));
    (frame_obj = func->getArg(1))->setName(useName("frame"));
    // TODO: 下面的，不知是否有必要
    func->getArg(0)->addAttr(Attribute::NoAlias);
    func->getArg(1)->addAttr(Attribute::NoAlias);
}


Translator::TranslatedResult *Translator::translate(Compiler &compiler, PyCodeObject *code) {
    py_code = code;
    // TODO: 重复了
    parseCFG();

    blocks[0].block = createBlock("entry_block", func);
    auto start = blocks[0].end;
    for (auto &b : Range(block_num - 1, &blocks[1])) {
        b.initial_stack_height = -1;
        b.is_handler = false;
        b.block = createBlock(useName("instr.", start), nullptr);
        start = b.end;
    }
    blocks[1].initial_stack_height = 0;

    builder.SetInsertPoint(blocks[0].block);

    auto code_obj = readData(frame_obj, &PyFrameObject::f_code, useName("code"));
    auto names_tuple = loadFieldValue(code_obj, &PyCodeObject::co_names, tbaa_frame_slot);
    code_names = getPointer(names_tuple, &PyTupleObject::ob_item, "names");
    auto consts_tuple = loadFieldValue(code_obj, &PyCodeObject::co_consts, tbaa_code_const);
    code_consts = getPointer(consts_tuple, &PyTupleObject::ob_item, "consts");
    rt_lasti = getPointer(frame_obj, &PyFrameObject::f_lasti, "lasti");

    error_block = createBlock("error_here", nullptr);

    for (auto &i : Range(block_num - 1, 1U)) {
        // 队列(栈)式生成
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

    di_builder.finalizeSubprogram(di_function);
    assert(!verifyModule(mod, &llvm::errs()));
    auto addr = compiler.compile(py_code, mod);

    // TODO: 可能需要考虑mod.dropAllReferences();不复用函数了，改成复用module，甚至都不复用
    func->dropAllReferences();
    return new Translator::TranslatedResult{
            addr,
            &vpc_to_stack_depth[0]
    };
}

void Translator::parseCFG() {
    py_instructions = reinterpret_cast<PyInstr *>(PyBytes_AS_STRING(py_code->co_code));
    auto size = PyBytes_GET_SIZE(py_code->co_code) / sizeof(PyInstr);
    vpc_to_stack_depth.reserve(size);

    BitArray is_boundary(size + 1);
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
    // do_CallSymbol<handle_INCREF, &Translator::attr_return>(py_obj);
    auto ref = getPointer(py_obj, &PyObject::ob_refcnt);
    auto old_value = loadValue<decltype(PyObject::ob_refcnt)>(ref, tbaa_refcnt);
    auto *delta_1 = asValue(decltype(PyObject::ob_refcnt){1});
    auto new_value = builder.CreateAdd(old_value, delta_1);
    builder.CreateAssumption(builder.CreateICmpSGT(new_value, delta_1)); // 需要么
    storeValue<decltype(PyObject::ob_refcnt)>(new_value, ref, tbaa_refcnt);
}

void Translator::do_Py_DECREF(Value *py_obj) {
    // do_CallSymbol<handle_DECREF, &Translator::attr_return>(py_obj);
    auto ref = getPointer(py_obj, &PyObject::ob_refcnt);
    auto old_value = loadValue<decltype(PyObject::ob_refcnt)>(ref, tbaa_refcnt);
    auto *delta_1 = asValue(decltype(PyObject::ob_refcnt){1});
    auto *zero = asValue(decltype(PyObject::ob_refcnt){0});
    auto new_value = builder.CreateSub(old_value, delta_1);
    storeValue<decltype(PyObject::ob_refcnt)>(new_value, ref, tbaa_refcnt);
    auto b_dealloc = createBlock("dealloc");
    auto b_end = createBlock("dealloc.end");
    builder.CreateCondBr(builder.CreateICmpEQ(new_value, zero), b_dealloc, b_end, likely_true);
    builder.SetInsertPoint(b_dealloc);
    do_CallSymbol<_Py_Dealloc>(py_obj);
    builder.CreateBr(b_end);
    builder.SetInsertPoint(b_end);
}

void Translator::do_Py_XDECREF(Value *py_obj) {
    // do_CallSymbol<handle_XDECREF, &Translator::attr_return>(py_obj);
    auto b_decref = createBlock("decref");
    auto b_end = createBlock("decref.end");
    builder.CreateCondBr(builder.CreateICmpNE(py_obj, c_null), b_decref, b_end, likely_true);
    builder.SetInsertPoint(b_decref);
    do_Py_DECREF(py_obj);
    builder.CreateBr(b_end);
    builder.SetInsertPoint(b_end);
}

Value *Translator::do_GETLOCAL(PyOparg oparg) {
    auto varname = PyTuple_GET_ITEM(py_code->co_varnames, oparg);
    auto offset = offsetof(PyFrameObject, f_localsplus) + sizeof(PyObject *) * oparg;
    auto slot = getPointer<char>(frame_obj, offset, useName("local.", varname, "."));
    return loadValue<PyObject *>(slot, tbaa_frame_slot, useName(varname));
}

void Translator::do_SETLOCAL(PyOparg oparg, llvm::Value *value) {
    auto varname = PyTuple_GET_ITEM(py_code->co_varnames, oparg);
    auto offset = offsetof(PyFrameObject, f_localsplus) + sizeof(PyObject *) * oparg;
    auto slot = getPointer<char>(frame_obj, offset, useName("local.", varname, "."));
    auto old_value = loadValue<PyObject *>(slot, tbaa_frame_slot, useName(varname, ".old", "."));
    storeValue<PyObject *>(value, slot, tbaa_frame_slot);
    do_Py_XDECREF(old_value);
}

Value *Translator::getStackSlot(int i) {
    assert(stack_height >= i);
    auto offset = offsetof(PyFrameObject, f_localsplus) + sizeof(PyObject *) * (stack_height - i +
            py_code->co_nlocals +
            PyTuple_GET_SIZE(py_code->co_cellvars) +
            PyTuple_GET_SIZE(py_code->co_freevars)
    );
    return getPointer<char>(frame_obj, offset, useName("stack.", i, "."));
}

Value *Translator::do_PEAK(int i) {
    return loadValue<PyObject *>(getStackSlot(i), tbaa_frame_cells);
}

void Translator::do_SET_PEAK(int i, llvm::Value *value) {
    storeValue<PyObject *>(value, getStackSlot(i), tbaa_frame_cells);
}

llvm::Value *Translator::do_POP() {
    --stack_height;
    return do_PEAK(0);
}

void Translator::do_PUSH(llvm::Value *value) {
    do_SET_PEAK(0, value);
    stack_height++;
}

Value *Translator::getName(int i) {
    PyObject *repr{};
    if constexpr(debug_build) {
        repr = PyTuple_GET_ITEM(py_code->co_names, i);
    }
    return loadElementValue<PyObject *>(code_names, i, tbaa_code_const, useName(repr, "$"));
}

Value *Translator::getFreevar(int i) {
    auto offset = offsetof(PyFrameObject, f_localsplus) + sizeof(PyObject *) * (py_code->co_nlocals + i);
    PyObject *name{};
    if constexpr(debug_build) {
        if (i < PyTuple_GET_SIZE(py_code->co_cellvars)) {
            name = PyTuple_GET_ITEM(py_code->co_cellvars, i);
        } else {
            name = PyTuple_GET_ITEM(py_code->co_freevars, i - PyTuple_GET_SIZE(py_code->co_cellvars));
        }
    }
    auto slot = getPointer<char>(frame_obj, offset, useName("free.", name, "."));
    return loadValue<PyObject *>(slot, tbaa_frame_slot, useName(name));
}

llvm::Value *Translator::getSymbol(size_t offset) {
    const char *name = nullptr;
    if constexpr (debug_build) {
        name = symbol_names[offset];
    }
    auto ptr = getPointer<void *>(func->getArg(0), offset, useName("sym.", offset, "."));
    return loadValue<void *>(ptr, tbaa_code_const, useName("sym.", name, "."));
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
