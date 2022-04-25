#ifndef PYNIC_TRANSLATOR
#define PYNIC_TRANSLATOR

#include <Python.h>
#include <frameobject.h>
#include <opcode.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/Support/Host.h>

#include "shared_symbols.h"
#include "general_utilities.h"
#include "compiler.h"

#ifdef NDEBUG
constexpr auto debug_build = false;
#else
constexpr auto debug_build = true;
#endif

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
    llvm::BasicBlock *llvm_block;
};

class Translator {
    const llvm::DataLayout &data_layout;
    llvm::LLVMContext context{};
    const RegisteredLLVMTypes types{(context.enableOpaquePointers(), context), data_layout};
    llvm::IRBuilder<> builder{context};
    llvm::Module mod{"singleton_module", context};
    llvm::Function *func{llvm::Function::Create(
            types.get<TranslatedFunctionType>(),
            llvm::Function::ExternalLinkage, "singleton_function", &mod
    )};
    llvm::Argument *simple_frame{func->getArg(1)};
    llvm::MDNode *likely_true{};
    llvm::MDNode *tbaa_refcnt{};
    llvm::MDNode *tbaa_frame_slot{};
    llvm::MDNode *tbaa_frame_cells{};
    llvm::MDNode *tbaa_code_const{};
    llvm::MDNode *tbaa_frame_status{};
    llvm::AttributeList attr_noreturn{};
    llvm::AttributeList attr_inaccessible_or_arg{};

    PyCodeObject *py_code{};
    PyInstr *py_instructions{};
    decltype(PyFrameObject::f_lasti) lasti{};
    decltype(PyFrameObject::f_stackdepth) stack_height{};
    unsigned block_num{};
    DynamicArray<PyBasicBlock> blocks;
    llvm::BasicBlock *error_block{};
    DynamicArray<llvm::Value *> py_names;
    DynamicArray<llvm::Value *> py_consts;
    DynamicArray<llvm::Value *> py_locals;
    DynamicArray<llvm::Value *> py_freevars;
    DynamicArray<llvm::Value *> py_stack;
    llvm::Value *py_symbols[external_symbol_count]{};

    llvm::Constant *c_null{llvm::ConstantPointerNull::get(types.get<void *>())};
    llvm::Value *rt_lasti{};
    llvm::Value *rt_stack_height_pointer{};


    template <typename T>
    std::enable_if_t<std::is_integral_v<T>, llvm::Constant *>
    asValue(T t) {
        return llvm::ConstantInt::get(types.get<T>(), t);
    }

    // template <typename T>
    // std::enable_if_t<std::is_base_of_v<llvm::Value, std::remove_pointer_t<T>>, llvm::Value *>
    // asValue(llvm::Value *t) {
    //     return t;
    // }

    auto createBlock(const llvm::Twine &name, llvm::Function *parent) {
        return llvm::BasicBlock::Create(context, name, parent);
    }

    auto createBlock(const char *extra) {
        return llvm::BasicBlock::Create(context, useName("$instr.", lasti, ".", extra), func);
    }

    template <typename T=char>
    auto getPointer(llvm::Value *base, size_t index, const llvm::Twine &name = "") {
        if (!index && name.isTriviallyEmpty()) {
            return base;
        }
        auto offset_value = asValue(sizeof(T) * index);
        return builder.CreateInBoundsGEP(types.get<char>(), base, offset_value, name);
    }

    template <typename T, typename M>
    auto getPointer(llvm::Value *instance, M T::* member, const llvm::Twine &name = "") {
        T dummy;
        auto offset = reinterpret_cast<const char *>(&(dummy.*member)) - reinterpret_cast<const char *>(&dummy);
        return getPointer(instance, offset, name);
    }

    template <typename T>
    auto loadValue(llvm::Value *ptr, llvm::MDNode *tbaa_node, const llvm::Twine &name = "") {
        auto type = types.getAll<T>();
        auto load_inst = new llvm::LoadInst(type.type, ptr, name, false, type.align);
        builder.Insert(load_inst, name);
        load_inst->setMetadata(llvm::LLVMContext::MD_tbaa, tbaa_node);
        return load_inst;
    }

    template <typename T>
    auto loadElementValue(llvm::Value *base, size_t index, llvm::MDNode *tbaa_node, const llvm::Twine &name = "") {
        auto ptr = getPointer<T>(base, index);
        return loadValue<T>(ptr, tbaa_node, name);
    }

    template <typename T, typename M>
    auto loadFieldValue(llvm::Value *instance, M T::* member, llvm::MDNode *tbaa_node, const llvm::Twine &name = "") {
        auto ptr = getPointer(instance, member);
        return loadValue<M>(ptr, tbaa_node, name);
    }

    template <typename T>
    void storeValue(llvm::Value *value, llvm::Value *ptr, llvm::MDNode *tbaa_node) {
        auto type = types.getAll<T>();
        auto store_inst = new llvm::StoreInst(value, ptr, false, type.align);
        builder.Insert(store_inst);
        store_inst->setMetadata(llvm::LLVMContext::MD_tbaa, tbaa_node);
    }

