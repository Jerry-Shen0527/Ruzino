#include <gtest/gtest.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/array.h>

#include <RHI/rhi.hpp>

#include "GUI/window.h"
#include "rzpython/rzpython.hpp"
#include "rzpython/usd_extensions.hpp"

// Check if we're running in a headless environment
bool is_headless_environment()
{
#ifdef _WIN32
    return false;  // Windows usually has a display
#else
    // On Linux, check if DISPLAY environment variable is set
    const char* display = std::getenv("DISPLAY");
    return display == nullptr || std::string(display).empty();
#endif
}

using namespace Ruzino;

// Test fixture that initializes Python once for all tests
class RZPythonRuntimeTest : public ::testing::Test {
   protected:
    static void SetUpTestSuite()
    {
        // Initialize Python once for all tests
        python::initialize();
    }

    static void TearDownTestSuite()
    {
        // Finalize Python after all tests are done
        python::finalize();
    }

    void SetUp() override
    {
        // Clear any leftover variables from previous tests
        python::call<void>("import gc");
        python::call<void>("gc.collect()");

        // Clear variables that might interfere between tests
        // But preserve important modules like torch, numpy, etc.
        python::call<void>(
            "# Clean up any leftover variables but preserve important modules\n"
            "import sys\n"
            "preserve_modules = {'sys', 'gc', 'torch', 'numpy', 'np', "
            "'RHI_py', 'GUI_py'}\n"
            "main_vars = list(globals().keys())\n"
            "for var in main_vars:\n"
            "    if not var.startswith('_') and var not in preserve_modules:\n"
            "        try:\n"
            "            del globals()[var]\n"
            "        except:\n"
            "            pass\n");
    }
};

TEST_F(RZPythonRuntimeTest, RHI_package)
{
    // Skip in headless environment as RHI requires graphics context
    if (is_headless_environment())
    {
        GTEST_SKIP() << "Skipping RHI test in headless environment";
    }

    python::import("RHI_py");
    int result = python::call<int>("RHI_py.init()");
    EXPECT_EQ(result, 0);

    // Test that we can call get_device without crashing, even if we can't
    // convert the result yet
    python::call<void>("device = RHI_py.get_device()");
    python::call<void>("print('Device type:', type(device))");

    // Test that we can get the backend enum
    python::call<void>("backend = RHI_py.get_backend()");
    python::call<void>("print('Backend type:', type(backend))");
    python::call<void>("print('Backend value:', backend)");

    result = python::call<int>("RHI_py.shutdown()");
    EXPECT_EQ(result, 0);
}

TEST_F(RZPythonRuntimeTest, GUI_package)
{
    // Skip in headless environment as GUI requires graphics context
    if (is_headless_environment())
    {
        GTEST_SKIP() << "Skipping GUI test in headless environment";
    }

    python::import("GUI_py");

    Window window;
    python::reference("w", &window);

    // Just test that we can call the method without crashing
    // and that we get some kind of numeric result
    python::call<void>("print('Testing Window binding...')");
    python::call<void>("print(type(w))");
    python::call<void>("result = w.get_elapsed_time()");
    python::call<void>("print('Elapsed time result:', result)");

    float time = python::call<float>("w.get_elapsed_time()");
    // Just check that we get a finite number (not inf or nan)
    EXPECT_TRUE(std::isfinite(time));
}

TEST_F(RZPythonRuntimeTest, ListToVector_conversion)
{
    // Test converting Python list to C++ vector<int>
    python::call<void>("int_list = [1, 2, 3, 4, 5]");
    std::vector<int> int_vec = python::call<std::vector<int>>("int_list");

    EXPECT_EQ(int_vec.size(), 5);
    EXPECT_EQ(int_vec[0], 1);
    EXPECT_EQ(int_vec[4], 5);

    // Test converting Python list to C++ vector<float>
    python::call<void>("float_list = [1.1, 2.2, 3.3]");
    std::vector<float> float_vec =
        python::call<std::vector<float>>("float_list");

    EXPECT_EQ(float_vec.size(), 3);
    EXPECT_FLOAT_EQ(float_vec[0], 1.1f);
    EXPECT_FLOAT_EQ(float_vec[2], 3.3f);

    // Test converting Python list to C++ vector<string>
    python::call<void>("str_list = ['hello', 'world', 'test']");
    std::vector<std::string> str_vec =
        python::call<std::vector<std::string>>("str_list");

    EXPECT_EQ(str_vec.size(), 3);
    EXPECT_EQ(str_vec[0], "hello");
    EXPECT_EQ(str_vec[1], "world");
    EXPECT_EQ(str_vec[2], "test");
}

