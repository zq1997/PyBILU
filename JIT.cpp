#include <memory>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <Python.h>

#include "JIT.h"


using namespace std;
using namespace llvm;

static unique_ptr<StringMap<void *>> addrMap;
static unique_ptr<TargetMachine> machine;

void MyJIT::init() {
    addrMap = make_unique<StringMap<void *>, initializer_list<pair<StringRef, void *>>>(
            {
                    {"PyLong_FromLong", reinterpret_cast<void *>(PyLong_FromLong)},
                    {"PyNumber_Add", reinterpret_cast<void *>(PyNumber_Add)},
            }
    );

    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) {
        throw runtime_error("err");
    }

    TargetOptions options;
    auto triple = sys::getProcessTriple();
    SubtargetFeatures features;
    StringMap<bool> feature_map;
    sys::getHostCPUFeatures(feature_map);
    for (auto &f: feature_map) {
        features.AddFeature(f.first(), f.second);
    }

    std::string err;
    auto *target = TargetRegistry::lookupTarget(triple, err);
    if (!target) {
        throw runtime_error(err);
    }
    auto *tm = target->createTargetMachine(
            triple,
            sys::getHostCPUName(),
            features.getString(),
            options, Reloc::Model::Static, CodeModel::Model::Large,
            CodeGenOpt::Aggressive, false
    );
    if (!tm) {
        throw runtime_error("");
    }
    machine = unique_ptr<TargetMachine>(tm);
}

MyJIT::MyJIT() {
    mod.setTargetTriple(machine->getTargetTriple().getTriple());
    mod.setDataLayout(machine->createDataLayout());

    t_PyType_Object = PointerType::getUnqual(context);
    t_PyObject = StructType::create({
            Type::getScalarTy<Py_ssize_t>(context),
            t_PyType_Object->getPointerTo()
    });
    t_PyObject_p = t_PyObject->getPointerTo();
}

void *emitModule(Module &mod) {
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

void *MyJIT::to_machine_code(void *cpy_ir) {
    // assert(PyBytes_CheckExact(cpy_ir->co_code));
    // auto instr_arr = PyBytes_AS_STRING(cpy_ir->co_code);
    // auto instr_size = PyBytes_GET_SIZE(cpy_ir->co_code);
    // auto instr_begin = reinterpret_cast<_Py_CODEUNIT *>(instr_arr);
    // auto instr_end = reinterpret_cast<_Py_CODEUNIT *>(instr_arr + instr_size);
    // for (auto iter = instr_begin; iter < instr_end; iter++) {
    //     auto opcode = _Py_OPCODE(*iter);
    //     auto oparg = _Py_OPARG(*iter);
    // }
    auto func = Function::Create(
            FunctionType::get(t_PyObject_p, {t_PyObject_p->getPointerTo()}, false),
            Function::ExternalLinkage,
            "main",
            &mod
    );
    func->setAttributes(attrs);

    BasicBlock *bb = BasicBlock::Create(context, "entry", func);
    builder.SetInsertPoint(bb);

    auto cpy_PyNumber_Add = Function::Create(
            FunctionType::get(
                    t_PyObject_p,
                    {t_PyObject_p, t_PyObject_p},
                    false
            ),
            Function::ExternalLinkage,
            "PyNumber_Add",
            mod
    );

    auto args = func->getArg(0);
    auto l = builder.CreateLoad(t_PyObject_p, builder.CreateConstInBoundsGEP1_32(t_PyObject_p, args, 0));
    auto r = builder.CreateLoad(t_PyObject_p, builder.CreateConstInBoundsGEP1_32(t_PyObject_p, args, 1));
    auto ret = builder.CreateCall(cpy_PyNumber_Add, {l, r});

    builder.CreateRet(ret);
    assert(!verifyFunction(*func, &errs()));
    mod.print(llvm::outs(), nullptr);

    auto addr = emitModule(mod);
    func->removeFromParent();
    delete func;
    return addr;
}
