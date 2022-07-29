#include <Python.h>

#include "compile_unit.h"


using namespace std;
using namespace llvm;

void DebugInfoBuilder::setFunction(llvm::IRBuilder<> &ir_builder, PyCodeObject *py_code, llvm::Function *function) {
    function->setName(PyStringAsString(py_code->co_name));
    auto res = callDebugHelperFunction("get_pydis_path", reinterpret_cast<PyObject *>(py_code));
    auto file = builder.createFile(PyStringAsString(res), "");
    builder.createCompileUnit(llvm::dwarf::DW_LANG_C, file, "", false, "", 0);
    sp = builder.createFunction(file, "", "", file, 1, builder.createSubroutineType({}), 1,
            llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);
    function->setSubprogram(sp);
    setLocation(ir_builder, -2);
};

void DebugInfoBuilder::setLocation(llvm::IRBuilder<> &ir_builder, int vpc) {
    ir_builder.SetCurrentDebugLocation(llvm::DILocation::get(ir_builder.getContext(), vpc + 3, 0, sp));
}


void CompileUnit::translate() {
    function = Function::Create(context.type<CompiledFunction>(),
            Function::ExternalLinkage, "the_function", &llvm_module);
    function->setAttributes(context.attr_default_call);
    di_builder.setFunction(builder, py_code, function);

    (shared_symbols = function->getArg(0))->setName(useName("symbols"));
    (frame_obj = function->getArg(1))->setName(useName("frame"));
    // TODO: 下面的，不知是否有必要
    shared_symbols->addAttr(Attribute::NoAlias);
    frame_obj->addAttr(Attribute::NoAlias);

    // TODO: 重复了
    parseCFG();
    doIntraBlockAnalysis();
    doInterBlockAnalysis();

    entry_block = createBlock(useName("entry_block"));
    entry_block->insertInto(function);
    builder.SetInsertPoint(entry_block);

    auto rt_code = loadFieldValue(frame_obj, &PyFrameObject::f_code, context.tbaa_frame_value, useName("code"));
#ifdef PRELOAD
    auto num_of_names = PyTuple_GET_SIZE(py_code->co_names);
    auto num_of_consts = PyTuple_GET_SIZE(py_code->co_consts);
    auto num_of_locals = py_code->co_nlocals;
    auto num_of_frees = PyTuple_GET_SIZE(py_code->co_cellvars) + PyTuple_GET_SIZE(py_code->co_freevars);
    auto num_of_stack_values = py_code->co_stacksize;
    value_pointers.reserve(num_of_names + num_of_consts + num_of_locals + num_of_frees + num_of_stack_values);

    name_slots = value_pointers.getPointer(0);
    auto rt_code_names_items = getPointer(
            loadFieldValue(rt_code, &PyCodeObject::co_names, context.tbaa_frame_value),
            &PyTupleObject::ob_item);
    for (auto i : IntRange(num_of_names)) {
        name_slots[i] = getPointer<PyObject *>(rt_code_names_items, i, useName("name_slots.", i));
    }

    const_slots = name_slots + num_of_names;
    auto rt_co_consts_items = getPointer(
            loadFieldValue(rt_code, &PyCodeObject::co_consts, context.tbaa_code_const),
            &PyTupleObject::ob_item);
    for (auto i : IntRange(num_of_consts)) {
        const_slots[i] = getPointer<PyObject *>(rt_co_consts_items, i, useName("const_slots.", i));
    }

    auto offset_in_frame = offsetof(PyFrameObject, f_localsplus);
    local_slots = const_slots + num_of_consts;
    for (auto i : IntRange(num_of_locals)) {
        local_slots[i] = getPointer<char>(frame_obj, offset_in_frame, useName("local_slots.", i));
        offset_in_frame += sizeof(PyObject *);
    }

    free_slots = local_slots + num_of_locals;
    for (auto i : IntRange(num_of_frees)) {
        free_slots[i] = getPointer<char>(frame_obj, offset_in_frame, useName("cell_free_slots.", i));
        offset_in_frame += sizeof(PyObject *);
    }

    stack_slots = free_slots + num_of_frees;
    for (auto i : IntRange(num_of_stack_values)) {
        stack_slots[i] = getPointer<char>(frame_obj, offset_in_frame, useName("cell_slots.", i));
        offset_in_frame += sizeof(PyObject *);
    }
#else
    code_names = getPointer(loadFieldValue(rt_code, &PyCodeObject::co_names,
            context.tbaa_frame_value), &PyTupleObject::ob_item, "co_names");
    code_consts = getPointer(loadFieldValue(rt_code, &PyCodeObject::co_consts,
            context.tbaa_code_const), &PyTupleObject::ob_item, "consts");
#endif
    rt_lasti = getPointer(frame_obj, &PyFrameObject::f_lasti, "lasti");

    auto offset = offsetof(PyFrameObject, f_blockstack) +
            sizeof(PyTryBlock) * (CO_MAXBLOCKS - 1) +
            offsetof(PyTryBlock, b_handler);
    coroutine_handler = getPointer<char>(frame_obj, offset, "coroutine_handler");
    entry_jump = builder.CreateIndirectBr(builder.CreateInBoundsGEP(
            context.type<char>(), BlockAddress::get(function, blocks[0]),
            loadValue<int>(coroutine_handler, context.tbaa_frame_value, "jump_to_offset")
    ), handler_num);
    entry_jump->addDestination(blocks[0]);

    error_block = createBlock("raise_error");

    abstract_stack.reserve(py_code->co_stacksize);
    for (auto &b : PtrRange(blocks.getPointer(), block_num)) {
        abstract_stack_height = stack_height = 0;
        b.block->insertInto(function);
        builder.SetInsertPoint(b);
        emitBlock(b);
    }

    error_block->insertInto(function);
    builder.SetInsertPoint(error_block);
    callSymbol<raiseException, &Context::attr_noreturn>();
    builder.CreateUnreachable();

    di_builder.finalize();
}

