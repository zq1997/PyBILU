#include <memory>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <Python.h>

#include "JIT.h"


using namespace std;
using namespace llvm;

static unique_ptr<TargetMachine> machine;

struct SymbolTable {
    void *PyNumber_Add{reinterpret_cast<void *>(::PyNumber_Add)};
    void *PyNumber_Multiply{reinterpret_cast<void *>(::PyNumber_Multiply)};
} extern const symbol_table{};

template<typename T>
inline T check(Expected<T> v) {
    if (v) {
        return std::move(*v);
    } else {
        throw runtime_error(toString(v.takeError()));
    }
}

template <typename T>
inline void throwIf(const T&cond, const string &msg) {
    if (cond) {
        throw runtime_error(msg);
    }
}


class {
public:
    const char *file_name{"/tmp/jit"};
    bool output{true};

    inline void dump(const string &ext, const char *data, size_t size) const {
        if (output) {
            ofstream(file_name + ext).write(data, size);
        }
    }

    inline void dump(const string &ext, const Module &mod) const {
        if (output) {
            error_code ec;
            raw_fd_ostream out(file_name + ext, ec);
            mod.print(out, nullptr);
            throwIf(ec, "cannnot write file: " + string(file_name) + ext);
        }
    }
} debug;

void MyJIT::init() {
    throwIf(LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter(), "initialization failed");

    auto triple = sys::getProcessTriple();
    SubtargetFeatures features;
    StringMap<bool> feature_map;
    sys::getHostCPUFeatures(feature_map);
    for (auto &f: feature_map) {
        features.AddFeature(f.first(), f.second);
    }

    std::string err;
    auto *target = TargetRegistry::lookupTarget(triple, err);
    throwIf(!target, err);
    machine = unique_ptr<TargetMachine>(target->createTargetMachine(
            triple,
            sys::getHostCPUName(),
            features.getString(),
            TargetOptions(),
            Reloc::Model::PIC_,
            CodeModel::Model::Small,
            CodeGenOpt::Aggressive
    ));
    throwIf(!machine, "cannot create TargetMachine");
}

MyJIT::MyJIT() {
    mod.setTargetTriple(machine->getTargetTriple().getTriple());
    mod.setDataLayout(machine->createDataLayout());

    PassBuilder pb{machine.get()};
    pb.registerModuleAnalyses(opt_MAM);
    pb.registerFunctionAnalyses(opt_FAM);
    opt_FAM.registerPass([&] { return ModuleAnalysisManagerFunctionProxy(opt_MAM); });
    opt_FPM = pb.buildFunctionSimplificationPipeline(
            PassBuilder::OptimizationLevel::O3,
            ThinOrFullLTOPhase::None
    );

    throwIf(machine->addPassesToEmitFile(out_PM, out_stream, nullptr, CodeGenFileType::CGFT_ObjectFile),
            "add emit pass error");
}

CallInst *createCall(MyJIT &jit, Value *table, void *const &func, Type *result,
        ArrayRef<Type *> arg_types, initializer_list<Value *> args) {
    auto bf_type = FunctionType::get(result, arg_types, false);
    auto offset = reinterpret_cast<const char *>(&func) - reinterpret_cast<const char *>(&symbol_table);
    auto entry = jit.builder.CreateInBoundsGEP(jit.type_char, table,
            ConstantInt::get(jit.type_ptrdiff, offset));
    entry = jit.builder.CreatePointerCast(entry, bf_type->getPointerTo()->getPointerTo());
    entry = jit.builder.CreateLoad(bf_type->getPointerTo(), entry);
    return jit.builder.CreateCall(bf_type, entry, args);
}

void MyJIT::compile_function(Function *func, void *cpy_ir) {
    // assert(PyBytes_CheckExact(cpy_ir->co_code));
    // auto instr_arr = PyBytes_AS_STRING(cpy_ir->co_code);
    // auto instr_size = PyBytes_GET_SIZE(cpy_ir->co_code);
    // auto instr_begin = reinterpret_cast<_Py_CODEUNIT *>(instr_arr);
    // auto instr_end = reinterpret_cast<_Py_CODEUNIT *>(instr_arr + instr_size);
    // for (auto iter = instr_begin; iter < instr_end; iter++) {
    //     auto opcode = _Py_OPCODE(*iter);
    //     auto oparg = _Py_OPARG(*iter);
    // }

    auto table = func->getArg(0);;
    auto args = func->getArg(1);
    auto l = builder.CreateLoad(type_char_p, builder.CreateConstInBoundsGEP1_32(type_char_p, args, 0));
    auto r = builder.CreateLoad(type_char_p, builder.CreateConstInBoundsGEP1_32(type_char_p, args, 1));
    Type *arg_types[]{type_char_p, type_char_p};
    auto ll = createCall(*this, table, symbol_table.PyNumber_Multiply, type_char_p, arg_types, {l, l});
    auto rr = createCall(*this, table, symbol_table.PyNumber_Multiply, type_char_p, arg_types, {r, r});
    auto ret = createCall(*this, table, symbol_table.PyNumber_Add, type_char_p, arg_types, {ll, rr});

    builder.CreateRet(ret);
}

void *MyJIT::compile(void *cpy_ir) {
    auto func = Function::Create(
            FunctionType::get(type_char_p, {
                    type_char_p,
                    type_char_p->getPointerTo()
            }, false),
            Function::ExternalLinkage,
            "",
            &mod
    );
    func->setAttributes(attrs);
    builder.SetInsertPoint(BasicBlock::Create(context, "", func));
    compile_function(func, cpy_ir);
    assert(!verifyFunction(*func, &errs()));

    debug.dump(".ll", mod);
    opt_FPM.run(*func, opt_FAM);
    debug.dump(".opt.ll", mod);
    opt_FAM.clear();
    out_vec.clear();
    out_PM.run(mod);
    func->removeFromParent();
    delete func;
    debug.dump(".o", out_vec.data(), out_vec.size());
    MemoryBufferRef buf(StringRef(out_vec.data(), out_vec.size()), "");

    StringRef code{nullptr, 0};
    auto obj = check(object::ObjectFile::createObjectFile(buf));
    for (auto &sec: obj->sections()) {
        assert(sec.relocations().empty());
        if (sec.isText()) {
            assert(!code.data());
            code = check(sec.getContents());
        }
    }

    error_code ec;
    auto flag = sys::Memory::ProtectionFlags::MF_RWE_MASK;
    auto llvm_mem = sys::Memory::allocateMappedMemory(code.size(), nullptr, flag, ec);
    throwIf(ec, ec.message());
    memcpy(llvm_mem.base(), code.data(), code.size());

    return llvm_mem.base();
}
