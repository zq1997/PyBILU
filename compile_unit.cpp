#include <memory>

#include <Python.h>

#include "compile_unit.h"


using namespace std;
using namespace llvm;


void CompileUnit::translate() {
    function = Function::Create(context.type<CompiledFunction>(),
            Function::ExternalLinkage, "the_function", &llvm_module);
    function->setAttributes(context.attr_default_call);

    (shared_symbols = function->getArg(0))->setName(useName("symbols"));
    (frame_obj = function->getArg(1))->setName(useName("frame"));
    // TODO: 下面的，不知是否有必要
    shared_symbols->addAttr(Attribute::NoAlias);
    frame_obj->addAttr(Attribute::NoAlias);

    DebugInfoBuilder di_builder{py_code, builder, llvm_module, *function};

    // TODO: 重复了
    parseCFG();

    blocks[0].block = createBlock("entry_block", function);
    auto start = blocks[0].end;
    for (auto &b : Range(block_num - 1, &blocks[1])) {
        b.initial_stack_height = -1;
        b.is_handler = false;
        b.block = createBlock(useName("block.", start), nullptr);
        start = b.end;
    }
    blocks[1].initial_stack_height = 0;

    builder.SetInsertPoint(blocks[0].block);

    auto code_obj = loadFieldValue(frame_obj, &PyFrameObject::f_code, context.tbaa_frame_value, useName("code"));
    auto names_tuple = loadFieldValue(code_obj, &PyCodeObject::co_names, context.tbaa_frame_value);
    code_names = getPointer(names_tuple, &PyTupleObject::ob_item, "names");
    auto consts_tuple = loadFieldValue(code_obj, &PyCodeObject::co_consts, context.tbaa_code_const);
    code_consts = getPointer(consts_tuple, &PyTupleObject::ob_item, "consts");
    rt_lasti = getPointer(frame_obj, &PyFrameObject::f_lasti, "lasti");

    error_block = createBlock("raise_error", nullptr);

    for (auto &i : Range(block_num - 1, 1U)) {
        // 队列(栈)式生成
        emitBlock(di_builder, i);
    }

    error_block->insertInto(function);
    builder.SetInsertPoint(error_block);
    callSymbol<raiseException, &Context::attr_noreturn>();
    builder.CreateUnreachable();

    // TODO: debug location有时候去除
    builder.SetInsertPoint(blocks[0].block);
    auto indirect_br = builder.CreateIndirectBr(
            builder.CreateInBoundsGEP(context.type<char>(), BlockAddress::get(blocks[1].block),
                    function->getArg(2)));
    blocks[1].is_handler = true;
    for (auto &b : Range(block_num - 1, &blocks[1])) {
        if (b.is_handler) {
            indirect_br->addDestination(b.block);
        }
    }

    di_builder.finalize();

    if constexpr (debug_build) {
        if (verifyModule(llvm_module, &errs())) {
            llvm_module.dump();
            throw runtime_error("the translater failed");
        }
    }
}

