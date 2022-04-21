#ifndef PYNIC_COMPILER
#define PYNIC_COMPILER

#include <llvm/IR/Verifier.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm-c/Target.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/Memory.h>
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

    void *operator()(llvm::Module &mod);

    auto createDataLayout() { return machine->createDataLayout(); }
};

#endif
