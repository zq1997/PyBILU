#include <memory>
#include <fstream>
#include <iostream>


#include <Python.h>

#include "llvm.h"


using namespace std;
using namespace llvm;
using namespace llvm::orc;

std::unique_ptr<llvm::StringMap<void *>> MyJIT::addrMap;

void MyJIT::init() {
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    addrMap = make_unique<StringMap<void *>, initializer_list<pair<StringRef, void *>>>(
            {
                    {"PyLong_FromLong", reinterpret_cast<void *>(PyLong_FromLong)},
                    {"PyNumber_Add",    reinterpret_cast<void *>(PyNumber_Add)},
            }
    );
}

llvm::Expected<unique_ptr<MyJIT>> MyJIT::create() {
    auto tm_builder = JITTargetMachineBuilder::detectHost();
    if (!tm_builder) {
        return tm_builder.takeError();
    }
    (*tm_builder).setCodeGenOptLevel(CodeGenOpt::Aggressive);
    auto tm = tm_builder->createTargetMachine();
    if (!tm) {
        return tm.takeError();
    }
    auto jit = unique_ptr<MyJIT>(new MyJIT(move(*tm)));
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
    ofstream my_file("/tmp/jit.o");
    my_file.write(buf.getBufferStart(), buf.getBufferSize());

    auto rel_text_sec = obj->section_end();
    auto text_sec = obj->section_end();
    for (auto &rel_sec: obj->sections()) {
        if (rel_sec.isText()) {
            text_sec = rel_sec;
        }
        auto sec = cantFail(rel_sec.getRelocatedSection());
        if (sec == obj->section_end() || !sec->isText()) {
            continue;
        }
        text_sec = *sec;
        rel_text_sec = rel_sec;
        break;
    }
    if (text_sec == obj->section_end()) {
        cerr << "错误" << endl;
        return nullptr;
    }
    auto code_data = cantFail(text_sec->getContents());
    SmallVector<char, 0> code(code_data.begin(), code_data.end());
    if (rel_text_sec != obj->section_end()) {
        for (auto &sym: rel_text_sec->relocations()) {
            auto name = cantFail(sym.getSymbol()->getName());
            auto offset = sym.getOffset();
            *(void **) (&code[offset]) = (*addrMap)[name];
        }
    }
    error_code ec;
    auto llvm_mem = sys::Memory::allocateMappedMemory(
            code.size(), nullptr, sys::Memory::ProtectionFlags::MF_RWE_MASK, ec);
    if (ec) {
        cerr << "错误" << endl;
        return nullptr;
    }
    memcpy(llvm_mem.base(), code.data(), code.size());
    return llvm_mem.base();
}
