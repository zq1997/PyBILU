#include <climits>
#include <cstddef>
#include <memory>
#include <bitset>
#include <tuple>
#include <array>
#include <type_traits>

#include <Python.h>
#include <opcode.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm-c/Target.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/Memory.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/MC/SubtargetFeature.h>

using PyInstr = const _Py_CODEUNIT;
using PyOpcode = decltype(_Py_OPCODE(PyInstr{}));
using PyOparg = decltype(_Py_OPCODE(PyInstr{}));
constexpr auto EXTENDED_ARG_BITS = 8;


template <typename Ret, typename ...Args>
union SymbolEntry {
public:
    Ret (*func_addr)(Args...);
    llvm::FunctionType *func_type;

    explicit SymbolEntry(Ret(*addr)(Args...)) noexcept: func_addr{addr} {}

    auto &operator=(const SymbolEntry &other) noexcept {
        assert(this != &other);
        func_type = nullptr;
        return *this;
    }
};

struct SymbolTable {
    decltype(SymbolEntry{::PyNumber_Add}) PyNumber_Add{::PyNumber_Add};
    decltype(SymbolEntry{::PyNumber_Add}) PyNumber_Multiply{::PyNumber_Multiply};
} extern const global_symbol_table;

template <typename T1, typename T2>
inline auto calcDistance(T1 &from, T2 &to) {
    return reinterpret_cast<const char *>(&to) - reinterpret_cast<const char *>(&from);
}


template <typename T, typename = void>
struct HasDereference : std::false_type {};
template <typename T>
struct HasDereference<T, std::void_t<decltype(*std::declval<T>())>> : std::true_type {};


template <typename Size=size_t, typename Iter=Size>
class Range {
    static_assert(std::is_integral<Size>::value);
    const Iter from;
    const Iter to;
public:
    class Iterator {
        Iter i;
    public:
        explicit Iterator(const Iter &i_) : i{i_} {}

        auto &operator++() {
            ++i;
            return *this;
        }

        auto operator!=(const Iterator &o) const { return o.i != i; }

        auto &operator*() {
            if constexpr(HasDereference<Iter>::value) {
                return *i;
            } else {
                return i;
            }
        }
    };

    Range(Size n, Iter from) : from{from}, to{from + n} {}

    explicit Range(Size n) : Range(n, Size{}) {}

    auto begin() { return Iterator{from}; }

    auto end() { return Iterator{to}; }
};

template <typename T>
class DynamicArray {
    std::unique_ptr<T[]> data;

public:
    using ValueType = T;

    DynamicArray() = default;

    explicit DynamicArray(size_t size, bool init = false) : data{init ? new T[size]{} : new T[size]} {};

    void reserve(size_t size, bool init = false) {
        data.reset(init ? new T[size]{} : new T[size]);
    }

    auto &operator[](size_t index) { return data[index]; }

    const auto &operator[](size_t index) const { return data[index]; }

    auto &operator*() const { return data[0]; }
};


class Compiler {
    std::unique_ptr<llvm::TargetMachine> machine;

    llvm::ModuleAnalysisManager opt_MAM;
    llvm::FunctionAnalysisManager opt_FAM;
    llvm::FunctionPassManager opt_FPM;

    llvm::legacy::PassManager out_PM;
    llvm::SmallVector<char> out_vec;
    llvm::raw_svector_ostream out_stream{out_vec};

public:
    Compiler();

    void *operator()(llvm::Module &mod);
};

class Translator {
    llvm::LLVMContext context{};
    llvm::IRBuilder<> builder{context};

    SymbolTable func_type_table{};

    llvm::Type *ctype_data{llvm::Type::getIntNTy(context, CHAR_BIT)};
    llvm::PointerType *ctype_ptr{ctype_data->getPointerTo()};
    llvm::Type *ctype_ptrdiff{llvm::Type::getIntNTy(context, CHAR_BIT * sizeof(std::ptrdiff_t))};
    llvm::Type *ctype_objref{llvm::Type::getScalarTy<decltype(PyObject::ob_refcnt)>(context)};
    llvm::Value *cvalue_objref_1{llvm::ConstantInt::get(ctype_objref, 1)};

    llvm::Module mod{"", context};

    llvm::Function *func{llvm::Function::Create(
            llvm::FunctionType::get(ctype_ptr, {
                    ctype_ptr,
                    ctype_ptr->getPointerTo()
            }, false),
            llvm::Function::ExternalLinkage,
            "",
            &mod
    )};

