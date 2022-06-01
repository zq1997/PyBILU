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
    unsigned end;
    llvm::BasicBlock *block;
    decltype(PyFrameObject::f_stackdepth) initial_stack_height{-1};
    bool is_handler{false};
    union {
        bool in_worklist;
        bool visited;
    };
    BitArray locals_touched;
    BitArray locals_deleted;
    union {
        BitArray locals_ever_deleted{};
        BitArray locals_input;
    };

    unsigned branch;
    // static constexpr auto branch_none = std::numeric_limits<decltype(branch)>::max();
    bool has_branch;
    bool fall_through;

    PyBasicBlock() {};
    PyBasicBlock(const PyBasicBlock &) = delete;
    ~PyBasicBlock() {
        locals_input.~BitArray();
    }
    auto operator=(const PyBasicBlock &) = delete;
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

class EmptyDebugInfoBuilder {
public:
    explicit EmptyDebugInfoBuilder(llvm::Module &module) {};

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
    llvm::BasicBlock *error_block;
    llvm::Value *code_names;
    llvm::Value *code_consts;

    PyCodeObject *py_code;
    BitArray redundant_loads;
    llvm::SmallVector<unsigned> live_block_indices;
    decltype(PyFrameObject::f_stackdepth) stack_depth;
    unsigned block_num;
    DynamicArray<decltype(stack_depth)> vpc_to_stack_depth;
    DynamicArray<PyBasicBlock> blocks;
    DynamicArray<std::pair<llvm::Value *, bool>> abstract_stack;
    decltype(stack_depth) abstract_stack_height;

    [[no_unique_address]] std::conditional_t<debug_build, DebugInfoBuilder, EmptyDebugInfoBuilder> di_builder;

    explicit CompileUnit(Context &context) : context{context}, di_builder{llvm_module} {};

    void parseCFG();
    void analyzeRedundantLoads();
    void analyzeLocalsDefinition();
    void translate();
    void emitBlock(unsigned index);
    void emitRotN(PyOparg n);

    llvm::Value *do_GETLOCAL(PyOparg oparg);
    void do_SETLOCAL(PyOparg oparg, llvm::Value *value, bool is_defined);
    llvm::Value *getName(int i);
    llvm::Value *getFreevar(int i);
    llvm::Value *getStackSlot(int i = 0);
    llvm::Value *fetchStackValue(int i);
    void do_PUSH(llvm::Value *value, bool really_pushed = true);

    class PoppedStackValue {
    public:
        llvm::Value *value;
        const bool really_pushed;

        PoppedStackValue(llvm::Value *value, bool really_pushed) : value{value}, really_pushed{really_pushed} {}

        PoppedStackValue(const PoppedStackValue &) = delete;

        operator llvm::Value *() { return value; }

        ~PoppedStackValue() { assert(!value); }
    };

    PoppedStackValue do_POP();

    llvm::Value *do_POPWithStolenRef();

    void do_Py_INCREF(llvm::Value *v);

    void do_Py_DECREF(llvm::Value *v);

    void do_Py_DECREF(PoppedStackValue &v) {
        assert(v.value);
        if (v.really_pushed) {
            do_Py_DECREF(static_cast<llvm::Value *>(v));
        }
        v.value = nullptr;
    }

    void do_Py_XDECREF(llvm::Value *v);

    void pyJumpIF(unsigned offset, bool pop_if_jump, bool jump_cond);
    unsigned findPyBlock(unsigned instr_offset);
    PyBasicBlock &findPyBlock(unsigned instr_offset, decltype(stack_depth) initial_stack_height);
    llvm::Value *getSymbol(size_t offset);

    template <typename T>
    std::enable_if_t<std::is_integral_v<T>, llvm::Constant *>
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
        return storeValue<M>(ptr, ptr, tbaa_node);
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
        auto call = callFunction<Attr>(type, callee, args...);
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

    void declarePendingBlock(unsigned index, decltype(stack_depth) initial_stack_height) {
        if (!blocks[index].visited) {
            blocks[index].visited = true;
            live_block_indices.push_back(index);
            blocks[index].initial_stack_height = initial_stack_height;
        }
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
