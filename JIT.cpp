#include <memory>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <Python.h>

#include "JIT.h"


using namespace std;
using namespace llvm;

static unique_ptr<TargetMachine> machine;

template<typename T>
inline T check(Expected<T> v) {
    if (v) {
        return std::move(*v);
    } else {
        throw runtime_error(toString(v.takeError()));
    }
}

template<typename T>
inline void throwIf(const T &cond, const string &msg) {
    if (cond) {
        throw runtime_error(msg);
    }
}


class {
public:
    const char *file_name{"/tmp/jit"};
    bool output{true};

    inline void dump(const string &ext, const char *data, size_t size) const {
        if (output) {
            ofstream(file_name + ext).write(data, size);
        }
    }

    inline void dump(const string &ext, const Module &mod) const {
        if (output) {
            error_code ec;
            raw_fd_ostream out(file_name + ext, ec);
            mod.print(out, nullptr);
            throwIf(ec, "cannnot write file: " + string(file_name) + ext);
        }
    }
} debug;

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
    pb.registerFunctionAnalyses(opt_FAM);
    opt_FAM.registerPass([&] { return ModuleAnalysisManagerFunctionProxy(opt_MAM); });
    opt_FPM = pb.buildFunctionSimplificationPipeline(
            PassBuilder::OptimizationLevel::O3,
            ThinOrFullLTOPhase::None
    );

    throwIf(machine->addPassesToEmitFile(out_PM, out_stream, nullptr, CodeGenFileType::CGFT_ObjectFile),
            "add emit pass error");
}

void MyJIT::compileFunction(Function *func, PyCodeObject *cpy_ir) {
    assert(PyBytes_CheckExact(cpy_ir->co_code));
    auto translator = parseCFG(cpy_ir);
    translator->jit = this;
    translator->py_obj = type_char_p;
    translator->py_symbol_table = func->getArg(0);;
    translator->py_fast_locals = func->getArg(1);
    for (decltype(+translator->block_num) i = 0; i < translator->block_num; i++) {
        translator->ir_blocks.get()[i] = BasicBlock::Create(context, "", func);
    }
    builder.SetInsertPoint(translator->ir_blocks.get()[0]);
    translator->py_stack = builder.CreateAlloca(type_char_p, builder.getInt32(cpy_ir->co_stacksize));
    for (decltype(+translator->block_num) i = 0; i < translator->block_num; i++) {
        builder.SetInsertPoint(translator->ir_blocks.get()[i]);
        translator->emit_block(i);
    }
}

void *MyJIT::compile(PyCodeObject *cpy_ir) {
    auto func = Function::Create(
            FunctionType::get(type_char_p, {
                    type_char_p,
                    type_char_p->getPointerTo()
            }, false),
            Function::ExternalLinkage,
            "",
            &mod
    );
    func->setAttributes(attrs);
    compileFunction(func, cpy_ir);
    assert(!verifyFunction(*func, &errs()));

    debug.dump(".ll", mod);
    opt_FPM.run(*func, opt_FAM);
    debug.dump(".opt.ll", mod);
    opt_FAM.clear();
    out_vec.clear();
    out_PM.run(mod);
    func->removeFromParent();
    delete func;
    debug.dump(".o", out_vec.data(), out_vec.size());
    MemoryBufferRef buf(StringRef(out_vec.data(), out_vec.size()), "");

    StringRef code{nullptr, 0};
    auto obj = check(object::ObjectFile::createObjectFile(buf));
    for (auto &sec: obj->sections()) {
        assert(sec.relocations().empty());
        if (sec.isText()) {
            assert(!code.data());
            code = check(sec.getContents());
        }
    }

    error_code ec;
    auto flag = sys::Memory::ProtectionFlags::MF_RWE_MASK;
    auto llvm_mem = sys::Memory::allocateMappedMemory(code.size(), nullptr, flag, ec);
    throwIf(ec, ec.message());
    memcpy(llvm_mem.base(), code.data(), code.size());

    return llvm_mem.base();
}
