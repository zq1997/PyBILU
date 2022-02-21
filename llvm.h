#include <memory>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/CompileUtils.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>

class MyJIT {
private:
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

    static void Init() {
        LLVMInitializeNativeTarget();
        LLVMInitializeNativeAsmPrinter();
    }

    static llvm::Expected<std::unique_ptr<MyJIT>> Create();

    void addModule(std::unique_ptr<llvm::Module> mod, std::unique_ptr<llvm::LLVMContext> ctx) {
        llvm::orc::ThreadSafeModule tmod(std::move(mod), std::move(ctx));
        cantFail(compile_layer.add(dylib, std::move(tmod)));
    }

    llvm::Expected<llvm::JITEvaluatedSymbol> lookup(llvm::StringRef Name) {
        return sess.lookup(&dylib, Name);
    }
};