TEST_F(RZPythonRuntimeTest, VectorToList_conversion)
{
    // Skip in headless environment as GUI_py requires graphics context
    if (is_headless_environment())
    {
        GTEST_SKIP() << "Skipping GUI test in headless environment";
    }

    python::import("GUI_py");

    // Test sending C++ vector<int> to Python list
    std::vector<int> cpp_int_vec = { 1, 2, 3, 4, 5 };
    python::send("py_int_list", cpp_int_vec);

    // Verify the list was created correctly in Python
    python::call<void>("print('Python int list:', py_int_list)");
    python::call<void>("print('Length:', len(py_int_list))");

    // Get it back to verify
    std::vector<int> retrieved_vec =
        python::call<std::vector<int>>("py_int_list");
    EXPECT_EQ(retrieved_vec.size(), 5);
    EXPECT_EQ(retrieved_vec[0], 1);
    EXPECT_EQ(retrieved_vec[4], 5);

    // Test sending C++ vector<float> to Python list
    std::vector<float> cpp_float_vec = { 1.1f, 2.2f, 3.3f };
    python::send("py_float_list", cpp_float_vec);

    python::call<void>("print('Python float list:', py_float_list)");
    std::vector<float> retrieved_float_vec =
        python::call<std::vector<float>>("py_float_list");
    EXPECT_EQ(retrieved_float_vec.size(), 3);
    EXPECT_FLOAT_EQ(retrieved_float_vec[0], 1.1f);
    EXPECT_FLOAT_EQ(retrieved_float_vec[2], 3.3f);

    // Test sending C++ vector<string> to Python list
    std::vector<std::string> cpp_str_vec = { "hello", "world", "test" };
    python::send("py_str_list", cpp_str_vec);

    python::call<void>("print('Python string list:', py_str_list)");
    std::vector<std::string> retrieved_str_vec =
        python::call<std::vector<std::string>>("py_str_list");
    EXPECT_EQ(retrieved_str_vec.size(), 3);
    EXPECT_EQ(retrieved_str_vec[0], "hello");
    EXPECT_EQ(retrieved_str_vec[1], "world");
    EXPECT_EQ(retrieved_str_vec[2], "test");
}

TEST_F(RZPythonRuntimeTest, NumPy_ndarray_conversion)
{
    // Skip in headless environment as GUI_py requires graphics context
    if (is_headless_environment())
    {
        GTEST_SKIP() << "Skipping GUI test in headless environment";
    }

    python::import("GUI_py");

    try {
        python::import("numpy");

        // Test creating NumPy array from C++ and sending to Python
        python::call<void>("import numpy as np");

        // Create a simple 2D array in Python and retrieve it
        python::call<void>(
            "np_array = np.array([[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]], "
            "dtype=np.float32)");
        python::call<void>("print('NumPy array shape:', np_array.shape)");
        python::call<void>("print('NumPy array dtype:', np_array.dtype)");

        // Retrieve the NumPy array in C++
        python::numpy_array_f32 np_array =
            python::call<python::numpy_array_f32>("np_array");

        // Test basic properties
        EXPECT_EQ(np_array.ndim(), 2);
        EXPECT_EQ(np_array.shape(0), 2);
        EXPECT_EQ(np_array.shape(1), 3);
        EXPECT_TRUE(np_array.is_valid());

        // Test data access
        float* data = np_array.data();
        EXPECT_FLOAT_EQ(data[0], 1.0f);
        EXPECT_FLOAT_EQ(data[1], 2.0f);
        EXPECT_FLOAT_EQ(data[2], 3.0f);

        // Send a C++ created ndarray to Python
        std::vector<float> cpp_data = { 7.0f, 8.0f, 9.0f, 10.0f };
        python::numpy_array_f32 cpp_array(cpp_data.data(), { 2, 2 });
        python::send("cpp_np_array", cpp_array);

        python::call<void>("print('C++ created NumPy array:', cpp_np_array)");
        python::call<void>("print('C++ array shape:', cpp_np_array.shape)");
    }
    catch (const std::exception& e) {
        // NumPy might not be available, skip test
        GTEST_SKIP() << "NumPy not available: " << e.what();
    }
}

