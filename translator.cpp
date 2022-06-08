#include "translator.h"

using namespace std;
using namespace llvm;


template <typename T>
inline void throwIf(const T &cond, const string &msg) {
    if (cond) {
        throw runtime_error(msg);
    }
}

template <typename T>
inline T check(Expected<T> v) {
    if (v) {
        return std::move(*v);
    } else {
        throw runtime_error(toString(v.takeError()));
    }
}

sys::MemoryBlock loadCode(llvm::SmallVector<char> &obj_vec) {
    assert(!obj_vec.empty());
    StringRef out_vec_ref{obj_vec.data(), obj_vec.size()};
    StringRef code{};
    auto obj = check(object::ObjectFile::createObjectFile(MemoryBufferRef(out_vec_ref, "")));
    for (auto &sec : obj->sections()) {
        if (sec.isText()) {
            assert(sec.relocations().empty());
            assert(code.empty());
            code = check(sec.getContents());
            assert(!code.empty());
        }
    }
    error_code ec;
    auto flag = sys::Memory::ProtectionFlags::MF_RWE_MASK;
    auto mem = sys::Memory::allocateMappedMemory(code.size(), nullptr, flag, ec);
    // TODO: exception分为设置了PythonError的和没有的
    if (ec) {
        throw runtime_error(ec.message());
    }
    memcpy(mem.base(), code.data(), code.size());
    return mem;
}

void unloadCode(sys::MemoryBlock &mem) {
    sys::Memory::releaseMappedMemory(mem);
}

Compiler::Compiler() {
    static bool initialized = false;
    if (!initialized) {
        throwIf(LLVMInitializeNativeTarget(), "LLVMInitializeNativeTarget() failed");
        throwIf(LLVMInitializeNativeAsmPrinter(), "LLVMInitializeNativeAsmPrinter() failed");
        initialized = true;
    }

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
    machine.reset(target->createTargetMachine(
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
    pb.registerCGSCCAnalyses(opt_CGAM);
    pb.registerFunctionAnalyses(opt_FAM);
    pb.registerLoopAnalyses(opt_LAM);
    pb.crossRegisterProxies(opt_LAM, opt_FAM, opt_CGAM, opt_MAM);
    opt_MPM = pb.buildPerModuleDefaultPipeline(OptimizationLevel::O3);

    throwIf(machine->addPassesToEmitFile(out_PM, out_stream, nullptr, CodeGenFileType::CGFT_ObjectFile),
            "add emit pass error");
}

Context::Context(const DataLayout &dl) :
        llvm_context{},
        registered_types{RegisteredTypes::create((llvm_context.enableOpaquePointers(), llvm_context), dl)} {
    c_null = ConstantPointerNull::get(type<void *>());

    auto md_builder = MDBuilder(llvm_context);
    likely_true = md_builder.createBranchWeights(INT32_MAX >> 1, 1);

    auto tbaa_root = md_builder.createTBAARoot("TBAA root for CPython");
    const auto &createTBAA = [&](const char *name, bool is_const = false) {
        // TODO: 似乎名字不可为空字符串，为空就会被取消掉
        auto scalar_node = md_builder.createTBAANode(name, tbaa_root);
        return md_builder.createTBAAStructTagNode(scalar_node, scalar_node, 0, is_const);
    };
    tbaa_refcnt = createTBAA("reference counter");
    tbaa_obj_field = createTBAA("object field");
    tbaa_frame_value = createTBAA("frame value");
    tbaa_code_const = createTBAA("code const", true);
    tbaa_symbols = createTBAA("symbols", true);

    auto attr_builder = AttrBuilder(llvm_context);
    attr_builder
            .addAttribute(Attribute::NoUnwind)
            .addAttribute(Attribute::NoReturn);
    attr_noreturn = AttributeList::get(llvm_context, AttributeList::FunctionIndex, attr_builder);
    attr_builder.clear();
    attr_builder
            .addAttribute(Attribute::NoUnwind)
            .addAttribute(Attribute::WillReturn)
            .addAttribute(Attribute::ArgMemOnly);
    attr_refcnt_call = AttributeList::get(llvm_context, AttributeList::FunctionIndex, attr_builder);
    attr_builder.clear();
    attr_builder
            .addAttribute(Attribute::NoUnwind)
            .addAttribute("tune-cpu", sys::getHostCPUName())
            .addAttribute(Attribute::InaccessibleMemOrArgMemOnly);
    attr_default_call = AttributeList::get(llvm_context, AttributeList::FunctionIndex, attr_builder);
}
