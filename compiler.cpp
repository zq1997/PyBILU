#include <filesystem>
#include "Python.h"

#include "compiler.h"
#include "general_utilities.h"

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

    PassBuilder pb{&*machine};
    pb.registerModuleAnalyses(opt_MAM);
    pb.registerCGSCCAnalyses(opt_CGAM);
    pb.registerFunctionAnalyses(opt_FAM);
    pb.registerLoopAnalyses(opt_LAM);
    pb.crossRegisterProxies(opt_LAM, opt_FAM, opt_CGAM, opt_MAM);
    opt_MPM = pb.buildPerModuleDefaultPipeline(OptimizationLevel::O3);

    throwIf(machine->addPassesToEmitFile(out_PM, out_stream, nullptr, CodeGenFileType::CGFT_ObjectFile),
            "add emit pass error");
}

sys::MemoryBlock loadCode(llvm::SmallVector<char> &obj_vec) {
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
    if (ec) {
        // TODO: exception分为设置了PythonError的和没有的
        throw runtime_error(ec.message());
    }
    memcpy(mem.base(), code.data(), code.size());
    obj_vec.clear();
    return mem;
}

void unloadCode(sys::MemoryBlock &mem) {
    sys::Memory::releaseMappedMemory(mem);
}

static void notifyCodeLoaded(PyCodeObject *py_code, void *code_addr) {}

sys::MemoryBlock Compiler::compileForDebug(PyCodeObject *py_code, Module &mod) {
    SmallVector<char> opt_ll_vec{};
    raw_svector_ostream ll_os{opt_ll_vec};

    mod.print(ll_os, nullptr);
    auto ll_vec = SmallVector<char>{opt_ll_vec};

    opt_MPM.run(mod, opt_MAM);
    opt_MAM.clear();
    mod.print(ll_os, nullptr);

    out_PM.run(mod);

    callDebugHelperFunction("dump", reinterpret_cast<PyObject *>(py_code),
            PyObjectRef{PyMemoryView_FromMemory(ll_vec.data(), ll_vec.size(), PyBUF_READ)},
            PyObjectRef{PyMemoryView_FromMemory(opt_ll_vec.data(), opt_ll_vec.size(), PyBUF_READ)},
            PyObjectRef{PyMemoryView_FromMemory(out_vec.data(), out_vec.size(), PyBUF_READ)}
    );

    auto mem = loadCode(out_vec);
    notifyCodeLoaded(py_code, mem.base());
    return mem;
}

WrappedContext::WrappedContext(const DataLayout &dl) :
        context{},
        registered_types{RegisteredTypes::create((context.enableOpaquePointers(), context), dl)},
        c_null{ConstantPointerNull::get(type<void *>())} {
    auto md_builder = MDBuilder(context);
    likely_true = md_builder.createBranchWeights(INT32_MAX >> 1, 1);

    auto tbaa_root = md_builder.createTBAARoot("TBAA root for CPython");
    const auto &createTBAA = [&](const char *name, bool is_const = false) {
        // TODO: 为啥name为空就会被合并
        // if constexpr(!debug_build) {
        //     name = "";
        // }
        auto scalar_node = md_builder.createTBAANode(name, tbaa_root);
        return md_builder.createTBAAStructTagNode(scalar_node, scalar_node, 0, is_const);
    };
    tbaa_refcnt = createTBAA("reference counter");
    tbaa_obj_field = createTBAA("object field");
    tbaa_frame_value = createTBAA("frame value");
    tbaa_code_const = createTBAA("code const", true);

    auto attr_builder = AttrBuilder(context);
    attr_builder
            .addAttribute(Attribute::NoUnwind)
            .addAttribute(Attribute::NoReturn);
    attr_noreturn = AttributeList::get(context, AttributeList::FunctionIndex, attr_builder);
    attr_builder.clear();
    attr_builder
            .addAttribute(Attribute::NoUnwind)
            .addAttribute(Attribute::WillReturn)
            .addAttribute(Attribute::ArgMemOnly);
    attr_return = AttributeList::get(context, AttributeList::FunctionIndex, attr_builder);
    attr_builder.clear();
    attr_builder
            .addAttribute(Attribute::NoUnwind)
            .addAttribute("tune-cpu", sys::getHostCPUName())
            .addAttribute(Attribute::InaccessibleMemOrArgMemOnly);
    attr_default_call = AttributeList::get(context, AttributeList::FunctionIndex, attr_builder);
}
