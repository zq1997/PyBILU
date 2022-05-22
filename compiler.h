#ifndef PYNIC_COMPILER
#define PYNIC_COMPILER

#include <llvm/IR/Verifier.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm-c/Target.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Memory.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/Host.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/MC/SubtargetFeature.h>

#include "shared_symbols.h"
#include "general_utilities.h"

#ifdef NDEBUG
constexpr auto debug_build = false;
#else
constexpr auto debug_build = true;
#endif

llvm::sys::MemoryBlock loadCode(llvm::SmallVector<char> &obj_vec);
void unloadCode(llvm::sys::MemoryBlock &mem);

class Compiler {
    std::unique_ptr<llvm::TargetMachine> machine{};

    llvm::ModuleAnalysisManager opt_MAM{};
    llvm::CGSCCAnalysisManager opt_CGAM{};
    llvm::FunctionAnalysisManager opt_FAM{};
    llvm::LoopAnalysisManager opt_LAM{};
    llvm::ModulePassManager opt_MPM{};

    llvm::legacy::PassManager out_PM{};
    llvm::SmallVector<char> out_vec{};
    llvm::raw_svector_ostream out_stream{out_vec};

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
            return loadCode(out_vec);
        }
    }

    decltype(auto) createDataLayout() { return machine->createDataLayout(); }
};

class WrappedContext {
    friend class WrappedModule;
    friend class Translator;

    llvm::LLVMContext context;
    RegisteredTypes::type registered_types;
    llvm::Constant *c_null;
    llvm::MDNode *likely_true;
    llvm::MDNode *tbaa_refcnt;
    llvm::MDNode *tbaa_obj_field;
    llvm::MDNode *tbaa_frame_value;
    llvm::MDNode *tbaa_code_const;
    llvm::AttributeList attr_return;
    llvm::AttributeList attr_noreturn;
    llvm::AttributeList attr_default_call;

public:
    explicit WrappedContext(const llvm::DataLayout &dl);
    operator llvm::LLVMContext &() { return context; }

    template <typename T>
    auto type() const { return std::get<NormalizedLLVMType<T>>(registered_types).type; }

    template <typename T>
    auto align() const { return std::get<NormalizedLLVMType<T>>(registered_types).align; }
};


#endif
