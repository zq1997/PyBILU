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
    pb.registerCGSCCAnalyses(opt_CGAM);
    pb.registerFunctionAnalyses(opt_FAM);
    pb.registerLoopAnalyses(opt_LAM);
    pb.crossRegisterProxies(opt_LAM, opt_FAM, opt_CGAM, opt_MAM);
    opt_MPM = pb.buildPerModuleDefaultPipeline(OptimizationLevel::O3);

    // pb.registerModuleAnalyses(opt_MAM);
    // pb.registerFunctionAnalyses(opt_FAM);
    // pb.registerLoopAnalyses(opt_LAM);
    // opt_FAM.registerPass([&] { return ModuleAnalysisManagerFunctionProxy(opt_MAM); });
    // opt_FAM.registerPass([&] { return LoopAnalysisManagerFunctionProxy(opt_LAM); });
    // opt_LAM.registerPass([&] { return FunctionAnalysisManagerLoopProxy(opt_FAM); });
    // opt_FPM = pb.buildFunctionSimplificationPipeline(
    //         OptimizationLevel::O3,
    //         ThinOrFullLTOPhase::None
    // );

    throwIf(machine->addPassesToEmitFile(out_PM, out_stream, nullptr, CodeGenFileType::CGFT_ObjectFile),
            "add emit pass error");
}

sys::MemoryBlock loadCode(llvm::SmallVector<char> &obj_vec) {
    StringRef out_vec_ref{obj_vec.data(), obj_vec.size()};
    StringRef code;
    auto obj = check(object::ObjectFile::createObjectFile(MemoryBufferRef(out_vec_ref, "")));
    for (auto &sec : obj->sections()) {
        if (sec.isText()) {
            assert(sec.relocations().empty());
            assert(!code.data());
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
    struct PyObjectRef {
        PyObject *o;

        explicit PyObjectRef(PyObject *o) : o{o} {
            if (!o) {
                throw bad_exception();
            }
        }

        ~PyObjectRef() { Py_DECREF(o); }

        operator PyObject *() const { return o; }
    };

    SmallVector<char> opt_ll_vec{};
    raw_svector_ostream ll_os{opt_ll_vec};

    mod.print(ll_os, nullptr);
    auto ll_vec = SmallVector<char>{opt_ll_vec};

    opt_MPM.run(mod, opt_MAM);
    opt_MAM.clear();
    mod.print(ll_os, nullptr);

    out_PM.run(mod);

    PyObjectRef dump_mod{PyImport_ImportModule("dump")};
    PyObjectRef dump_func{PyObject_GetAttrString(dump_mod, "dump")};
    PyObjectRef ll_bytes{PyMemoryView_FromMemory(ll_vec.data(), ll_vec.size(), PyBUF_READ)};
    PyObjectRef opt_ll_bytes{PyMemoryView_FromMemory(opt_ll_vec.data(), opt_ll_vec.size(), PyBUF_READ)};
    PyObjectRef obj_bytes{PyMemoryView_FromMemory(out_vec.data(), out_vec.size(), PyBUF_READ)};
    PyObject *args[]{
            nullptr, reinterpret_cast<PyObject *>(py_code), ll_bytes, opt_ll_bytes, obj_bytes
    };
    PyObjectRef res{PyObject_Vectorcall(dump_func, &args[1], 4 | PY_VECTORCALL_ARGUMENTS_OFFSET, nullptr)};

    auto mem = loadCode(out_vec);
    notifyCodeLoaded(py_code, mem.base());
    return mem;
}
