#ifndef PYNIC_COMPILE_UNIT
#define PYNIC_COMPILE_UNIT

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
#include "translator.h"

using PyOparg = decltype(_Py_OPCODE(std::declval<_Py_CODEUNIT>()));

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
    unsigned end_index;
    llvm::BasicBlock *block;
    PyBasicBlock *worklist_link;
    BitArray locals_kept;
    BitArray locals_set;
    union {
        BitArray locals_ever_deleted{};
        BitArray locals_input;
    };
    union {
        unsigned _branch_offset;
        PyBasicBlock *branch;
    };
    union {
        PyBasicBlock *visited_eh_block{nullptr};
        PyBasicBlock *eh_setup_block;
    };
    int initial_stack_height;
    int stack_effect;
    int branch_stack_difference;
    bool fall_through{false};
    bool eh_body_enter{false};
    bool eh_body_exit{false};

    PyBasicBlock() {};
    PyBasicBlock(const PyBasicBlock &) = delete;

    ~PyBasicBlock() {
        locals_input.~BitArray();
    }

    operator llvm::BasicBlock *() { return block; }

    PyBasicBlock &next() { return this[1]; }
};

class DebugInfoBuilder {
    llvm::DIBuilder builder;
    llvm::DISubprogram *sp;

public:
    explicit DebugInfoBuilder(llvm::Module &module) : builder{module} {};

    void setFunction(llvm::IRBuilder<> &ir_builder, PyCodeObject *py_code, llvm::Function *function);

    void setLocation(llvm::IRBuilder<> &ir_builder, int vpc);

    void finalize() { builder.finalize(); }
};

class NullDebugInfoBuilder {
public:
    explicit NullDebugInfoBuilder(llvm::Module &module) {};

    void setFunction(llvm::IRBuilder<> &ir_builder, PyCodeObject *py_code, llvm::Function *function) {};

    void setLocation(llvm::IRBuilder<> &ir_builder, int vpc) {}

    void finalize() {}
};

class CompileUnit {
    Context &context;
    llvm::Module llvm_module{"the_module", context.llvm_context};
    llvm::IRBuilder<> builder{context.llvm_context};
    llvm::Function *function;
    llvm::Argument *shared_symbols;
    llvm::Argument *frame_obj;
    llvm::Value *rt_lasti;
    llvm::Value *coroutine_handler;
    llvm::BasicBlock *entry_block;
    llvm::BasicBlock *error_block;
    llvm::Value *code_names;
    llvm::Value *code_consts;
    llvm::IndirectBrInst *entry_jump;

    PyCodeObject *py_code;
    unsigned handler_num;
    unsigned block_num;
    unsigned try_block_num;
    DynamicArray<PyBasicBlock> blocks{};
    BitArray redundant_loads{};

    decltype(PyFrameObject::f_stackdepth) stack_height;
    DynamicArray<decltype(stack_height)> vpc_to_stack_height{};

    struct StackValue {
        llvm::Value *value;
        bool really_pushed;
    };

    DynamicArray<StackValue> abstract_stack{};
    decltype(stack_height) abstract_stack_height;

    [[no_unique_address]] std::conditional_t<debug_build, DebugInfoBuilder, NullDebugInfoBuilder> di_builder;

    explicit CompileUnit(Context &context) : context{context}, di_builder{llvm_module} {};

    void parseCFG();
    void doIntraBlockAnalysis();
    void doInterBlockAnalysis();
    void translate();
    void emitBlock(PyBasicBlock &this_block);
    void emitRotN(PyOparg n);

    std::pair<llvm::Value *, llvm::Value *> do_GETLOCAL(PyOparg oparg);
    llvm::Value *getName(int i);
    llvm::Value *getFreevar(int i);
    llvm::Value *getStackSlot(int i = 0);
    void do_PUSH(llvm::Value *value, bool really_pushed = true);

    struct FetchedStackValue : public StackValue {
        explicit FetchedStackValue(const StackValue &v) : StackValue{v} {}

        FetchedStackValue(const FetchedStackValue &) = delete;

        operator llvm::Value *() { return value; }
    };

    FetchedStackValue fetchStackValue(int i);

    struct PoppedStackValue : public StackValue {
        IF_DEBUG(bool has_decref{false};)

        explicit PoppedStackValue(const StackValue &v) : StackValue{v} {}

        PoppedStackValue(const PoppedStackValue &) = delete;

        ~PoppedStackValue() { assert(has_decref); }

        operator llvm::Value *() { return value; }
    };

    PoppedStackValue do_POP();

    llvm::Value *do_POP_with_newref();

    void popAndSave(llvm::Value *slot, llvm::MDNode *tbaa_node);

    llvm::Value *do_POP_N(PyOparg n) {
        abstract_stack_height -= n;
        stack_height -= n;
        for ([[maybe_unused]] auto &v : PtrRange(abstract_stack.getPointer(abstract_stack_height), n)) {
            assert(v.really_pushed);
        }
        return getStackSlot();
    }

    void do_Py_INCREF(llvm::Value *v);

    void do_Py_DECREF(llvm::Value *v);

    void do_Py_DECREF(FetchedStackValue &v) = delete;

    void do_Py_DECREF(PoppedStackValue &v) {
        if (v.really_pushed) {
            do_Py_DECREF(v.value);
        }
        IF_DEBUG(v.has_decref = true;)
    }

    void do_Py_XDECREF(llvm::Value *v);

    // TODO: FetchedStackValue等其他要不要实现INCREF和XDECREF

    void pyJumpIF(PyBasicBlock &current, bool pop_if_jump, bool jump_cond);
    llvm::Value *getSymbol(size_t offset);

    template <typename T>
    std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>, llvm::Constant *>
    asValue(T t) { return llvm::ConstantInt::get(context.type<T>(), t); }

    auto createBlock(const llvm::Twine &name) {
        return llvm::BasicBlock::Create(context.llvm_context, name, nullptr);
    }

    auto appendBlock(const char *extra) {
        return llvm::BasicBlock::Create(context.llvm_context, useName(extra), function);
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
        return storeValue<M>(value, ptr, tbaa_node);
    }

    template <llvm::AttributeList Context::* Attr = &Context::attr_default_call>
    llvm::CallInst *callFunction(llvm::FunctionType *type, llvm::Value *callee, auto &&... args) {
        auto call_instr = builder.CreateCall(type, callee, {args...});
        call_instr->setAttributes(context.*Attr);
        return call_instr;
    }

    template <auto &Symbol, llvm::AttributeList Context::* Attr = &Context::attr_default_call>
    llvm::CallInst *callSymbol(auto &&... args) {
        auto type = context.type<std::remove_reference_t<decltype(Symbol)>>();
        auto callee = getSymbol(searchSymbol<Symbol>());
        auto call = callFunction<Attr>(type, callee, static_cast<llvm::Value *>(args)...);
        if constexpr (Attr == &Context::attr_refcnt_call) {
            call->setCallingConv(llvm::CallingConv::PreserveMost);
        }
        return call;
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
    struct TranslatedResult {
        llvm::sys::MemoryBlock mem_block;
        DynamicArray<decltype(PyFrameObject::f_stackdepth)> sp_map;

        auto operator()(auto ...args) {
            auto f = reinterpret_cast<CompiledFunction *>(mem_block.base());
            return f(args...);
        }
    };

    static TranslatedResult *emit(Translator &translator, PyObject *py_code);
};

#endif
