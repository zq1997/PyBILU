#include <climits>
#include <cstddef>
#include <memory>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm-c/Target.h>
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
#include <llvm/Object/RelocationResolver.h>

class MyJIT {
public:
    llvm::LLVMContext context{};
    llvm::IRBuilder<> builder{context};
    llvm::Module mod{"", context};

    llvm::ModuleAnalysisManager opt_MAM{};
    llvm::FunctionAnalysisManager opt_FAM{};
    llvm::FunctionPassManager opt_FPM{};

    llvm::legacy::PassManager out_PM;
    llvm::SmallVector<char> out_vec{};
    llvm::raw_svector_ostream out_stream{out_vec};

    llvm::AttributeList attrs{
            llvm::AttributeList::get(
                    context, llvm::AttributeList::FunctionIndex,
                    llvm::AttrBuilder()
                            .addAttribute(llvm::Attribute::NoUnwind)
                            .addAttribute("tune-cpu", llvm::sys::getHostCPUName())
            )
    };

    llvm::Type *type_char{llvm::Type::getIntNTy(context, CHAR_BIT)};
    llvm::Type *type_char_p{type_char->getPointerTo()};
    llvm::Type *type_ptrdiff{llvm::Type::getIntNTy(context, CHAR_BIT * sizeof(std::ptrdiff_t))};

    void compile_function(llvm::Function *func, void *cpy_ir);
public:
    static void init();

    MyJIT();

    void *compile(void *cpy_ir);
};
