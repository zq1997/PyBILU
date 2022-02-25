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
private:
    static std::unique_ptr<llvm::StringMap<void *>> addr_map;

    std::unique_ptr<llvm::TargetMachine> machine;

    explicit MyJIT(std::unique_ptr<llvm::TargetMachine> machine_) : machine(move(machine_)) {}

public:
    static void Init();

    static llvm::Expected<std::unique_ptr<MyJIT>> Create();

    void *emitModule(llvm::Module &mod);

    llvm::DataLayout getDL() {
        return machine->createDataLayout();
    }
};