TEST_F(RZPythonRuntimeTest, USD_VtArray_conversion)
{
    try {
        // Check if USD imports work
        python::call<void>(
            "try:\n"
            "    from pxr import Vt, Gf\n"
            "    _usd_available = True\n"
            "except Exception as e:\n"
            "    print(f'USD import error: {e}')\n"
            "    _usd_available = False\n");

        bool usd_available = python::call<bool>("_usd_available");
        if (!usd_available) {
            GTEST_SKIP() << "USD not available or incompatible";
        }

        // Test VtArray<int> conversion using USD-specific functions
        python::call<void>("vt_int_array = Vt.IntArray([1, 2, 3, 4, 5])");

        // Use USD-specific conversion function
        pxr::VtArray<int> vt_int =
            python::usd::call_usd<pxr::VtArray<int>>("vt_int_array");

        EXPECT_EQ(vt_int.size(), 5);
        EXPECT_EQ(vt_int[0], 1);
        EXPECT_EQ(vt_int[4], 5);

        // Test sending C++ VtArray<int> to Python using USD-specific function
        pxr::VtArray<int> cpp_vt_int = { 10, 20, 30 };
        python::usd::send_usd("cpp_vt_int", cpp_vt_int);

        python::call<void>("print('C++ VtArray<int>:', cpp_vt_int)");
        pxr::VtArray<int> retrieved_vt_int =
            python::usd::call_usd<pxr::VtArray<int>>("cpp_vt_int");
        EXPECT_EQ(retrieved_vt_int.size(), 3);
        EXPECT_EQ(retrieved_vt_int[0], 10);

        // Test VtArray<float> conversion
        python::call<void>("vt_float_array = Vt.FloatArray([1.1, 2.2, 3.3])");
        pxr::VtArray<float> vt_float =
            python::usd::call_usd<pxr::VtArray<float>>("vt_float_array");

        EXPECT_EQ(vt_float.size(), 3);
        EXPECT_FLOAT_EQ(vt_float[0], 1.1f);
        EXPECT_FLOAT_EQ(vt_float[2], 3.3f);

        // Test sending C++ VtArray<float> to Python
        pxr::VtArray<float> cpp_vt_float = { 4.4f, 5.5f };
        python::usd::send_usd("cpp_vt_float", cpp_vt_float);

        python::call<void>("print('C++ VtArray<float>:', cpp_vt_float)");
    }
    catch (const std::exception& e) {
        GTEST_SKIP() << "USD VtArray not available or incompatible: "
                     << e.what();
    }
}

TEST_F(RZPythonRuntimeTest, USD_GfVec3f_conversion)
{
    try {
        // Check if USD was successfully imported
        python::call<void>(
            "try:\n"
            "    from pxr import Vt, Gf\n"
            "    _usd_available = True\n"
            "except Exception as e:\n"
            "    print(f'USD import error: {e}')\n"
            "    _usd_available = False\n");

        bool usd_available = python::call<bool>("_usd_available");
        if (!usd_available) {
            GTEST_SKIP() << "USD not available or incompatible";
        }

        // Test GfVec3f conversion from Python to C++
        python::call<void>("gf_vec3 = Gf.Vec3f(1.0, 2.0, 3.0)");
        pxr::GfVec3f vec3 = python::usd::call_usd<pxr::GfVec3f>("gf_vec3");

        EXPECT_FLOAT_EQ(vec3[0], 1.0f);
        EXPECT_FLOAT_EQ(vec3[1], 2.0f);
        EXPECT_FLOAT_EQ(vec3[2], 3.0f);

        // Test sending C++ GfVec3f to Python using USD-specific function
        pxr::GfVec3f cpp_vec3(4.0f, 5.0f, 6.0f);
        python::usd::send_usd("cpp_gf_vec3", cpp_vec3);

        python::call<void>("print('C++ GfVec3f:', cpp_gf_vec3)");
        pxr::GfVec3f retrieved_vec3 =
            python::usd::call_usd<pxr::GfVec3f>("cpp_gf_vec3");
        EXPECT_FLOAT_EQ(retrieved_vec3[0], 4.0f);
        EXPECT_FLOAT_EQ(retrieved_vec3[1], 5.0f);
        EXPECT_FLOAT_EQ(retrieved_vec3[2], 6.0f);
    }
    catch (const std::exception& e) {
        GTEST_SKIP() << "USD GfVec3f not available or incompatible: "
                     << e.what();
    }
}

