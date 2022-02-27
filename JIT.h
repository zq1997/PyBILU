#include <memory>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm-c/Target.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/Memory.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/ADT/StringMap.h>

class MyJIT {
private:
    llvm::LLVMContext context{};
    llvm::IRBuilder<> builder{context};
    llvm::Module mod{"", context};
    llvm::AttributeList attrs{
            llvm::AttributeList::get(context, llvm::AttributeList::FunctionIndex, {llvm::Attribute::NoUnwind})
    };
    llvm::Type *t_PyType_Object{};
    llvm::Type *t_PyObject{};
    llvm::Type *t_PyObject_p{};

public:
    static void init();

    MyJIT();

    void *to_machine_code(void *cpy_ir);
};
