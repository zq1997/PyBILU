#include <memory>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/Memory.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/ADT/StringMap.h>

class MyJIT {
public:
    static std::unique_ptr<llvm::StringMap<void *>> addrMap;
    static std::unique_ptr<llvm::TargetMachine> machine;

    // std::unique_ptr<llvm::TargetMachine> machine{};
    llvm::LLVMContext context{};
    llvm::IRBuilder<> builder{context};
    llvm::Module mod{"", context};
    llvm::Type *t_PyType_Object{};
    llvm::Type *t_PyObject{};
    llvm::Type *t_PyObject_p{};

    MyJIT();
    ~MyJIT() = default;

public:
    static void init();

    void *to_machine_code(void *cpy_ir);
};
