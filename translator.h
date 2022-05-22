#ifndef PYNIC_TRANSLATOR
#define PYNIC_TRANSLATOR

#include <fstream>

#include <Python.h>
#include <frameobject.h>
#include <opcode.h>

#include <llvm/IR/Verifier.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/Support/Host.h>

#include "shared_symbols.h"
#include "general_utilities.h"
#include "compiler.h"


template <typename T>
auto useName(const T &arg) {
    if constexpr (debug_build) {
        if constexpr (std::is_convertible_v<T, std::string>) {
            return std::string(arg);
        }
        if constexpr (std::is_integral_v<T>) {
            return std::to_string(arg);
        }
        if constexpr (std::is_same_v<T, PyObject *>) {
            return std::string(PyUnicode_AsUTF8(arg));
        }
    } else {
        return llvm::Twine::createNull();
    }
}

template <typename T, typename... Ts>
auto useName(const T &arg, const Ts &... more) {
    if constexpr (debug_build) {
        return useName(arg) + useName(more...);
    } else {
        return llvm::Twine::createNull();
    }
}

struct PyBasicBlock {
    unsigned end;
    llvm::BasicBlock *block;
    decltype(PyFrameObject::f_stackdepth) initial_stack_height;
    bool is_handler;

    PyBasicBlock() = default;
    PyBasicBlock(const PyBasicBlock &) = delete;
    auto operator=(const PyBasicBlock &) = delete;
};

class DebugInfoBuilder {
    llvm::DIBuilder builder;
    llvm::DISubprogram *sp{};

public:
    DebugInfoBuilder(PyCodeObject *py_code,
            llvm::IRBuilder<> &ir_builder, llvm::Module &module, llvm::Function &function) : builder{module} {
        function.setName(PyStringAsString(py_code->co_name));
        auto res = callDebugHelperFunction("get_pydis_dir_and_file", reinterpret_cast<PyObject *>(py_code));
        auto *res_tuple = static_cast<PyObject *>(res);
        auto dis_dir = PyTuple_GET_ITEM(res_tuple, 0);
        auto dis_file = PyTuple_GET_ITEM(res_tuple, 1);

        auto file = builder.createFile(PyStringAsString(dis_file), PyStringAsString(dis_dir));
        builder.createCompileUnit(llvm::dwarf::DW_LANG_C, file, "pynic", false, "", 0);
        sp = builder.createFunction(file, "", "", file, 1, builder.createSubroutineType({}), 1,
                llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);
        function.setSubprogram(sp);
        setLocation(ir_builder, -2);
    };

    void setLocation(llvm::IRBuilder<> &ir_builder, int vpc) {
        ir_builder.SetCurrentDebugLocation(llvm::DILocation::get(ir_builder.getContext(), vpc + 3, 0, sp));
    }

    void finalize() { builder.finalize(); }
};


class WrappedModule {
public:
    struct TranslatedResult {
        llvm::sys::MemoryBlock mem_block;
        DynamicArray<decltype(PyFrameObject::f_stackdepth)> sp_map;

        auto operator()(auto ...args) {
            auto f = reinterpret_cast<CompiledFunction *>(mem_block.base());
            return f(args...);
        }
    };

private:
    WrappedContext &context;
    llvm::IRBuilder<> builder{context};

    llvm::Function *function{};
    llvm::Argument *shared_symbols{};
    llvm::Argument *frame_obj{};
    llvm::Value *rt_lasti{};
    llvm::BasicBlock *error_block{};
    llvm::Value *code_names{};
    llvm::Value *code_consts{};

    PyCodeObject *py_code{};
    decltype(PyFrameObject::f_stackdepth) stack_depth{};
    unsigned block_num{};
    DynamicArray<decltype(PyFrameObject::f_stackdepth)> vpc_to_stack_depth;
    DynamicArray<PyBasicBlock> blocks;

    llvm::Value *getSymbol(size_t offset);
    void parseCFG();
    void emitBlock(DebugInfoBuilder &di_builder, unsigned index);
    llvm::Value *do_GETLOCAL(PyOparg oparg);
    void do_SETLOCAL(PyOparg oparg, llvm::Value *value);
    llvm::Value *do_POP();
    void do_PUSH(llvm::Value *value);
    llvm::Value *getStackSlot(int i);
    llvm::Value *do_PEAK(int i);
    void do_SET_PEAK(int i, llvm::Value *value);
    llvm::Value *getName(int i);
    llvm::Value *getFreevar(int i);

    void do_Py_INCREF(llvm::Value *v);
    void do_Py_DECREF(llvm::Value *v);
    void do_Py_XDECREF(llvm::Value *v);

    void pyJumpIF(unsigned offset, bool pop_if_jump, bool jump_cond);
    PyBasicBlock &findPyBlock(unsigned instr_offset);

    template <typename T>
    std::enable_if_t<std::is_integral_v<T>, llvm::Constant *>
    asValue(T t) { return llvm::ConstantInt::get(context.type<T>(), t); }

    auto createBlock(const llvm::Twine &name, llvm::Function *parent) {
        return llvm::BasicBlock::Create(context, name, parent);
    }

    auto createBlock(const char *extra) {
        return llvm::BasicBlock::Create(context, useName(extra), function);
    }