void CompileUnit::do_Py_INCREF(Value *py_obj) {
    // callSymbol<handle_INCREF, &Translator::attr_refcnt_call>(py_obj);
    Value *ref = py_obj;
    if constexpr (offsetof(PyObject, ob_refcnt)) {
        ref = getPointer(py_obj, &PyObject::ob_refcnt);
    }
    auto old_value = loadValue<decltype(PyObject::ob_refcnt)>(ref, context.tbaa_refcnt);
    auto *delta_1 = asValue(decltype(PyObject::ob_refcnt){1});
    auto new_value = builder.CreateAdd(old_value, delta_1);
    storeValue<decltype(PyObject::ob_refcnt)>(new_value, ref, context.tbaa_refcnt);
}

void CompileUnit::do_Py_DECREF(Value *py_obj) {
    // callSymbol<handle_DECREF, &Translator::attr_refcnt_call>(py_obj);
    Value *ref = py_obj;
    if constexpr (offsetof(PyObject, ob_refcnt)) {
        ref = getPointer(py_obj, &PyObject::ob_refcnt);
    }
    auto old_value = loadValue<decltype(PyObject::ob_refcnt)>(ref, context.tbaa_refcnt);
    auto *delta_1 = asValue(decltype(PyObject::ob_refcnt){1});
    auto *zero = asValue(decltype(PyObject::ob_refcnt){0});
    auto new_value = builder.CreateSub(old_value, delta_1);
    storeValue<decltype(PyObject::ob_refcnt)>(new_value, ref, context.tbaa_refcnt);
    auto b_dealloc = appendBlock("dealloc");
    auto b_end = appendBlock("dealloc.end");
    builder.CreateCondBr(builder.CreateICmpEQ(new_value, zero), b_dealloc, b_end, context.likely_true);
    builder.SetInsertPoint(b_dealloc);
    callSymbol<handle_dealloc, &Context::attr_refcnt_call>(py_obj);
    builder.CreateBr(b_end);
    builder.SetInsertPoint(b_end);
}

