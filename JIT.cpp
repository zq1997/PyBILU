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
} extern const call_table{};

template<typename T>
T check(Expected<T> v) {
    if (v) {
        return std::move(*v);
    } else {
        throw runtime_error(toString(v.takeError()));
    }
}

inline void throwIf(bool cond, const string &msg) {
    if (cond) {
        throw runtime_error(msg);
    }
}

void MyJIT::init() {
    throwIf(LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter(), "initialization failed");

    auto triple = sys::getProcessTriple();
    SubtargetFeatures features;
    StringMap<bool> feature_map;
    sys::getHostCPUFeatures(feature_map);
    for (auto &f: feature_map) {
        features.AddFeature(f.first(), f.second);
    }

    std::string err;
    auto *target = TargetRegistry::lookupTarget(triple, err);
    throwIf(!target, err);
    machine = unique_ptr<TargetMachine>(target->createTargetMachine(
            triple,
            sys::getHostCPUName(),
            features.getString(),
            TargetOptions(),
            Reloc::Model::PIC_,
            CodeModel::Model::Small,
            CodeGenOpt::Aggressive
    ));
    throwIf(!machine, "cannot create TargetMachine");
}

MyJIT::MyJIT() {
    mod.setTargetTriple(machine->getTargetTriple().getTriple());
    mod.setDataLayout(machine->createDataLayout());

    PassBuilder pb{machine.get()};
    pb.registerModuleAnalyses(opt_MAM);
    pb.registerCGSCCAnalyses(opt_CGAM);
    pb.registerFunctionAnalyses(opt_FAM);
    pb.registerLoopAnalyses(opt_LAM);
    pb.crossRegisterProxies(opt_LAM, opt_FAM, opt_CGAM, opt_MAM);
    opt_FPM = pb.buildFunctionSimplificationPipeline(
            PassBuilder::OptimizationLevel::O3,
            ThinOrFullLTOPhase::None
    );

    throwIf(machine->addPassesToEmitFile(out_PM, out_stream, nullptr, CodeGenFileType::CGFT_ObjectFile),
            "add emit pass error");
}

CallInst *createCall(MyJIT &jit, Value *table, void *const &func, Type *result,
        ArrayRef<Type *> arg_types, initializer_list<Value *> args) {
    auto bf_type = FunctionType::get(result, arg_types, false);
    auto offset = reinterpret_cast<const char *>(&func) - reinterpret_cast<const char *>(&call_table);
    auto entry = jit.builder.CreateInBoundsGEP(jit.ctype_char, table,
            ConstantInt::get(jit.ctype_ptrdiff, offset));
    entry = jit.builder.CreatePointerCast(entry, bf_type->getPointerTo()->getPointerTo());
    entry = jit.builder.CreateLoad(bf_type->getPointerTo(), entry);
    return jit.builder.CreateCall(bf_type, entry, args);
}

void *MyJIT::compile(void *cpy_ir) {
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
    Type *arg_types[]{t_PyObject_p, t_PyObject_p};
    auto ll = createCall(*this, table, call_table.PyNumber_Multiply, t_PyObject_p, arg_types, {l, l});
    auto rr = createCall(*this, table, call_table.PyNumber_Multiply, t_PyObject_p, arg_types, {r, r});
    auto ret = createCall(*this, table, call_table.PyNumber_Add, t_PyObject_p, arg_types, {ll, rr});

    builder.CreateRet(ret);
    assert(!verifyFunction(*func, &errs()));

    opt_FPM.run(*func, opt_FAM);
    // mod.print(llvm::outs(), nullptr);
    out_vec.clear();
    out_PM.run(mod);
    func->removeFromParent();
    delete func;
    ofstream("/tmp/jit.o").write(out_vec.data(), out_vec.size());
    MemoryBufferRef buf(StringRef(out_vec.data(), out_vec.size()), "");

    auto obj = check(object::ObjectFile::createObjectFile(buf));
    auto sec = obj->section_end();
    for (auto &s: obj->sections()) {
        assert(s.relocations().empty());
        if (s.isText()) {
            assert(sec == obj->section_end());
            sec = s;
        }
    }
    assert(sec != obj->section_end());
    assert(sec->isText());
    auto sec_content = check(sec->getContents());

    error_code ec;
    auto flag = sys::Memory::ProtectionFlags::MF_RWE_MASK;
    auto llvm_mem = sys::Memory::allocateMappedMemory(sec_content.size(), nullptr, flag, ec);
    throwIf(bool(ec), ec.message());
    memcpy(llvm_mem.base(), sec_content.data(), sec_content.size());

    return llvm_mem.base();
}
