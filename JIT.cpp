#include <memory>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <Python.h>

#include "JIT.h"


using namespace std;
using namespace llvm;

static unique_ptr<TargetMachine> machine;

struct CallTable {
    void *PyNumber_Add{reinterpret_cast<void *>(::PyNumber_Add)};
    void *PyNumber_Multiply{reinterpret_cast<void *>(::PyNumber_Multiply)};
    void *PyLong_FromLong{reinterpret_cast<void *>(::PyLong_FromLong)};
} extern const callTable{};

template<typename T>
T check(Expected<T> v) {
    if (v) {
        return std::move(*v);
    } else {
        throw runtime_error(toString(v.takeError()));
    }
}

void MyJIT::init() {
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) {
        throw runtime_error("err");
    }

    TargetOptions options;
    auto triple = sys::getProcessTriple();
    SubtargetFeatures features;
    StringMap<bool> feature_map;
    sys::getHostCPUFeatures(feature_map);
    for (auto &f: feature_map) {
        features.AddFeature(f.first(), f.second);
    }

    std::string err;
    auto *target = TargetRegistry::lookupTarget(triple, err);
    if (!target) {
        throw runtime_error(err);
    }
    machine = unique_ptr<TargetMachine>(target->createTargetMachine(
            triple,
            sys::getHostCPUName(),
            features.getString(),
            options,
            Reloc::Model::PIC_,
            CodeModel::Model::Small,
            CodeGenOpt::Aggressive
    ));
    if (!machine) {
        throw runtime_error("");
    }
}

MyJIT::MyJIT() {
    mod.setTargetTriple(machine->getTargetTriple().getTriple());
    mod.setDataLayout(machine->createDataLayout());

    PassBuilder pb{machine.get()};
    pb.registerModuleAnalyses(opt_mam);
    pb.registerCGSCCAnalyses(opt_cgam);
    pb.registerFunctionAnalyses(opt_fam);
    pb.registerLoopAnalyses(opt_lam);
    pb.crossRegisterProxies(opt_lam, opt_fam, opt_cgam, opt_mam);
    opt_fpm = pb.buildFunctionSimplificationPipeline(
            PassBuilder::OptimizationLevel::O3,
            ThinOrFullLTOPhase::None
    );

    if (machine->addPassesToEmitFile(out_pm, out_stream, nullptr, CodeGenFileType::CGFT_ObjectFile)) {
        throw runtime_error("add emit pass error");
    }
}

void *emitModule(MemoryBufferRef &buf) {
    auto obj = check(object::ObjectFile::createObjectFile(buf));

    auto text_sec = obj->section_end();
    for (auto &sec: obj->sections()) {
        assert(sec.relocations().empty());
        if (sec.isText()) {
            assert(text_sec == obj->section_end());
            text_sec = sec;
        }
    }
    assert(text_sec != obj->section_end());

    auto sec_content = text_sec->getContents();
    if (!sec_content) {
        throw runtime_error("bad section");
    }
    auto code = const_cast<char *>(sec_content->data());
    auto code_size = sec_content->size();

    error_code ec;
    auto flag = sys::Memory::ProtectionFlags::MF_RWE_MASK;
    auto llvm_mem = sys::Memory::allocateMappedMemory(code_size, nullptr, flag, ec);
    if (ec) {
        throw runtime_error("allocation error");
    }
    memcpy(llvm_mem.base(), code, code_size);
    return llvm_mem.base();
}

CallInst *createCall(MyJIT &jit, Value *table, void *const &func, Type *result, initializer_list<Value *> args) {
    Type *arg_types[args.size()];
    Type **type = arg_types;
    for (auto &arg: args) {
        *type++ = arg->getType();
    }
    ArrayRef<Type *> x{arg_types, args.size()};
    auto bf_type = FunctionType::get(result, x, false);
    auto offset = reinterpret_cast<const char *>(&func) - reinterpret_cast<const char *>(&callTable);
    auto entry = jit.builder.CreateInBoundsGEP(jit.ctype_char, table,
            ConstantInt::get(jit.ctype_ptrdiff, offset));
    entry = jit.builder.CreatePointerCast(entry, bf_type->getPointerTo()->getPointerTo());
    entry = jit.builder.CreateLoad(bf_type->getPointerTo(), entry);
    return jit.builder.CreateCall(bf_type, entry, args);
}

void *MyJIT::to_machine_code(void *cpy_ir) {
    // assert(PyBytes_CheckExact(cpy_ir->co_code));
    // auto instr_arr = PyBytes_AS_STRING(cpy_ir->co_code);
    // auto instr_size = PyBytes_GET_SIZE(cpy_ir->co_code);
    // auto instr_begin = reinterpret_cast<_Py_CODEUNIT *>(instr_arr);
    // auto instr_end = reinterpret_cast<_Py_CODEUNIT *>(instr_arr + instr_size);
    // for (auto iter = instr_begin; iter < instr_end; iter++) {
    //     auto opcode = _Py_OPCODE(*iter);
    //     auto oparg = _Py_OPARG(*iter);
    // }

    auto func = Function::Create(
            FunctionType::get(t_PyObject_p, {
                    ctype_char_ptr,
                    t_PyObject_p->getPointerTo()
            }, false),
            Function::ExternalLinkage,
            "",
            &mod
    );
    func->setAttributes(attrs);

    BasicBlock *bb = BasicBlock::Create(context, "", func);
    builder.SetInsertPoint(bb);

    auto table = func->getArg(0);;
    auto args = func->getArg(1);
    auto l = builder.CreateLoad(t_PyObject_p, builder.CreateConstInBoundsGEP1_32(t_PyObject_p, args, 0));
    auto r = builder.CreateLoad(t_PyObject_p, builder.CreateConstInBoundsGEP1_32(t_PyObject_p, args, 1));
    auto ll = createCall(*this, table, callTable.PyNumber_Multiply, t_PyObject_p, {l, l});
    auto rr = createCall(*this, table, callTable.PyNumber_Multiply, t_PyObject_p, {r, r});
    auto ret = createCall(*this, table, callTable.PyNumber_Add, t_PyObject_p, {ll, rr});

    builder.CreateRet(ret);
    assert(!verifyFunction(*func, &errs()));

    opt_fpm.run(*func, opt_fam);
    // mod.print(llvm::outs(), nullptr);
    out_vec.clear();
    out_pm.run(mod);
    ofstream("/tmp/jit.o").write(out_vec.data(), out_vec.size());
    MemoryBufferRef buf(StringRef(out_vec.data(), out_vec.size()), "");
    auto addr = emitModule(buf);
    func->removeFromParent();
    delete func;
    return addr;
}