    PyInstr *py_instructions{};

    unsigned block_num{};
    DynamicArray<unsigned> boundaries{};
    DynamicArray<llvm::BasicBlock *> ir_blocks{};

    llvm::Value *py_symbol_table{func->getArg(0)};
    llvm::Value *py_fast_locals{func->getArg(1)};
    llvm::Value *py_stack{};
    std::ptrdiff_t stack_height{};

    auto getPointer(llvm::Value *base, ptrdiff_t offset) {
        auto offset_value = llvm::ConstantInt::get(ctype_ptrdiff, offset);
        return builder.CreateInBoundsGEP(ctype_data, base, offset_value);
    }

    auto getPointer(llvm::Value *base, ptrdiff_t offset, llvm::Type *cast_to) {
        return builder.CreatePointerCast(getPointer(base, offset), cast_to);
    }

    template <typename T, typename M>
    auto getMemberPointer(M T::* member_pointer, llvm::Value *instance) {
        T dummy;
        return getPointer(instance, calcDistance(dummy, dummy.*member_pointer));
    }

    auto readData(llvm::Value *base, ptrdiff_t offset, llvm::Type *data_type) {
        auto p = builder.CreatePointerCast(getPointer(base, offset), data_type->getPointerTo());
        return builder.CreateLoad(data_type, p);
    }

    void parseCFG(PyCodeObject *cpy_ir);
    void emitBlock(unsigned index);
    llvm::Value *do_GETLOCAL(PyOparg oparg);
    void do_SETLOCAL(PyOparg oparg, llvm::Value *value);
    llvm::Value *do_POP();
    void do_PUSH(llvm::Value *value);
    void do_Py_INCREF(llvm::Value *v);
    void do_Py_DECREF(llvm::Value *v);

    template <typename RetT, typename ...ArgTs, typename ...Args>
    std::enable_if_t<std::conjunction<std::is_same<llvm::Value *, Args>...>::value, llvm::CallInst *>
    do_Call(SymbolEntry<RetT, ArgTs...> SymbolTable::* entry, Args... args);


public:
    Translator() {
        func->setAttributes(
                llvm::AttributeList::get(context, llvm::AttributeList::FunctionIndex, llvm::AttrBuilder()
                        .addAttribute(llvm::Attribute::NoUnwind)
                        .addAttribute("tune-cpu", llvm::sys::getHostCPUName())));
        func_type_table = global_symbol_table;
    }

    void *operator()(Compiler &compiler, PyCodeObject *cpy_ir);
};

class PyInstrIter {
    const PyInstr *base;
    const size_t size;
    size_t offset{0};

    class IterResult {
        const bool has_next{false};
    public:
        const PyOpcode opcode{};
        const PyOparg oparg{};

        IterResult(PyOpcode op, PyOparg arg) : has_next{true}, opcode{op}, oparg{arg} {}

        IterResult() = default;

        explicit operator bool() const {
            return has_next;
        }
    };

public:
    PyInstrIter(const PyInstr *base_, size_t size_) : base(base_), size(size_) {}

    [[nodiscard]] auto getOffset() const { return offset; }

    IterResult next() {
        PyOpcode opcode;
        PyOparg oparg = 0;
        if (offset == size) {
            return {};
        }
        do {
            assert(offset < size);
            opcode = _Py_OPCODE(base[offset]);
            oparg = oparg << EXTENDED_ARG_BITS | _Py_OPARG(base[offset]);
            offset++;
        } while (opcode == EXTENDED_ARG);
        return {opcode, oparg};
    }
};


class BitArray : public DynamicArray<unsigned long> {
public:
    static constexpr auto BitsPerValue = CHAR_BIT * sizeof(ValueType);

    static auto chunkNumber(size_t size) { return size / BitsPerValue + !!(size % BitsPerValue); };

    explicit BitArray(size_t size) : DynamicArray<ValueType>(chunkNumber(size), true) {}

    void set(size_t index, bool value = true) {
        (*this)[index / BitsPerValue] |= ValueType{value} << index % BitsPerValue;
    }

    bool get(size_t index) {
        return (*this)[index / BitsPerValue] & (ValueType{1} << index % BitsPerValue);
    }

    [[nodiscard]] auto count(size_t size) const {
        size_t counted = 0;
        for (auto i = chunkNumber(size); i--;) {
            counted += std::bitset<BitsPerValue>((*this)[i]).count();
        }
        return counted;
    }
};
