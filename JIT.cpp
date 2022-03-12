#include <memory>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <Python.h>

#include "JIT.h"


using namespace std;
using namespace llvm;

template <typename T>
inline T check(Expected<T> v) {
    if (v) {
        return std::move(*v);
    } else {
        throw runtime_error(toString(v.takeError()));
    }
}

template <typename T>
inline void throwIf(const T &cond, const string &msg) {
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


void *Translator::operator()(Compiler &compiler, PyCodeObject *cpy_ir) {
    parseCFG(cpy_ir);
    for (auto &b : Range(block_num, &*ir_blocks)) {
        b = BasicBlock::Create(context, "", func);
    }
    builder.SetInsertPoint(*ir_blocks);
    py_stack = builder.CreateAlloca(ctype_ptr, builder.getInt32(cpy_ir->co_stacksize));
    for (auto &i : Range(block_num)) {
        emitBlock(i);
    }

    auto result = compiler(mod);

    func->dropAllReferences();

    return result;
}

void Translator::parseCFG(PyCodeObject *cpy_ir) {
    py_instructions = reinterpret_cast<PyInstr *>(PyBytes_AS_STRING(cpy_ir->co_code));
    auto size = PyBytes_GET_SIZE(cpy_ir->co_code) / sizeof(PyInstr);

    PyInstrIter iter(py_instructions, size);
    BitArray is_boundary(size + 1);

    while (auto instr = iter.next()) {
        switch (instr.opcode) {
        case JUMP_ABSOLUTE:
        case JUMP_IF_TRUE_OR_POP:
        case JUMP_IF_FALSE_OR_POP:
        case POP_JUMP_IF_TRUE:
        case POP_JUMP_IF_FALSE:
        case JUMP_IF_NOT_EXC_MATCH:
            is_boundary.set(iter.getOffset());
            is_boundary.set(instr.oparg);
            break;
        case JUMP_FORWARD:
            is_boundary.set(iter.getOffset());
            is_boundary.set(iter.getOffset() + instr.oparg);
            break;
        case FOR_ITER:
            is_boundary.set(iter.getOffset() + instr.oparg);
            break;
        case RETURN_VALUE:
        case RAISE_VARARGS:
        case RERAISE:
            is_boundary.set(iter.getOffset());
        default:
            break;
        }
    }

    is_boundary.set(0, false);
    assert(is_boundary.get(size));

    block_num = is_boundary.count(size);
    boundaries.reserve(block_num);
    ir_blocks.reserve(block_num);

    unsigned current_num = 0;
    for (auto i : Range(BitArray::chunkNumber(size))) {
        auto bits = is_boundary[i];
        BitArray::ValueType tester{1};
        for (unsigned j = 0; j < BitArray::BitsPerValue; j++) {
            if (tester & bits) {
                boundaries[current_num++] = i * BitArray::BitsPerValue + j;
            }
            tester <<= 1;
        }
    }
    assert(current_num == block_num);
}

Compiler::Compiler() {
    throwIf(LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter(), "initialization failed");

    auto triple = sys::getProcessTriple();
    SubtargetFeatures features;
    StringMap<bool> feature_map;
    sys::getHostCPUFeatures(feature_map);
    for (auto &f : feature_map) {
        features.AddFeature(f.first(), f.second);
    }

    string err;
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


    PassBuilder pb{machine.get()};
    pb.registerModuleAnalyses(opt_MAM);
    pb.registerFunctionAnalyses(opt_FAM);
    pb.registerLoopAnalyses(opt_LAM);
    opt_FAM.registerPass([&] { return ModuleAnalysisManagerFunctionProxy(opt_MAM); });
    opt_FAM.registerPass([&] { return LoopAnalysisManagerFunctionProxy(opt_LAM); });
    opt_LAM.registerPass([&] { return FunctionAnalysisManagerLoopProxy(opt_FAM); });
    opt_FPM = pb.buildFunctionSimplificationPipeline(
            PassBuilder::OptimizationLevel::O3,
            ThinOrFullLTOPhase::None
    );

    throwIf(machine->addPassesToEmitFile(out_PM, out_stream, nullptr, CodeGenFileType::CGFT_ObjectFile),
            "add emit pass error");
}

void *Compiler::operator()(llvm::Module &mod) {
    debug.dump(".ll", mod);
    for (auto &func : mod) {
        assert(!verifyFunction(func, &errs()));
        opt_FPM.run(func, opt_FAM);
    }
    opt_LAM.clear();
    opt_FAM.clear();
    debug.dump(".opt.ll", mod);

    mod.setDataLayout(machine->createDataLayout());
    out_PM.run(mod);
    debug.dump(".o", out_vec.data(), out_vec.size());

    StringRef code{};
    auto obj = check(object::ObjectFile::createObjectFile(
            MemoryBufferRef(StringRef(out_vec.data(), out_vec.size()), "")));
    for (auto &sec : obj->sections()) {
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

    out_vec.clear();
    return llvm_mem.base();
}
