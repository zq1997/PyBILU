#ifndef PYNIC_TRANSLATOR
#define PYNIC_TRANSLATOR

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

llvm::sys::MemoryBlock loadCode(llvm::SmallVector<char> &obj_vec);
void unloadCode(llvm::sys::MemoryBlock &mem);

class Compiler {
public:
    std::unique_ptr<llvm::TargetMachine> machine;

    llvm::ModuleAnalysisManager opt_MAM;
    llvm::CGSCCAnalysisManager opt_CGAM;
    llvm::FunctionAnalysisManager opt_FAM;
    llvm::LoopAnalysisManager opt_LAM;
    llvm::ModulePassManager opt_MPM;

    llvm::legacy::PassManager out_PM;
    llvm::SmallVector<char> out_vec;
    llvm::raw_svector_ostream out_stream{out_vec};

public:
    Compiler();

    auto &compile(llvm::Module &module) {
        opt_MPM.run(module, opt_MAM);
        opt_MAM.clear();
        out_PM.run(module);
        return out_vec;
    }
};

class Context {
public:
    llvm::LLVMContext llvm_context;
    RegisteredTypes::type registered_types;
    llvm::Constant *c_null;
    llvm::MDNode *likely_true;
    llvm::MDNode *tbaa_refcnt;
    llvm::MDNode *tbaa_obj_field;
    llvm::MDNode *tbaa_frame_value;
    llvm::MDNode *tbaa_code_const;
    llvm::MDNode *tbaa_symbols;
    llvm::AttributeList attr_refcnt_call;
    llvm::AttributeList attr_noreturn;
    llvm::AttributeList attr_default_call;

    explicit Context(const llvm::DataLayout &dl);

    template <typename T>
    auto type() const { return std::get<NormalizedLLVMType<T>>(registered_types).type; }

    template <typename T>
    auto align() const { return std::get<NormalizedLLVMType<T>>(registered_types).align; }
};

class Translator : public Compiler, public Context {
public:
    Translator() : Compiler{}, Context{machine->createDataLayout()} {}
};

#endif