void CompileUnit::do_Py_XDECREF(Value *py_obj) {
    // callSymbol<handle_XDECREF, &Translator::attr_refcnt_call>(py_obj);
    auto b_decref = appendBlock("decref");
    auto b_end = appendBlock("decref.end");
    builder.CreateCondBr(builder.CreateICmpNE(py_obj, context.c_null), b_decref, b_end, context.likely_true);
    builder.SetInsertPoint(b_decref);
    do_Py_DECREF(py_obj);
    builder.CreateBr(b_end);
    builder.SetInsertPoint(b_end);
}

std::pair<llvm::Value *, llvm::Value *> CompileUnit::do_GETLOCAL(PyOparg oparg) {
    auto varname = PyTuple_GET_ITEM(py_code->co_varnames, oparg);
    auto offset = offsetof(PyFrameObject, f_localsplus) + sizeof(PyObject *) * oparg;
    auto slot = getPointer<char>(frame_obj, offset, useName("local.", varname, "."));
    auto value = loadValue<PyObject *>(slot, context.tbaa_frame_value, useName(varname));
    return {slot, value};
}

Value *CompileUnit::getStackSlot(int i) {
    assert(stack_height >= i);
#ifdef PRELOAD
    return stack_slots[stack_height - i];
#else
    auto offset = offsetof(PyFrameObject, f_localsplus) + sizeof(PyObject *) * (
            stack_height - i +
                    py_code->co_nlocals +
                    PyTuple_GET_SIZE(py_code->co_cellvars) +
                    PyTuple_GET_SIZE(py_code->co_freevars)
    );
    return getPointer<char>(frame_obj, offset, useName("stack.", i, "."));
#endif
}

CompileUnit::FetchedStackValue CompileUnit::fetchStackValue(int i) {
    return FetchedStackValue{abstract_stack[abstract_stack_height - i]};
}

CompileUnit::PoppedStackValue CompileUnit::do_POP() {
    auto v = abstract_stack[--abstract_stack_height];
    stack_height -= v.really_pushed;
    return PoppedStackValue{v};
}

Value *CompileUnit::do_POP_with_newref() {
    auto &v = abstract_stack[--abstract_stack_height];
    stack_height -= v.really_pushed;
    if (!v.really_pushed) {
        do_Py_INCREF(v.value);
    }
    return v.value;
}

void CompileUnit::popAndSave(llvm::Value *slot, llvm::MDNode *tbaa_node) {
    storeValue<PyObject *>(do_POP_with_newref(), slot, tbaa_node);
}

void CompileUnit::do_PUSH(llvm::Value *value) {
    auto &stack_value = abstract_stack[abstract_stack_height++];
    stack_value.value = value;
    stack_value.really_pushed = true;
    storeValue<PyObject *>(value, getStackSlot(), context.tbaa_frame_value);
    stack_height++;
}

void CompileUnit::do_RedundantPUSH(llvm::Value *value, bool is_local, PyOparg index) {
    // TODO: 构造函数赋值吧
    auto &stack_value = abstract_stack[abstract_stack_height++];
    stack_value.value = value;
    stack_value.really_pushed = false;
    stack_value.is_local = is_local;
    stack_value.index = index;
}

Value *CompileUnit::getName(int i) {
#ifdef PRELOAD
    return loadValue<PyObject *>(value_pointers[i], context.tbaa_code_const,
            useName("name.", PyTuple_GET_ITEM(py_code->co_names, i)));
#else
    PyObject * repr{};
    if constexpr (debug_build) {
        repr = PyTuple_GET_ITEM(py_code->co_names, i);
    }
    auto ptr = getPointer<PyObject *>(code_names, i);
    return loadValue<PyObject *>(ptr, context.tbaa_code_const, useName(repr, "$"));
#endif
}

