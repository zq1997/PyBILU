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
    pb.registerCGSCCAnalyses(opt_CGAM);
    pb.registerFunctionAnalyses(opt_FAM);
    pb.registerLoopAnalyses(opt_LAM);
    pb.crossRegisterProxies(opt_LAM, opt_FAM, opt_CGAM, opt_MAM);
    opt_MPM = pb.buildPerModuleDefaultPipeline(OptimizationLevel::O3);

    // pb.registerModuleAnalyses(opt_MAM);
    // pb.registerFunctionAnalyses(opt_FAM);
    // pb.registerLoopAnalyses(opt_LAM);
    // opt_FAM.registerPass([&] { return ModuleAnalysisManagerFunctionProxy(opt_MAM); });
    // opt_FAM.registerPass([&] { return LoopAnalysisManagerFunctionProxy(opt_LAM); });
    // opt_LAM.registerPass([&] { return FunctionAnalysisManagerLoopProxy(opt_FAM); });
    // opt_FPM = pb.buildFunctionSimplificationPipeline(
    //         OptimizationLevel::O3,
    //         ThinOrFullLTOPhase::None
    // );

    throwIf(machine->addPassesToEmitFile(out_PM, out_stream, nullptr, CodeGenFileType::CGFT_ObjectFile),
            "add emit pass error");
}

StringRef Compiler::findPureCode(StringRef obj_file) {
    StringRef code;
    auto obj = check(object::ObjectFile::createObjectFile(MemoryBufferRef(obj_file, "")));
    for (auto &sec : obj->sections()) {
        if (sec.isText()) {
            assert(sec.relocations().empty());
            assert(!code.data());
            code = check(sec.getContents());
            assert(!code.empty());
        }
    }
    return code;
}

// StringRef Compiler::compile(llvm::Module &mod) {
//     debug.dump(".ll", mod);
//     assert(!verifyModule(mod, &errs()));
//     opt_MPM.run(mod, opt_MAM);
//     opt_MAM.clear();
//     debug.dump(".opt.ll", mod);
//
//     out_PM.run(mod);
//     debug.dump(".o", out_vec.data(), out_vec.size());
//
//     StringRef code{};
//     StringRef obj_file(out_vec.data(), out_vec.size());
//     auto obj = check(object::ObjectFile::createObjectFile(MemoryBufferRef(obj_file, "")));
//     for (auto &sec : obj->sections()) {
//         if (sec.isText()) {
//             assert(sec.relocations().empty());
//             assert(!code.data());
//             code = check(sec.getContents());
//             assert(!code.empty());
//         }
//     }
//     return code;
// }
