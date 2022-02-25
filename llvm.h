#include <memory>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/Memory.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/ADT/StringMap.h>

class MyJIT {
private:
    static std::unique_ptr<llvm::StringMap<void *>> addr_map;
    static const llvm::StringMap<void *> &addr_map2;

    std::unique_ptr<llvm::TargetMachine> machine;
    llvm::orc::ExecutionSession sess;
    llvm::orc::RTDyldObjectLinkingLayer object_layer;
    llvm::orc::IRCompileLayer compile_layer;
    llvm::orc::JITDylib &dylib;

    MyJIT(std::unique_ptr<llvm::TargetMachine> machine,
          std::unique_ptr<llvm::orc::SelfExecutorProcessControl> epc);

public:
    ~MyJIT() {
        cantFail(sess.endSession());
    }

    static void Init();

    static llvm::Expected<std::unique_ptr<MyJIT>> Create();

    void *emitModule(llvm::Module &mod);

    llvm::DataLayout getDL() {
        return machine->createDataLayout();
    }

    llvm::Expected<llvm::JITEvaluatedSymbol> lookup(llvm::StringRef Name) {
        return sess.lookup(&dylib, Name);
    }
};