void CompileUnit::parseCFG() {
    auto size = PyBytes_GET_SIZE(py_code->co_code) / sizeof(PyInstr);
    vpc_to_stack_depth.reserve(size);

    BitArray is_boundary(size + 1);
    block_num = 0;

    auto py_instructions = reinterpret_cast<PyInstr *>(PyBytes_AS_STRING(py_code->co_code));
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


void CompileUnit::do_Py_INCREF(Value *py_obj) {
    // callSymbol<handle_INCREF, &Translator::attr_return>(py_obj);
    auto ref = getPointer(py_obj, &PyObject::ob_refcnt);
    auto old_value = loadValue<decltype(PyObject::ob_refcnt)>(ref, context.tbaa_refcnt);
    auto *delta_1 = asValue(decltype(PyObject::ob_refcnt){1});
    auto new_value = builder.CreateAdd(old_value, delta_1);
    builder.CreateAssumption(builder.CreateICmpSGT(new_value, delta_1)); // 需要么
    storeValue<decltype(PyObject::ob_refcnt)>(new_value, ref, context.tbaa_refcnt);
}

void CompileUnit::do_Py_DECREF(Value *py_obj) {
    // callSymbol<handle_DECREF, &Translator::attr_return>(py_obj);
    auto ref = getPointer(py_obj, &PyObject::ob_refcnt);
    auto old_value = loadValue<decltype(PyObject::ob_refcnt)>(ref, context.tbaa_refcnt);
    auto *delta_1 = asValue(decltype(PyObject::ob_refcnt){1});
    auto *zero = asValue(decltype(PyObject::ob_refcnt){0});
    auto new_value = builder.CreateSub(old_value, delta_1);
    storeValue<decltype(PyObject::ob_refcnt)>(new_value, ref, context.tbaa_refcnt);
    auto b_dealloc = createBlock("dealloc");
    auto b_end = createBlock("dealloc.end");
    builder.CreateCondBr(builder.CreateICmpEQ(new_value, zero), b_dealloc, b_end, context.likely_true);
    builder.SetInsertPoint(b_dealloc);
    callSymbol<_Py_Dealloc, &Context::attr_return>(py_obj);
    builder.CreateBr(b_end);
    builder.SetInsertPoint(b_end);
}

void CompileUnit::do_Py_XDECREF(Value *py_obj) {
    // callSymbol<handle_XDECREF, &Translator::attr_return>(py_obj);
    auto b_decref = createBlock("decref");
    auto b_end = createBlock("decref.end");
    builder.CreateCondBr(builder.CreateICmpNE(py_obj, context.c_null), b_decref, b_end, context.likely_true);
    builder.SetInsertPoint(b_decref);
    do_Py_DECREF(py_obj);
    builder.CreateBr(b_end);
    builder.SetInsertPoint(b_end);
}

Value *CompileUnit::do_GETLOCAL(PyOparg oparg) {
    auto varname = PyTuple_GET_ITEM(py_code->co_varnames, oparg);
    auto offset = offsetof(PyFrameObject, f_localsplus) + sizeof(PyObject *) * oparg;
    auto slot = getPointer<char>(frame_obj, offset, useName("local.", varname, "."));
    return loadValue<PyObject *>(slot, context.tbaa_frame_value, useName(varname));
}

void CompileUnit::do_SETLOCAL(PyOparg oparg, llvm::Value *value) {
    auto varname = PyTuple_GET_ITEM(py_code->co_varnames, oparg);
    auto offset = offsetof(PyFrameObject, f_localsplus) + sizeof(PyObject *) * oparg;
    auto slot = getPointer<char>(frame_obj, offset, useName("local.", varname, "."));
    auto old_value = loadValue<PyObject *>(slot, context.tbaa_frame_value, useName(varname, ".old", "."));
    storeValue<PyObject *>(value, slot, context.tbaa_frame_value);
    do_Py_XDECREF(old_value);
}

Value *CompileUnit::getStackSlot(int i) {
    assert(stack_depth >= i);
    auto offset = offsetof(PyFrameObject, f_localsplus) + sizeof(PyObject *) * (stack_depth - i +
            py_code->co_nlocals +
            PyTuple_GET_SIZE(py_code->co_cellvars) +
            PyTuple_GET_SIZE(py_code->co_freevars)
    );
    return getPointer<char>(frame_obj, offset, useName("stack.", i, "."));
}

Value *CompileUnit::do_PEAK(int i) {
    return loadValue<PyObject *>(getStackSlot(i), context.tbaa_frame_value);
}

void CompileUnit::do_SET_PEAK(int i, llvm::Value *value) {
    storeValue<PyObject *>(value, getStackSlot(i), context.tbaa_frame_value);
}

llvm::Value *CompileUnit::do_POP() {
    --stack_depth;
    return do_PEAK(0);
}

void CompileUnit::do_PUSH(llvm::Value *value) {
    do_SET_PEAK(0, value);
    stack_depth++;
}

Value *CompileUnit::getName(int i) {
    PyObject *repr{};
    if constexpr(debug_build) {
        repr = PyTuple_GET_ITEM(py_code->co_names, i);
    }
    auto ptr = getPointer<PyObject *>(code_names, i);
    return loadValue<PyObject *>(ptr, context.tbaa_code_const, useName(repr, "$"));
}

Value *CompileUnit::getFreevar(int i) {
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
    // TODO: tbaa_frame_value还是const
    return loadValue<PyObject *>(slot, context.tbaa_frame_value, useName(name));
}

llvm::Value *CompileUnit::getSymbol(size_t offset) {
    const char *name = nullptr;
    if constexpr (debug_build) {
        name = symbol_names[offset];
    }
    auto ptr = getPointer<void *>(shared_symbols, offset, useName("sym.", offset, "."));
    return loadValue<void *>(ptr, context.tbaa_code_const, useName("sym.", name, "."));
}

PyBasicBlock &CompileUnit::findPyBlock(unsigned offset) {
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

void CompileUnit::pyJumpIF(unsigned offset, bool pop_if_jump, bool jump_cond) {
    auto &jump_target = findPyBlock(offset);
    jump_target.initial_stack_height = stack_depth - pop_if_jump;
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
    builder.CreateCondBr(callSymbol<castPyObjectToBool>(obj), true_block, false_block);
    if (pop_if_jump) {
        builder.SetInsertPoint(jump_block);
        do_Py_DECREF(obj);
        builder.CreateBr(jump_target.block);
    }
    builder.SetInsertPoint(fall_block);
    do_Py_DECREF(obj);
    stack_depth--;
}


static void debug_dump_obj(PyObject *py_code, Module &module, SmallVector<char> *obj_vec = nullptr) {
    if constexpr(debug_build) {
        SmallVector<char> ll_vec{};
        raw_svector_ostream os{ll_vec};
        module.print(os, nullptr);
        callDebugHelperFunction("dump", py_code,
                PyObjectRef{PyMemoryView_FromMemory(ll_vec.data(), ll_vec.size(), PyBUF_READ)},
                PyObjectRef{obj_vec ? PyMemoryView_FromMemory(obj_vec->data(), obj_vec->size(), PyBUF_READ) :
                        Py_NewRef(Py_None)});

    }
}

static void notifyCodeLoaded(PyObject *py_code, void *code_addr) {}

CompileUnit::TranslatedResult *CompileUnit::emit(Translator &translator, PyObject *py_code) {
    CompileUnit cu{translator};
    cu.py_code = reinterpret_cast<PyCodeObject *>(py_code);
    cu.llvm_module.setDataLayout(translator.machine->createDataLayout());
    cu.translate();

    debug_dump_obj(py_code, cu.llvm_module, nullptr);
    translator.opt_MPM.run(cu.llvm_module, translator.opt_MAM);
    translator.opt_MAM.clear();
    translator.out_PM.run(cu.llvm_module);
    auto memory = loadCode(translator.out_vec);
    debug_dump_obj(py_code, cu.llvm_module, &translator.out_vec);
    translator.out_vec.resize(0);
    notifyCodeLoaded(py_code, memory.base());

    return new CompileUnit::TranslatedResult{memory, move(cu.vpc_to_stack_depth)};
}
