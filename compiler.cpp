#include <fstream>

#include "compiler.h"
#include "general_utilities.h"

using namespace std;
using namespace llvm;

template <typename T>
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

template <typename T>
inline T check(Expected<T> v) {
    if (v) {
        return std::move(*v);
    } else {
        throw runtime_error(toString(v.takeError()));
    }
}

Compiler::Compiler() {
    throwIf(LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter(), "initialization failed");

    auto triple = sys::getProcessTriple();
    SubtargetFeatures features;
    StringMap<bool> feature_map;
    sys::getHostCPUFeatures(feature_map);
    for (auto &f : feature_map) {
        features.AddFeature(f.first(), f.second);
    }

    string err;
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


    PassBuilder pb{machine.get()};
    pb.registerModuleAnalyses(opt_MAM);
    pb.registerFunctionAnalyses(opt_FAM);
    pb.registerLoopAnalyses(opt_LAM);
    opt_FAM.registerPass([&] { return ModuleAnalysisManagerFunctionProxy(opt_MAM); });
    opt_FAM.registerPass([&] { return LoopAnalysisManagerFunctionProxy(opt_LAM); });
    opt_LAM.registerPass([&] { return FunctionAnalysisManagerLoopProxy(opt_FAM); });
    opt_FPM = pb.buildFunctionSimplificationPipeline(
            OptimizationLevel::O3,
            ThinOrFullLTOPhase::None
    );

    throwIf(machine->addPassesToEmitFile(out_PM, out_stream, nullptr, CodeGenFileType::CGFT_ObjectFile),
            "add emit pass error");
}

void *Compiler::operator()(llvm::Module &mod) {
    debug.dump(".ll", mod);
    for (auto &func : mod) {
        opt_FPM.run(func, opt_FAM);
    }
    opt_LAM.clear();
    opt_FAM.clear();
    debug.dump(".opt.ll", mod);

    mod.setDataLayout(machine->createDataLayout());
    out_PM.run(mod);
    debug.dump(".o", out_vec.data(), out_vec.size());

    StringRef code{};
    auto obj = check(object::ObjectFile::createObjectFile(
            MemoryBufferRef(StringRef(out_vec.data(), out_vec.size()), "")));
    for (auto &sec : obj->sections()) {
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

    out_vec.clear();
    return llvm_mem.base();
}