Value *CompileUnit::getFreevar(int i) {
#ifdef PRELOAD
    PyObject * name{};
    if constexpr (debug_build) {
        if (i < PyTuple_GET_SIZE(py_code->co_cellvars)) {
            name = PyTuple_GET_ITEM(py_code->co_cellvars, i);
        } else {
            name = PyTuple_GET_ITEM(py_code->co_freevars, i - PyTuple_GET_SIZE(py_code->co_cellvars));
        }
    }
    // TODO: tbaa_frame_value还是const
    return loadValue<PyObject *>(free_slots[i], context.tbaa_frame_value, useName(name));
#else
    auto offset = offsetof(PyFrameObject, f_localsplus) + sizeof(PyObject *) * (py_code->co_nlocals + i);
    PyObject * name{};
    if constexpr (debug_build) {
        if (i < PyTuple_GET_SIZE(py_code->co_cellvars)) {
            name = PyTuple_GET_ITEM(py_code->co_cellvars, i);
        } else {
            name = PyTuple_GET_ITEM(py_code->co_freevars, i - PyTuple_GET_SIZE(py_code->co_cellvars));
        }
    }
    auto slot = getPointer<char>(frame_obj, offset, useName("free.", name, "."));
    // TODO: tbaa_frame_value还是const
    return loadValue<PyObject *>(slot, context.tbaa_frame_value, useName(name));
#endif
}

llvm::Value *CompileUnit::getSymbol(size_t offset) {
    const char *name = nullptr;
    if constexpr (debug_build) {
        name = symbol_names[offset];
    }
    auto ptr = getPointer<void *>(shared_symbols, offset, useName("sym.", offset, "."));
    return loadValue<void *>(ptr, context.tbaa_symbols, useName("sym.", name, "."));
}

void CompileUnit::pyJumpIF(PyBasicBlock &current, bool pop_if_jump, bool jump_cond) {
    auto cond_obj = do_POP();

    auto fall_block = cond_obj.really_pushed ? appendBlock("") : current.next();
    auto jump_block = cond_obj.really_pushed && pop_if_jump ? appendBlock("") : *current.branch;
    auto fast_cmp_block = appendBlock("");
    auto slow_cmp_block = appendBlock("");
    auto true_block = jump_cond ? jump_block : fall_block;
    auto false_block = jump_cond ? fall_block : jump_block;

    auto py_true = getSymbol(searchSymbol<_Py_TrueStruct>());
    builder.CreateCondBr(builder.CreateICmpEQ(cond_obj, py_true), true_block, fast_cmp_block);
    builder.SetInsertPoint(fast_cmp_block);
    auto py_false = getSymbol(searchSymbol<_Py_FalseStruct>());
    builder.CreateCondBr(builder.CreateICmpEQ(cond_obj, py_false), false_block, slow_cmp_block, context.likely_true);
    builder.SetInsertPoint(slow_cmp_block);
    builder.CreateCondBr(callSymbol<castPyObjectToBool>(cond_obj), true_block, false_block);

    if (cond_obj.really_pushed) {
        if (pop_if_jump) {
            builder.SetInsertPoint(jump_block);
            do_Py_DECREF(cond_obj);
            builder.CreateBr(*current.branch);
        }
        builder.SetInsertPoint(fall_block);
        do_Py_DECREF(cond_obj);
        builder.CreateBr(current.next());
    }
}

// TODO: 直接加载不好，最好延迟
void CompileUnit::declareStackGrowth(int n) {
    for ([[maybe_unused]]auto i : IntRange(n)) {
        auto &v = abstract_stack[abstract_stack_height];
        v.really_pushed = true;
        v.value = loadValue<PyObject *>(getStackSlot(0), context.tbaa_frame_value);
        abstract_stack_height++;
        stack_height++;
    }
}

void CompileUnit::refreshAbstractStack() {
    auto j = 0;
    for (auto i : IntRange(abstract_stack_height)) {
        auto &v = abstract_stack[abstract_stack_height - i - 1];
        if (v.really_pushed) {
            v.value = loadValue<PyObject *>(getStackSlot(++j), context.tbaa_frame_value);
        } else if (v.is_local) {
            v.value = do_GETLOCAL(v.index).second;
        } else {
            // TODO: get const也作为函数
#ifdef PRELOAD
            auto ptr = const_slots[v.index];
#else
            auto ptr = getPointer<PyObject *>(code_consts, v.index);
#endif
            v.value = loadValue<PyObject *>(ptr, context.tbaa_code_const);
        }
    }
    assert(j == stack_height);
}

