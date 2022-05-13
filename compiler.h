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

#include "memory_manager.h"


#ifdef NDEBUG
constexpr auto debug_build = false;
#else
constexpr auto debug_build = true;
#endif

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

    llvm::sys::MemoryBlock loadCode();
    llvm::sys::MemoryBlock compileForDebug(PyCodeObject *py_code, llvm::Module &mod);
public:
    Compiler();

    llvm::sys::MemoryBlock compile(PyCodeObject *py_code, llvm::Module &mod) {
        if constexpr (debug_build) {
            return compileForDebug(py_code, mod);
        } else {
            opt_MPM.run(mod, opt_MAM);
            opt_MAM.clear();
            out_PM.run(mod);
            return loadCode();
        }
    }

    decltype(auto) createDataLayout() { return machine->createDataLayout(); }
};

#endif