    template <typename T>
    auto storeElementValue(llvm::Value *value, llvm::Value *base, size_t index, llvm::MDNode *tbaa_node) {
        auto ptr = getPointer<T>(base, index);
        return storeValue<T>(value, ptr, tbaa_node);
    }

    template <typename T, typename M>
    auto storeFiledValue(llvm::Value *value, llvm::Value *instance, M T::* member, llvm::MDNode *tbaa_node) {
        auto ptr = getPointer(instance, member);
        return storeValue<M>(ptr, ptr, tbaa_node);
    }

    template <typename T>
    auto readData(llvm::Value *base, size_t index, const llvm::Twine &name = "") {
        return builder.CreateLoad(types.get<T>(), getPointer<T>(base, index), name);
    }

    template <typename T, typename M>
    auto readData(llvm::Value *instance, M T::* member, const llvm::Twine &name = "") {
        return builder.CreateLoad(types.get<M>(), getPointer(instance, member), name);
    }

    template <typename T>
    auto writeData(llvm::Value *base, size_t index, llvm::Value *value) {
        return builder.CreateStore(value, getPointer<T>(base, index));
    }

    template <typename T, typename M>
    auto writeData(llvm::Value *instance, M T::* member, llvm::Value *value) {
        return builder.CreateStore(value, getPointer(instance, member));
    }

    llvm::Value *getSymbol(size_t i);

    void parseCFG();
    void emitBlock(unsigned index);
    llvm::Value *do_GETLOCAL(PyOparg oparg);
    void do_SETLOCAL(PyOparg oparg, llvm::Value *value);
    llvm::Value *do_POP();
    void do_PUSH(llvm::Value *value);

    auto do_PEAK(int i) {
        return builder.CreateLoad(types.get<PyObject *>(), py_stack[stack_height - i]);
    }

    auto do_SET_PEAK(int i, llvm::Value *value) {
        return builder.CreateStore(value, py_stack[stack_height - i]);
    }

    void do_Py_INCREF(llvm::Value *v);
    void do_Py_DECREF(llvm::Value *v);
    void do_Py_XDECREF(llvm::Value *v);


    template <llvm::AttributeList Translator::* Attr = &Translator::attr_inaccessible_or_arg, typename... Args>
    llvm::CallInst *do_Call(llvm::FunctionType *type, llvm::Value *callee, Args... args) {
        auto call_instr = builder.CreateCall(type, callee, {args...});
        call_instr->setAttributes(this->*Attr);
        return call_instr;
    }

    template <typename T, typename M, typename... Args>
    llvm::CallInst *do_CallSlot(llvm::Value *instance, M *T::* member, Args... args) {
        return do_Call(types.get<M>(), readData(instance, member), args...);
    }

    template <auto &S, llvm::AttributeList Translator::* Attr = &Translator::attr_inaccessible_or_arg, typename... Args>
    llvm::CallInst *do_CallSymbol(Args... args) {
        return do_Call<Attr>(types.get<std::remove_reference_t<decltype(S)>>(), getSymbol(searchSymbol<S>()), args...);
    }

    void handle_UNARY_OP(size_t i) {
        auto value = do_POP();
        using UnaryFunction = PyObject *(PyObject *);
        auto res = do_Call(types.get<UnaryFunction>(), getSymbol(i), value);
        do_PUSH(res);
        do_Py_DECREF(value);
    }

    // TODO:泛型化
    void handle_BINARY_OP(size_t i) {
        auto right = do_POP();
        auto left = do_POP();
        using BinaryFunction = PyObject *(PyObject *, PyObject *);
        auto res = do_Call(types.get<BinaryFunction>(), getSymbol(i), left, right);
        do_PUSH(res);
        do_Py_DECREF(left);
        do_Py_DECREF(right);
    }

    llvm::BasicBlock *findBlock(unsigned instr_offset);

    // TODO：删除
    void createIf(llvm::Value *cond, const std::function<void()> &make_body, bool terminated = false) {
        auto block_true = createBlock(useName("if_true"), func);
        auto block_end = createBlock(useName("if_false"), nullptr);
        builder.CreateCondBr(cond, block_true, block_end);
        builder.SetInsertPoint(block_true);
        make_body();
        if (!terminated) {
            builder.CreateBr(block_end);
        }
        block_end->insertInto(func);
        builder.SetInsertPoint(block_end);
    }

public:
    explicit Translator(const llvm::DataLayout &dl);

    void *operator()(Compiler &compiler, PyCodeObject *code);
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
        oparg = ext_oparg << EXTENDED_ARG_BITS | _Py_OPARG(base[offset]);
        offset++;
        ext_oparg = 0;
    }

    auto extend_current_oparg() {
        ext_oparg = oparg;
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