static void notifyCodeLoaded(PyObject * py_code, void * code_addr) {}

CompileUnit::TranslatedResult *CompileUnit::emit(Translator &translator, PyObject *py_code) {
    if constexpr (debug_build) {
        callDebugHelperFunction("dump_pydis", py_code);
    }

    CompileUnit cu{translator};
    cu.py_code = reinterpret_cast<PyCodeObject *>(py_code);
    cu.llvm_module.setDataLayout(translator.machine->createDataLayout());
    cu.translate();

    if constexpr (debug_build) {
        SmallVector<char> ll_vec{};
        raw_svector_ostream os{ll_vec};
        cu.llvm_module.print(os, nullptr);
        callDebugHelperFunction("dump_binary", py_code,
                PyObjectRef{PyUnicode_FromString(".ll")},
                PyObjectRef{PyMemoryView_FromMemory(ll_vec.data(), ll_vec.size(), PyBUF_READ)});
    }
    auto &obj = translator.compile(cu.llvm_module);
    if constexpr (debug_build) {
        SmallVector<char> ll_vec{};
        raw_svector_ostream os{ll_vec};
        cu.llvm_module.print(os, nullptr);
        callDebugHelperFunction("dump_binary", py_code,
                PyObjectRef{PyUnicode_FromString(".opt.ll")},
                PyObjectRef{PyMemoryView_FromMemory(ll_vec.data(), ll_vec.size(), PyBUF_READ)});
        callDebugHelperFunction("dump_binary", py_code,
                PyObjectRef{PyUnicode_FromString(".o")},
                PyObjectRef{PyMemoryView_FromMemory(obj.data(), obj.size(), PyBUF_READ)});
    }
    auto memory = loadCode(obj);
    obj.resize(0);
    // TODO: cout capcity
    notifyCodeLoaded(py_code, memory.base());

    return new CompileUnit::TranslatedResult{memory, move(cu.vpc_to_stack_height)};
}

void CompileUnit::emitRotN(PyOparg n) {
    auto abs_top = abstract_stack[abstract_stack_height - 1];
    unsigned n_lift = 0;
    for (auto i : IntRange(1, n)) {
        auto v = abstract_stack[abstract_stack_height - i] = abstract_stack[abstract_stack_height - (i + 1)];
        n_lift += v.really_pushed;
    }
    abstract_stack[abstract_stack_height - n] = abs_top;

    if (abs_top.really_pushed && n_lift) {
        auto dest_begin = getStackSlot(1);
        auto dest_end = getStackSlot(n_lift + 1);
        auto top = loadValue<PyObject *>(dest_begin, context.tbaa_frame_value);
        if (n_lift <= 8) {
            auto dest = dest_begin;
            for (auto i : IntRange(2, n_lift + 2)) {
                auto src = getStackSlot(i);
                auto value = loadValue<PyObject *>(src, context.tbaa_frame_value);
                storeValue<PyObject *>(value, dest, context.tbaa_frame_value);
                dest = src;
            }
        } else {
            auto src_start = getStackSlot(2);
            auto current_block = builder.GetInsertBlock();
            auto loop_block = appendBlock("ROT_N.loop");
            auto end_block = appendBlock("ROT_N.end");
            builder.CreateBr(loop_block);
            builder.SetInsertPoint(loop_block);
            auto dest = builder.CreatePHI(context.type<PyObject *>(), 2);
            dest->addIncoming(dest_begin, current_block);
            auto src = builder.CreatePHI(context.type<PyObject *>(), 2);
            src->addIncoming(src_start, current_block);
            auto value = loadValue<PyObject *>(src, context.tbaa_frame_value);
            storeValue<PyObject *>(value, dest, context.tbaa_frame_value);
            auto next_src = getPointer<PyObject *>(src, -1);
            builder.CreateCondBr(builder.CreateICmpEQ(dest, dest_end), end_block, loop_block);
            dest->addIncoming(src, loop_block);
            src->addIncoming(next_src, loop_block);
            builder.SetInsertPoint(end_block);
        }
        storeValue<PyObject *>(top, dest_end, context.tbaa_frame_value);
        return;
    }
}
