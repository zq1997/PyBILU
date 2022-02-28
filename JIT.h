#include <memory>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm-c/Target.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/Memory.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/ADT/StringMap.h>

#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/InstSimplifyPass.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/TailRecursionElimination.h>

class MyJIT {
private:
    llvm::LLVMContext context{};
    llvm::IRBuilder<> builder{context};
    llvm::Module mod{"", context};

    llvm::LoopAnalysisManager opt_lam{};
    llvm::CGSCCAnalysisManager opt_cgam{};
    llvm::ModuleAnalysisManager opt_mam{};
    llvm::FunctionAnalysisManager opt_fam{};
    llvm::FunctionPassManager opt_fpm{};

    llvm::legacy::PassManager out_pm;
    llvm::SmallVector<char> out_vec{};
    llvm::raw_svector_ostream out_stream{out_vec};

    llvm::AttributeList attrs{
            llvm::AttributeList::get(context, llvm::AttributeList::FunctionIndex, {llvm::Attribute::NoUnwind})
    };

    llvm::Type *t_PyType_Object{};
    llvm::Type *t_PyObject{};
    llvm::Type *t_PyObject_p{};

    llvm::Function *f_PyNumber_Add{};
    llvm::Function *f_PyLong_FromLong{};
public:
    static void init();

    MyJIT();

    void *to_machine_code(void *cpy_ir);
};
