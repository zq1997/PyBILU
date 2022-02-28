#include <memory>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <Python.h>

#include "JIT.h"


using namespace std;
using namespace llvm;

static unique_ptr<StringMap<char *>> addrMap;
static unique_ptr<TargetMachine> machine;

template <typename T>
T check(Expected<T> v) {
    if (v)
        return std::move(*v);
    else {
        throw runtime_error(toString(v.takeError()));
    }
}

void MyJIT::init() {
    addrMap = make_unique<StringMap<char *>, initializer_list<pair<StringRef, char *>>>(
            {
                    {"PyLong_FromLong", reinterpret_cast<char *>(PyLong_FromLong)},
                    {"PyNumber_Add", reinterpret_cast<char *>(PyNumber_Add)},
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
    machine = unique_ptr<TargetMachine>(target->createTargetMachine(
            triple,
            sys::getHostCPUName(),
            features.getString(),
            options,
            Reloc::Model::ROPI,
            CodeModel::Model::Large,
            CodeGenOpt::Aggressive,
            true
    ));
    if (!machine) {
        throw runtime_error("");
    }
}

MyJIT::MyJIT() {
    mod.setTargetTriple(machine->getTargetTriple().getTriple());
    mod.setDataLayout(machine->createDataLayout());

    PassBuilder pb{machine.get()};
    pb.registerModuleAnalyses(opt_mam);
    pb.registerCGSCCAnalyses(opt_cgam);
    pb.registerFunctionAnalyses(opt_fam);
    pb.registerLoopAnalyses(opt_lam);
    pb.crossRegisterProxies(opt_lam, opt_fam, opt_cgam, opt_mam);
    opt_fpm = pb.buildFunctionSimplificationPipeline(
            PassBuilder::OptimizationLevel::O3,
            ThinOrFullLTOPhase::None
    );

    if (machine->addPassesToEmitFile(out_pm, out_stream, nullptr, CodeGenFileType::CGFT_ObjectFile)) {
        throw runtime_error("add emit pass error");
    }

    t_PyType_Object = PointerType::getUnqual(context);
    t_PyObject = StructType::create({
            Type::getScalarTy<Py_ssize_t>(context),
            t_PyType_Object->getPointerTo()
    });
    t_PyObject_p = t_PyObject->getPointerTo();

    f_PyNumber_Add = Function::Create(
            FunctionType::get(
                    t_PyObject_p,
                    {t_PyObject_p, t_PyObject_p},
                    false
            ),
            Function::ExternalLinkage,
            "PyNumber_Add",
            mod
    );
    f_PyLong_FromLong = Function::Create(
            FunctionType::get(
                    t_PyObject_p,
                    {builder.getIntNTy(sizeof(long) * CHAR_BIT)},
                    false
            ),
            Function::ExternalLinkage,
            "PyLong_FromLong",
            mod
    );
}

void *emitModule(MemoryBufferRef &buf) {
    auto obj = check(object::ObjectFile::createObjectFile(buf));

    auto rel_text_sec = obj->section_end();
    auto text_sec = obj->section_end();
    for (auto &rel_sec: obj->sections()) {
        if (rel_sec.isText()) {
            text_sec = rel_sec;
        }
        auto sec = check(rel_sec.getRelocatedSection());
        if (sec == obj->section_end() || !sec->isText()) {
            continue;
        }
        text_sec = *sec;
        rel_text_sec = rel_sec;
        break;
    }
    if (text_sec == obj->section_end()) {
        throw runtime_error("cannot find right section");
    }

    auto sec_content = text_sec->getContents();
    if (!sec_content) {
        throw runtime_error("bad section error");
    }
    auto code = const_cast<char *>(sec_content->data());
    auto code_size = sec_content->size();

    auto[support, resolve] = object::getRelocationResolver(*obj);

    if (rel_text_sec != obj->section_end()) {
        for (auto &rel: rel_text_sec->relocations()) {
            if (!support(rel.getType())) {
                throw runtime_error("bad");
            }
            auto name = check(rel.getSymbol()->getName());
            auto sym_at = addrMap->lookup(name);
            auto offset = rel.getOffset();
            auto fix = object::resolveRelocation(resolve, rel, reinterpret_cast<uint64_t>(sym_at), 0);
            cout << reinterpret_cast<void *>(offset) << ":\t" << name.str() << " -> " << fix << endl;
            *(uint64_t *) (&code[offset]) = fix;
        }
    }
    error_code ec;
    auto flag = sys::Memory::ProtectionFlags::MF_RWE_MASK;
    auto llvm_mem = sys::Memory::allocateMappedMemory(code_size, nullptr, flag, ec);
    if (ec) {
        throw runtime_error("allocation error");
    }
    memcpy(llvm_mem.base(), code, code_size);
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

    auto args = func->getArg(0);
    auto l = builder.CreateLoad(t_PyObject_p, builder.CreateConstInBoundsGEP1_32(t_PyObject_p, args, 0));
    auto r = builder.CreateLoad(t_PyObject_p, builder.CreateConstInBoundsGEP1_32(t_PyObject_p, args, 1));
    auto ret = builder.CreateCall(f_PyNumber_Add, {l, r});

    auto bb2 = BasicBlock::Create(context, "block2", func);
    builder.CreateBr(bb2);
    builder.SetInsertPoint(bb2);

    auto b_addr = builder.CreatePtrToInt(BlockAddress::get(bb2), builder.getInt64Ty());
    auto more = builder.CreateCall(f_PyLong_FromLong, {b_addr});
    ret = builder.CreateCall(f_PyNumber_Add, {more, ret});

    builder.CreateRet(ret);
    assert(!verifyFunction(*func, &errs()));

    // mod.print(llvm::outs(), nullptr);
    opt_fpm.run(*func, opt_fam);
    mod.print(llvm::outs(), nullptr);
    out_vec.clear();
    out_pm.run(mod);
    ofstream("/tmp/jit.o").write(out_vec.data(), out_vec.size());
    MemoryBufferRef buf(StringRef(out_vec.data(), out_vec.size()), "");
    auto addr = emitModule(buf);
    func->removeFromParent();
    delete func;
    return addr;
}