    template <typename T>
    auto getPointer(llvm::Value *base, ptrdiff_t index, const llvm::Twine &name = "") {
        if (!index && name.isTriviallyEmpty()) {
            return base;
        }
        return builder.CreateInBoundsGEP(context.type<T>(), base, asValue(index), name);
    }

    template <typename T, typename M>
    auto getPointer(llvm::Value *instance, M T::* member, const llvm::Twine &name = "") {
        T dummy;
        auto offset = reinterpret_cast<const char *>(&(dummy.*member)) - reinterpret_cast<const char *>(&dummy);
        return getPointer<char>(instance, offset, name);
    }

    template <typename T>
    auto loadValue(llvm::Value *ptr, llvm::MDNode *tbaa_node, const llvm::Twine &name = "") {
        auto load_inst = new llvm::LoadInst(context.type<T>(), ptr, name, false, context.align<T>());
        builder.Insert(load_inst, name);
        load_inst->setMetadata(llvm::LLVMContext::MD_tbaa, tbaa_node);
        return load_inst;
    }

    template <typename T, typename M>
    auto loadFieldValue(llvm::Value *instance, M T::* member, llvm::MDNode *tbaa_node, const llvm::Twine &name = "") {
        auto ptr = getPointer(instance, member);
        return loadValue<M>(ptr, tbaa_node, name);
    }

    template <typename T>
    void storeValue(llvm::Value *value, llvm::Value *ptr, llvm::MDNode *tbaa_node) {
        auto store_inst = new llvm::StoreInst(value, ptr, false, context.align<T>());
        builder.Insert(store_inst);
        store_inst->setMetadata(llvm::LLVMContext::MD_tbaa, tbaa_node);
    }

    template <typename T, typename M>
    auto storeFiledValue(llvm::Value *value, llvm::Value *instance, M T::* member, llvm::MDNode *tbaa_node) {
        auto ptr = getPointer(instance, member);
        return storeValue<M>(ptr, ptr, tbaa_node);
    }

    template <llvm::AttributeList WrappedContext::* Attr = &WrappedContext::attr_default_call>
    llvm::CallInst *callFunction(llvm::FunctionType *type, llvm::Value *callee, auto... args) {
        auto call_instr = builder.CreateCall(type, callee, {args...});
        call_instr->setAttributes(context.*Attr);
        return call_instr;
    }

    template <auto &Symbol, llvm::AttributeList WrappedContext::* Attr = &WrappedContext::attr_default_call>
    llvm::CallInst *callSymbol(auto... args) {
        auto type = context.type<std::remove_reference_t<decltype(Symbol)>>();
        auto callee = getSymbol(searchSymbol<Symbol>());
        return callFunction<Attr>(type, callee, args...);
    }

    template <auto &Symbol>
    void emit_UNARY_OP() {
        auto value = do_POP();
        auto res = callSymbol<Symbol>(value);
        do_PUSH(res);
        do_Py_DECREF(value);
    }

    template <auto &Symbol>
    void emit_BINARY_OP() {
        auto right = do_POP();
        auto left = do_POP();
        auto res = callSymbol<Symbol>(left, right);
        do_PUSH(res);
        do_Py_DECREF(left);
        do_Py_DECREF(right);
    }

public:
    explicit WrappedModule(WrappedContext &context);

    TranslatedResult *translate(Compiler &compiler, PyCodeObject *code);
};

class QuickPyInstrIter {
    const PyInstr *base;
    const size_t limit;
public:
    size_t offset;
    PyOpcode opcode{};

    QuickPyInstrIter(const PyInstr *instr, size_t from, size_t to) : base(instr), limit(to), offset(from) {}

    auto next() {
        if (offset == limit) {
            return false;
        } else {
            opcode = _Py_OPCODE(base[offset]);
            offset++;
            return true;
        }
    }

    auto getOparg() {
        auto o = offset;
        auto oparg = _Py_OPARG(base[--o]);
        unsigned shift = 0;
        while (o && _Py_OPCODE(base[--o]) == EXTENDED_ARG) {
            shift += EXTENDED_ARG_BITS;
            oparg |= _Py_OPARG(base[o]) << shift;
        }
        return oparg;
    }
};

class PyInstrIter {
    const PyInstr *base;
    const size_t limit;
    // TODO: 也许可以模仿ceval不需要ext_oparg
    PyOparg ext_oparg{};
public:
    size_t offset;
    PyOpcode opcode{};
    PyOparg oparg{};

    PyInstrIter(const PyInstr *instr, size_t from, size_t to) : base(instr), limit(to), offset(from) {}

    explicit operator bool() const { return offset < limit; }

    auto next() {
        opcode = _Py_OPCODE(base[offset]);
        oparg = ext_oparg | _Py_OPARG(base[offset]);
        offset++;
        ext_oparg = 0;
    }

    auto extend_current_oparg() {
        ext_oparg = oparg << EXTENDED_ARG_BITS;
    }
    // void fetch_next() {
    //     assert(bool(*this));
    //     opcode = _Py_OPCODE(base[offset]);
    //     oparg = _Py_OPARG(base[offset]);
    //     offset++;
    // }
    //
    // auto extend_current_oparg() {
    //     assert(bool(*this));
    //     opcode = _Py_OPCODE(base[offset]);
    //     oparg = oparg << EXTENDED_ARG_BITS | _Py_OPARG(base[offset]);
    //     offset++;
    // }
};

#endif
