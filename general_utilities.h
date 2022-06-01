#ifndef PYNIC_GENERAL_UTILITIES
#define PYNIC_GENERAL_UTILITIES

#include <exception>
#include <string>
#include <type_traits>

#include <Python.h>
#include <opcode.h>

struct PyObjectRef {
    PyObject *o;

    PyObjectRef(const PyObjectRef &) = delete;
    auto operator=(const PyObjectRef &) = delete;

    PyObjectRef(PyObject *o) : o{o} {
        if (!o) {
            throw std::bad_exception();
        }
    }

    ~PyObjectRef() {
        Py_DECREF(o);
    }

    operator PyObject *() const { return o; }
};

inline const char *PyStringAsString(PyObject *o) {
    auto str = PyUnicode_AsUTF8(o);
    if (!str) {
        throw std::bad_exception();
    }
    return str;
}

inline PyObjectRef callDebugHelperFunction(const char *callee_name, const auto &... args) {
    PyObjectRef py_mod = PyImport_ImportModule("debug_helper");
    PyObjectRef py_callee = PyObject_GetAttrString(py_mod, callee_name);
    PyObject *py_args[]{nullptr, args...};
    return PyObject_Vectorcall(py_callee, &py_args[1], sizeof...(args) | PY_VECTORCALL_ARGUMENTS_OFFSET, nullptr);
}

template <typename T, typename = void>
struct HasDereference : std::false_type {};
template <typename T>
struct HasDereference<T, std::void_t<decltype(*std::declval<T>())>> : std::true_type {};

template <typename Size=size_t, typename Iter=Size>
class Range {
    static_assert(std::is_integral_v<Size>);
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
            if constexpr (HasDereference<Iter>::value) {
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
    T *data{};

public:
    DynamicArray() = default;

    DynamicArray(DynamicArray &&other) noexcept: data{other.data} {
        other.data = nullptr;
    };

    explicit DynamicArray(size_t size, bool init = false) : data{init ? new T[size]{} : new T[size]} {};

    ~DynamicArray() {
        if (data) {
            delete[] data;
        }
    };

    void reserve(size_t size, bool init = false) {
        assert(!data);
        data = init ? new T[size]{} : new T[size];
    }

    T &operator[](size_t index) { return data[index]; }

    const auto &operator[](size_t index) const { return data[index]; }

    auto &operator*() const { return data[0]; }
};


template <typename T>
class LimitedStack {
    DynamicArray<T> storage_space;
    T *stack_pointer;
public:
    explicit LimitedStack(size_t size) : storage_space{size}, stack_pointer(&storage_space[0]) {}

    auto empty() {
        return stack_pointer == &storage_space[0];
    }

    void push(const T &v) {
        *stack_pointer++ = v;
    }

    auto pop() {
        assert(!empty());
        return *--stack_pointer;
    }
};

class BitArray : public DynamicArray<uintmax_t> {
    using ChunkType = uintmax_t;
    using Parent = DynamicArray<ChunkType>;
public:
    static constexpr auto BitsPerValue = CHAR_BIT * sizeof(ChunkType);

    static auto chunkNumber(size_t size) { return size / BitsPerValue + !!(size % BitsPerValue); }

    BitArray() = default;

    explicit BitArray(size_t size) : Parent(chunkNumber(size), true) {}

    auto &getChunk(size_t index) { return Parent::operator[](index); }

    void reserve(size_t size) {
        Parent::reserve(chunkNumber(size), true);
    }

    bool set(size_t index) {
        auto old = get(index);
        getChunk(index / BitsPerValue) |= ChunkType{1} << index % BitsPerValue;
        return !old;
    }

    void reset(size_t index) {
        getChunk(index / BitsPerValue) &= ~(ChunkType{1} << index % BitsPerValue);
    }

    void setIf(size_t index, bool cond) {
        getChunk(index / BitsPerValue) |= ChunkType{cond} << index % BitsPerValue;
    }

    bool get(size_t index) {
        return getChunk(index / BitsPerValue) & (ChunkType{1} << index % BitsPerValue);
    }

    void fill(size_t size, bool set = false) {
        memset(&getChunk(0), set ? UCHAR_MAX : 0, chunkNumber(size) * sizeof(ChunkType));
    }

    auto operator[](size_t index) = delete;
};


class PyInstrPointer {
    _Py_CODEUNIT *pointer;

public:
    static constexpr auto extended_arg_shift = 8;

    explicit PyInstrPointer(PyCodeObject *py_code) :
            pointer(reinterpret_cast<_Py_CODEUNIT *>(PyBytes_AS_STRING(py_code->co_code))) {}

    explicit PyInstrPointer(_Py_CODEUNIT *pointer) : pointer(pointer) {}

    auto opcode() const { return _Py_OPCODE(*pointer); }

    auto oparg() const { return _Py_OPARG(*pointer); }

    auto fullOparg(const PyInstrPointer &instr_begin) const {
        auto p = pointer;
        auto arg = _Py_OPARG(*p);
        unsigned shift = 0;
        while (p-- != instr_begin.pointer && _Py_OPCODE(*p) == EXTENDED_ARG) {
            shift += extended_arg_shift;
            arg |= _Py_OPARG(*p) << shift;
        }
        return arg;
    }

    auto operator*() const { return std::pair{opcode(), oparg()}; }

    auto &operator++() {
        ++pointer;
        return *this;
    }

    auto &operator--() {
        --pointer;
        return *this;
    }

    auto operator++(int) { return PyInstrPointer(pointer++); }

    auto operator--(int) { return PyInstrPointer(pointer--); }

    auto operator<=>(const PyInstrPointer &other) const { return pointer <=> other.pointer; }

    auto operator==(const PyInstrPointer &other) const { return pointer == other.pointer; }

    auto operator+(ptrdiff_t offset) const { return PyInstrPointer{pointer + offset}; }

    auto operator-(ptrdiff_t offset) const { return *this + (-offset); }

    auto operator-(const PyInstrPointer &other) const { return pointer - other.pointer; }

    auto operator[](ptrdiff_t offset) const { return *(*this + offset); }
};

// TODO: N-dim动态数组

#endif
