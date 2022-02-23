#include <memory>
#include <iostream>
#include <fstream>

#include "llvm.h"

using namespace std;
using namespace llvm;
using namespace llvm::orc;


MyJIT::MyJIT(unique_ptr<TargetMachine> machine_,
             unique_ptr<SelfExecutorProcessControl> epc) :
        machine(move(machine_)),
        sess(move(epc)),
        object_layer(sess, make_unique<SectionMemoryManager>),
        compile_layer(sess, object_layer, make_unique<SimpleCompiler>(*machine)),
        dylib(sess.createBareJITDylib("")) {}

llvm::Expected<unique_ptr<MyJIT>> MyJIT::Create() {
    auto epc = SelfExecutorProcessControl::Create();
    if (!epc) {
        return epc.takeError();
    }
    auto tm_builder = JITTargetMachineBuilder::detectHost();
    if (!tm_builder) {
        return tm_builder.takeError();
    }
    auto tm = tm_builder->createTargetMachine();
    if (!tm) {
        return tm.takeError();
    }
    char prefix = (*tm)->createDataLayout().getGlobalPrefix();
    auto jit = unique_ptr<MyJIT>(new MyJIT(move(*tm), move(*epc)));
    auto gen = DynamicLibrarySearchGenerator::GetForCurrentProcess(prefix);
    if (!gen) {
        return gen.takeError();
    }
    jit->dylib.addGenerator(move(*gen));
    return jit;
}

void MyJIT::emitModule(llvm::Module &mod) {
    auto &compiler = static_cast<llvm::orc::SimpleCompiler &>(compile_layer.getCompiler());
    auto buffer = llvm::cantFail(compiler(mod));
    auto obj = cantFail(object::ObjectFile::createELFObjectFile(*buffer));
    for (auto &sec : obj->sections()) {
        cout << "sec: " << cantFail(sec.getName()).str() << endl;
        for (auto &sym : sec.relocations()) {
            cout << "    sym: " << cantFail(sym.getSymbol()->getName()).str() << " " << sym.getOffset() << endl;
        }
    }
    ofstream my_file("/tmp/jit.o");
    my_file.write(buffer->getBufferStart(), buffer->getBufferSize());
}
