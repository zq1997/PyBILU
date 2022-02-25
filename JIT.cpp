#include <memory>
#include <iostream>
#include <fstream>


#include <Python.h>

#include "llvm.h"


using namespace std;
using namespace llvm;
using namespace llvm::orc;

std::unique_ptr<llvm::StringMap<void *>> MyJIT::addr_map;

void MyJIT::Init() {
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    addr_map = make_unique<StringMap<void *>, initializer_list<pair<StringRef, void *>>>(
            {
                    {"PyLong_FromLong", reinterpret_cast<void *>(PyLong_FromLong)},
                    {"PyNumber_Add",    reinterpret_cast<void *>(PyNumber_Add)},
            }
    );
}


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
    (*tm_builder).setCodeGenOptLevel(CodeGenOpt::Aggressive);
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

void *MyJIT::emitModule(llvm::Module &mod) {
    legacy::PassManager pass;
    SmallVector<char, 0> vec;
    raw_svector_ostream os(vec);
    if (machine->addPassesToEmitFile(pass, os, nullptr, CodeGenFileType::CGFT_ObjectFile)) {
        errs() << "TheTargetMachine can't emit a file of this type";
    }
    pass.run(mod);
    SmallVectorMemoryBuffer buf(move(vec));

    auto obj = cantFail(object::ObjectFile::createELFObjectFile(buf.getMemBufferRef()));
    for (auto &rel_sec: obj->sections()) {
        auto sec = cantFail(rel_sec.getRelocatedSection());
        if (sec == obj->section_end() || !sec->isText()) {
            continue;
        }
        auto sec_data = cantFail(sec->getContents());
        SmallVector<char, 0> code(sec_data.begin(), sec_data.end());
        for (auto &sym: rel_sec.relocations()) {
            auto name = cantFail(sym.getSymbol()->getName());
            auto offset = sym.getOffset();
            cout << "    sym: " << name.str()
                 << " " << offset
                 << " " << (*addr_map)[name]
                 << endl;
            *(void **) (&code[offset]) = (*addr_map)[name];
        }
        error_code ec;
        auto llvm_mem = sys::Memory::allocateMappedMemory(
                code.size(), nullptr, sys::Memory::ProtectionFlags::MF_RWE_MASK, ec);
        if (ec) {
            return nullptr;
        }
        cout << "allocating: " << code.size() << endl
             << "ec: " << ec << endl
             << "allocated: " << llvm_mem.allocatedSize() << " at " << llvm_mem.base() << endl;
        memcpy(llvm_mem.base(), code.data(), code.size());
        return llvm_mem.base();
    }
    return nullptr;
    // ofstream my_file("/tmp/jit.o");
    // my_file.write(buf.getBufferStart(), buf.getBufferSize());
}
