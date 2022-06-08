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

    auto code_obj = loadFieldValue(frame_obj, &PyFrameObject::f_code, context.tbaa_frame_value, useName("code"));
    auto names_tuple = loadFieldValue(code_obj, &PyCodeObject::co_names, context.tbaa_frame_value);
    code_names = getPointer(names_tuple, &PyTupleObject::ob_item, "names");
    auto consts_tuple = loadFieldValue(code_obj, &PyCodeObject::co_consts, context.tbaa_code_const);
    code_consts = getPointer(consts_tuple, &PyTupleObject::ob_item, "consts");
    rt_lasti = getPointer(frame_obj, &PyFrameObject::f_lasti, "lasti");

    error_block = createBlock("raise_error");

    abstract_stack.reserve(py_code->co_stacksize);
    for (auto &b : PtrRange(&blocks[0], block_num)) {
        abstract_stack_height = stack_height = b.initial_stack_height;
        emitBlock(b);
    }

    error_block->insertInto(function);
    builder.SetInsertPoint(error_block);
    callSymbol<raiseException, &Context::attr_noreturn>();
    builder.CreateUnreachable();

    // TODO: debug location有时候去除
    builder.SetInsertPoint(entry_block);
    auto indirect_br = builder.CreateIndirectBr(
            builder.CreateInBoundsGEP(context.type<char>(), BlockAddress::get(blocks[0]),
                    function->getArg(2)));
    for (auto &b : PtrRange(blocks.getPointer(), block_num)) {
        if (b.is_handler) {
            indirect_br->addDestination(b);
        }
    }

    di_builder.finalize();
}

void CompileUnit::do_Py_INCREF(Value *py_obj) {
    // callSymbol<handle_INCREF, &Translator::attr_refcnt_call>(py_obj);
    auto ref = getPointer(py_obj, &PyObject::ob_refcnt);
    auto old_value = loadValue<decltype(PyObject::ob_refcnt)>(ref, context.tbaa_refcnt);
    auto *delta_1 = asValue(decltype(PyObject::ob_refcnt){1});
    auto new_value = builder.CreateAdd(old_value, delta_1);
    // builder.CreateAssumption(builder.CreateICmpSGT(new_value, delta_1)); // 需要么
    storeValue<decltype(PyObject::ob_refcnt)>(new_value, ref, context.tbaa_refcnt);
}

void CompileUnit::do_Py_DECREF(Value *py_obj) {
    // callSymbol<handle_DECREF, &Translator::attr_refcnt_call>(py_obj);
    auto ref = getPointer(py_obj, &PyObject::ob_refcnt);
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
    auto offset = offsetof(PyFrameObject, f_localsplus) + sizeof(PyObject *) * (stack_height - i +
            py_code->co_nlocals +
            PyTuple_GET_SIZE(py_code->co_cellvars) +
            PyTuple_GET_SIZE(py_code->co_freevars)
    );
    return getPointer<char>(frame_obj, offset, useName("stack.", i, "."));
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

void CompileUnit::do_PUSH(llvm::Value *value, bool really_pushed) {
    abstract_stack[abstract_stack_height++] = {value, really_pushed};
    if (really_pushed) {
        storeValue<PyObject *>(value, getStackSlot(), context.tbaa_frame_value);
        stack_height++;
    }
}

Value *CompileUnit::getName(int i) {
    PyObject *repr{};
    if constexpr (debug_build) {
        repr = PyTuple_GET_ITEM(py_code->co_names, i);
    }
    auto ptr = getPointer<PyObject *>(code_names, i);
    return loadValue<PyObject *>(ptr, context.tbaa_code_const, useName(repr, "$"));
}

Value *CompileUnit::getFreevar(int i) {
    auto offset = offsetof(PyFrameObject, f_localsplus) + sizeof(PyObject *) * (py_code->co_nlocals + i);
    PyObject *name{};
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


static void debugDumpObj(PyObject *py_code, Module &module, SmallVector<char> *obj_vec = nullptr) {
    if constexpr (debug_build) {
        SmallVector<char> ll_vec{};
        raw_svector_ostream os{ll_vec};
        module.print(os, nullptr);
        callDebugHelperFunction("dump", py_code,
                PyObjectRef{PyMemoryView_FromMemory(ll_vec.data(), ll_vec.size(), PyBUF_READ)},
                PyObjectRef{obj_vec ? PyMemoryView_FromMemory(obj_vec->data(), obj_vec->size(), PyBUF_READ) :
                        Py_NewRef(Py_None)});
        if (!obj_vec && verifyModule(module, &errs())) {
            throw runtime_error("llvm module verification failed");
        }
    }
}

static void notifyCodeLoaded(PyObject *py_code, void *code_addr) {}

CompileUnit::TranslatedResult *CompileUnit::emit(Translator &translator, PyObject *py_code) {
    CompileUnit cu{translator};
    cu.py_code = reinterpret_cast<PyCodeObject *>(py_code);
    cu.llvm_module.setDataLayout(translator.machine->createDataLayout());
    cu.translate();

    debugDumpObj(py_code, cu.llvm_module, nullptr);
    auto &obj = translator.compile(cu.llvm_module);
    auto memory = loadCode(obj);
    debugDumpObj(py_code, cu.llvm_module, &obj);
    obj.resize(0);
    // TODO: cout capcity
    notifyCodeLoaded(py_code, memory.base());

    return new CompileUnit::TranslatedResult{memory, move(cu.vpc_to_stack_height)};
}