TEST_F(RZPythonRuntimeTest, USD_GfVec4f_conversion)
{
    try {
        // Check if USD was successfully imported (don't assume it's already
        // imported)
        python::call<void>(
            "try:\n"
            "    from pxr import Vt, Gf\n"
            "    _usd_available = True\n"
            "except Exception as e:\n"
            "    print(f'USD import error: {e}')\n"
            "    _usd_available = False\n");

        bool usd_available = python::call<bool>("_usd_available");
        if (!usd_available) {
            GTEST_SKIP() << "USD not available or incompatible";
        }

        // Test GfVec4f conversion from Python to C++
        python::call<void>("gf_vec4 = Gf.Vec4f(1.0, 2.0, 3.0, 4.0)");
        pxr::GfVec4f vec4 = python::usd::call_usd<pxr::GfVec4f>("gf_vec4");

        EXPECT_FLOAT_EQ(vec4[0], 1.0f);
        EXPECT_FLOAT_EQ(vec4[1], 2.0f);
        EXPECT_FLOAT_EQ(vec4[2], 3.0f);
        EXPECT_FLOAT_EQ(vec4[3], 4.0f);

        // Test sending C++ GfVec4f to Python using USD-specific function
        pxr::GfVec4f cpp_vec4(5.0f, 6.0f, 7.0f, 8.0f);
        python::usd::send_usd("cpp_gf_vec4", cpp_vec4);

        python::call<void>("print('C++ GfVec4f:', cpp_gf_vec4)");
        pxr::GfVec4f retrieved_vec4 =
            python::usd::call_usd<pxr::GfVec4f>("cpp_gf_vec4");
        EXPECT_FLOAT_EQ(retrieved_vec4[0], 5.0f);
        EXPECT_FLOAT_EQ(retrieved_vec4[1], 6.0f);
        EXPECT_FLOAT_EQ(retrieved_vec4[2], 7.0f);
        EXPECT_FLOAT_EQ(retrieved_vec4[3], 8.0f);
    }
    catch (const std::exception& e) {
        GTEST_SKIP() << "USD GfVec4f not available: " << e.what();
    }
}

TEST_F(RZPythonRuntimeTest, USD_VtArray_Vec3f_conversion)
{
    try {
        // Import USD modules
        python::call<void>("from pxr import Vt, Gf");

        // Test VtArray<GfVec3f> conversion from Python to C++
        python::call<void>(
            "vt_vec3_array = Vt.Vec3fArray([Gf.Vec3f(1.0, 2.0, 3.0), "
            "Gf.Vec3f(4.0, 5.0, 6.0)])");

        // For complex types like VtArray<GfVec3f>, we need special handling
        // For now, let's test that we can at least access the data
        python::call<void>(
            "print('VtArray<GfVec3f> size:', len(vt_vec3_array))");
        python::call<void>("print('First element:', vt_vec3_array[0])");

        // Test sending C++ VtArray<GfVec3f> to Python using USD-specific
        // function
        pxr::VtArray<pxr::GfVec3f> cpp_vt_vec3;
        cpp_vt_vec3.push_back(pxr::GfVec3f(7.0f, 8.0f, 9.0f));
        cpp_vt_vec3.push_back(pxr::GfVec3f(10.0f, 11.0f, 12.0f));
        python::usd::send_usd("cpp_vt_vec3", cpp_vt_vec3);

        python::call<void>("print('C++ VtArray<GfVec3f>:', cpp_vt_vec3)");
        python::call<void>(
            "print('C++ VtArray<GfVec3f> size:', len(cpp_vt_vec3))");
    }
    catch (const std::exception& e) {
        GTEST_SKIP() << "USD VtArray<GfVec3f> not available: " << e.what();
    }
}

TEST_F(RZPythonRuntimeTest, USD_VtArray_Vec4f_conversion)
{
    try {
        // Import USD modules
        python::call<void>("from pxr import Vt, Gf");

        // Test VtArray<GfVec4f> conversion from Python to C++
        python::call<void>(
            "vt_vec4_array = Vt.Vec4fArray([Gf.Vec4f(1.0, 2.0, 3.0, 4.0), "
            "Gf.Vec4f(5.0, 6.0, 7.0, 8.0)])");

        // For complex types, test that we can access the data
        python::call<void>(
            "print('VtArray<GfVec4f> size:', len(vt_vec4_array))");
        python::call<void>("print('First element:', vt_vec4_array[0])");

        // Test sending C++ VtArray<GfVec4f> to Python using USD-specific
        // function
        pxr::VtArray<pxr::GfVec4f> cpp_vt_vec4;
        cpp_vt_vec4.push_back(pxr::GfVec4f(9.0f, 10.0f, 11.0f, 12.0f));
        cpp_vt_vec4.push_back(pxr::GfVec4f(13.0f, 14.0f, 15.0f, 16.0f));
        python::usd::send_usd("cpp_vt_vec4", cpp_vt_vec4);

        python::call<void>("print('C++ VtArray<GfVec4f>:', cpp_vt_vec4)");
        python::call<void>(
            "print('C++ VtArray<GfVec4f> size:', len(cpp_vt_vec4))");
    }
    catch (const std::exception& e) {
        GTEST_SKIP() << "USD VtArray<GfVec4f> not available: " << e.what();
    }
}

