#pragma once

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>

#include <string>
#include <utility>
#include <vector>

#include "api.h"

RUZINO_NAMESPACE_OPEN_SCOPE

namespace python {

// Type aliases for common ndarray configurations
using numpy_array_f32 = nanobind::ndarray<nanobind::numpy, float>;
using numpy_array_f64 = nanobind::ndarray<nanobind::numpy, double>;
using numpy_array_i32 = nanobind::ndarray<nanobind::numpy, int32_t>;
using numpy_array_i64 = nanobind::ndarray<nanobind::numpy, int64_t>;

using torch_tensor_f32 = nanobind::ndarray<nanobind::pytorch, float>;
using torch_tensor_f64 = nanobind::ndarray<nanobind::pytorch, double>;
using torch_tensor_i32 = nanobind::ndarray<nanobind::pytorch, int32_t>;
using torch_tensor_i64 = nanobind::ndarray<nanobind::pytorch, int64_t>;

// CPU ndarray types
using cpu_array_f32 = nanobind::ndarray<float, nanobind::device::cpu>;
using cpu_array_f64 = nanobind::ndarray<double, nanobind::device::cpu>;
using cpu_array_i32 = nanobind::ndarray<int32_t, nanobind::device::cpu>;
using cpu_array_i64 = nanobind::ndarray<int64_t, nanobind::device::cpu>;

// CUDA ndarray types for GPU memory
using cuda_array_f32 = nanobind::ndarray<float, nanobind::device::cuda>;
using cuda_array_f64 = nanobind::ndarray<double, nanobind::device::cuda>;
using cuda_array_i32 = nanobind::ndarray<int32_t, nanobind::device::cuda>;
using cuda_array_i64 = nanobind::ndarray<int64_t, nanobind::device::cuda>;

// Helper structure for zero-copy CUDA array transfer
template<typename T>
struct CudaArrayView {
    T* data;
    std::vector<size_t> shape;

    CudaArrayView(T* data, std::vector<size_t> shape)
        : data(data),
          shape(std::move(shape))
    {
    }

    // Convenience constructor for 1D arrays
    CudaArrayView(T* data, size_t size) : data(data), shape{ size }
    {
    }
};

// Initialize Python interpreter
RZPYTHON_API void initialize();

// Finalize Python interpreter
RZPYTHON_API void finalize();

// Import a Python module
RZPYTHON_API void import(const std::string& module_name);

// Safe import that handles Boost.Python conflicts
RZPYTHON_API void safe_import(const std::string& module_name);

// Check if module uses Boost.Python
RZPYTHON_API bool is_boost_python_module(const std::string& module_name);

// Internal helper for dynamic type conversion
RZPYTHON_API PyObject* call_raw(const std::string& code);

// Generic call function - implemented in header for template instantiation
template<typename T>
T call(const std::string& code);

// Safe call function for mixed binding environments
template<typename T>
T call_safe(const std::string& code);

// Only void needs special handling (different PyRun_String mode)
template<>
RZPYTHON_API void call<void>(const std::string& code);

// Helper function to get nanobind cast for objects
template<typename T>
void reference(const std::string& name, T* obj);

// Send C++ data to Python variable (by value)
template<typename T>
void send(const std::string& name, const T& value);

// Send C++ object to Python variable (by reference, zero-copy)
template<typename T>
void send_ref(const std::string& name, T& obj);

// Get Python variable as C++ type
template<typename T>
T get(const std::string& name);

// Flush Python's stdout/stderr and print to C++ console
RZPYTHON_API void flush_python_output();

// Get the last Python error as a string (returns empty if no error)
RZPYTHON_API std::string get_last_error();

// Execute Python code and return result or error message
RZPYTHON_API std::pair<bool, std::string> execute_with_error(
    const std::string& code);

}  // namespace python

RUZINO_NAMESPACE_CLOSE_SCOPE

// Template implementations - include after declarations
#include "rzpython_impl.hpp"
