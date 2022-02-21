#include <memory>

#include "llvm.h"

using namespace std;
using namespace llvm;
using namespace llvm::orc;


MyJIT::MyJIT(unique_ptr<TargetMachine> machine,
             unique_ptr<SelfExecutorProcessControl> epc) :
        machine(move(machine)),
        sess(move(epc)),
        object_layer(sess, make_unique<SectionMemoryManager>),
        compile_layer(sess, object_layer, make_unique<SimpleCompiler>(*this->machine)),
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
    auto dl = tm_builder->getDefaultDataLayoutForTarget();
    if (!dl) {
        return dl.takeError();
    }
    auto jit = unique_ptr<MyJIT>(new MyJIT(move(*tm), move(*epc)));
    auto gen = DynamicLibrarySearchGenerator::GetForCurrentProcess(dl->getGlobalPrefix());
    if (!gen) {
        return gen.takeError();
    }
    jit->dylib.addGenerator(move(*gen));
    return jit;
}
