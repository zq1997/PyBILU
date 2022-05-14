#ifndef PYNIC_GENERAL_UTILITIES
#define PYNIC_GENERAL_UTILITIES

#include <exception>
#include <string>
#include <type_traits>

#include <Python.h>

struct PyObjectRef {
    PyObject *o;

    PyObjectRef(const PyObjectRef &) = delete;

    // PyObjectRef(PyObjectRef &&other) : o{other.o} { other.o = nullptr; };

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
    return PyObjectRef{PyObject_Vectorcall(py_callee, &py_args[1],
            sizeof...(args) | PY_VECTORCALL_ARGUMENTS_OFFSET, nullptr)};
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

class BitArray : public DynamicArray<unsigned long> {
public:
    static constexpr auto BitsPerValue = CHAR_BIT * sizeof(ValueType);

    static auto chunkNumber(size_t size) { return size / BitsPerValue + !!(size % BitsPerValue); }

    explicit BitArray(size_t size) : DynamicArray<ValueType>(chunkNumber(size), true) {}

    bool set(size_t index) {
        auto old = get(index);
        (*this)[index / BitsPerValue] |= ValueType{1} << index % BitsPerValue;
        return !old;
    }

    bool get(size_t index) {
        return (*this)[index / BitsPerValue] & (ValueType{1} << index % BitsPerValue);
    }
};

#endif