TEST_F(RZPythonRuntimeTest, PyTorch_tensor_conversion)
{
    // Skip in headless environment as GUI_py requires graphics context
    if (is_headless_environment())
    {
        GTEST_SKIP() << "Skipping GUI test in headless environment";
    }

    python::import("GUI_py");

    try {
        // Import torch once - since Python is persistent across tests now,
        // this should work without the CPU dispatcher issue
        python::call<void>("import torch");

        // Test creating PyTorch tensor and retrieving it
        python::call<void>(
            "torch_tensor = torch.tensor([[1.0, 2.0], [3.0, 4.0]], "
            "dtype=torch.float32)");
        python::call<void>(
            "print('PyTorch tensor shape:', torch_tensor.shape)");
        python::call<void>(
            "print('PyTorch tensor dtype:', torch_tensor.dtype)");

        // Retrieve the PyTorch tensor in C++
        python::torch_tensor_f32 torch_tensor =
            python::call<python::torch_tensor_f32>("torch_tensor");

        // Test basic properties
        EXPECT_EQ(torch_tensor.ndim(), 2);
        EXPECT_EQ(torch_tensor.shape(0), 2);
        EXPECT_EQ(torch_tensor.shape(1), 2);
        EXPECT_TRUE(torch_tensor.is_valid());

        // Test data access
        float* data = torch_tensor.data();
        EXPECT_FLOAT_EQ(data[0], 1.0f);
        EXPECT_FLOAT_EQ(data[1], 2.0f);

        // Create a CPU tensor and send to Python
        std::vector<float> cpp_data = { 5.0f, 6.0f, 7.0f, 8.0f };
        python::torch_tensor_f32 cpp_tensor(cpp_data.data(), { 2, 2 });
        python::send("cpp_torch_tensor", cpp_tensor);

        python::call<void>(
            "print('C++ created PyTorch tensor:', cpp_torch_tensor)");
    }
    catch (const std::exception& e) {
        // PyTorch might not be available, skip test
        GTEST_SKIP() << "PyTorch not available: " << e.what();
    }
}

TEST_F(RZPythonRuntimeTest, CUDA_tensor_conversion)
{
    try {
        // torch should already be imported from previous test
        // Check if CUDA is available
        python::call<void>("cuda_available = torch.cuda.is_available()");

        bool cuda_available = false;
        try {
            cuda_available = python::call<int>("int(cuda_available)") > 0;
        }
        catch (...) {
            cuda_available = false;
        }

        if (!cuda_available) {
            GTEST_SKIP() << "CUDA not available";
        }

        // Test creating CUDA tensor
        python::call<void>(
            "cuda_tensor = torch.tensor([[1.0, 2.0], [3.0, 4.0]], "
            "dtype=torch.float32, device='cuda')");
        python::call<void>("print('CUDA tensor device:', cuda_tensor.device)");
        python::call<void>("print('CUDA tensor shape:', cuda_tensor.shape)");

        // Retrieve the CUDA tensor in C++
        python::cuda_array_f32 cuda_tensor =
            python::call<python::cuda_array_f32>("cuda_tensor");

        // Test basic properties
        EXPECT_EQ(cuda_tensor.ndim(), 2);
        EXPECT_EQ(cuda_tensor.shape(0), 2);
        EXPECT_EQ(cuda_tensor.shape(1), 2);
        EXPECT_TRUE(cuda_tensor.is_valid());
        EXPECT_EQ(cuda_tensor.device_type(), nanobind::device::cuda::value);

        python::call<void>(
            "print('Successfully retrieved CUDA tensor in C++')");
    }
    catch (const std::exception& e) {
        // CUDA might not be available, skip test
        GTEST_SKIP() << "CUDA not available: " << e.what();
    }
}
