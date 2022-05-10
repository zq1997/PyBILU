#ifndef PYNIC_COMPILER
#define PYNIC_COMPILER

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm-c/Target.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Host.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/MC/SubtargetFeature.h>

class Compiler {
    std::unique_ptr<llvm::TargetMachine> machine{};

    llvm::ModuleAnalysisManager opt_MAM{};
    llvm::CGSCCAnalysisManager opt_CGAM{};
    llvm::FunctionAnalysisManager opt_FAM{};
    llvm::LoopAnalysisManager opt_LAM{};
    // llvm::FunctionPassManager opt_FPM{};
    llvm::ModulePassManager opt_MPM{};

    llvm::legacy::PassManager out_PM{};
    llvm::SmallVector<char> out_vec{};
    llvm::raw_svector_ostream out_stream{out_vec};

public:
    Compiler();

    void optimize(llvm::Module &mod) {
        opt_MPM.run(mod, opt_MAM);
        opt_MAM.clear();
    }

    auto compile(llvm::Module &mod) {
        out_PM.run(mod);
        return llvm::StringRef{out_vec.data(), out_vec.size()};
    }

    llvm::StringRef findPureCode(llvm::StringRef obj_file);

    void clean() { out_vec.clear(); }

    decltype(auto) createDataLayout() { return machine->createDataLayout(); }
};

#endif
