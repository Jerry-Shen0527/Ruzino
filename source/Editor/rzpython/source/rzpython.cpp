#include <GUI/window.h>
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <object.h>
#include <pytypedefs.h>
#include <spdlog/spdlog.h>
#include <unicodeobject.h>

#include <filesystem>
#include <rzpython/rzpython.hpp>
#include <stdexcept>
#include <unordered_map>

namespace nb = nanobind;

RUZINO_NAMESPACE_OPEN_SCOPE

namespace python {

// Global variables - accessible from template implementations
PyObject* main_module = nullptr;
PyObject* main_dict = nullptr;
bool initialized = false;
std::unordered_map<std::string, nb::object> bound_objects;

void initialize()
{
    std::filesystem::path executable_path;
#ifdef _WIN32
    char p[MAX_PATH];
    GetModuleFileNameA(NULL, p, MAX_PATH);
    executable_path = std::filesystem::path(p).parent_path();
#else
    char p[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", p, PATH_MAX);
    if (count != -1) {
        p[count] = '\0';
        executable_path = std::filesystem::path(p).parent_path();
    }
    else {
        throw std::runtime_error("Failed to get executable path.");
    }
#endif

    if (initialized) {
        return;
    }

    // Add path to ensure Python finds our modules
    Py_Initialize();
    if (!Py_IsInitialized()) {
        throw std::runtime_error("Failed to initialize Python interpreter");
    }

    // Simple initialization marker without problematic import hooks
    try {
        PyRun_SimpleString(
            "import sys\n"
            "sys._rzpython_initialized = True\n");
    }
    catch (...) {
        // Ignore setup errors
    }

    // Setup USD DLL path for Windows to resolve Boost.Python import issues
    try {
        PyObject* os = PyImport_ImportModule("os");
        PyObject* sys = PyImport_ImportModule("sys");
        PyObject* sys_path = PyObject_GetAttrString(sys, "path");
        PyObject* curDir = PyUnicode_FromString(executable_path.c_str());
        PyList_Append(sys_path, curDir);
        Py_DECREF(os);
        Py_DECREF(sys);
        Py_DECREF(sys_path);
        Py_DECREF(curDir);
    }
    catch (...) {
        // Ignore USD setup errors - USD might not be available
    }

    main_module = PyImport_AddModule("__main__");
    if (!main_module) {
        throw std::runtime_error("Failed to get __main__ module");
    }

    main_dict = PyModule_GetDict(main_module);
    if (!main_dict) {
        throw std::runtime_error("Failed to get __main__ dictionary");
    }

    initialized = true;

    // Simple and robust output capture
    python::call<void>(
        "import sys\n"
        "from io import StringIO\n"
        "_console_stdout = StringIO()\n"
        "_console_stderr = StringIO()\n"
        "_original_stdout = sys.stdout\n"
        "_original_stderr = sys.stderr\n"
        "sys.stdout = _console_stdout\n"
        "sys.stderr = _console_stderr\n");
}

void flush_python_output()
{
    if (!initialized) {
        return;
    }

    try {
        // Get stdout content
        std::string stdout_code =
            "_console_stdout.getvalue() if '_console_stdout' in dir() else ''";
        PyObject* stdout_result = PyRun_String(
            stdout_code.c_str(), Py_eval_input, main_dict, main_dict);

        if (stdout_result && PyUnicode_Check(stdout_result)) {
            const char* stdout_str = PyUnicode_AsUTF8(stdout_result);
            if (stdout_str && strlen(stdout_str) > 0) {
                spdlog::info("[Python stdout] {}", stdout_str);
                // Clear the buffer
                python::call<void>(
                    "_console_stdout.truncate(0); _console_stdout.seek(0)");
            }
            Py_DECREF(stdout_result);
        }

        // Get stderr content
        std::string stderr_code =
            "_console_stderr.getvalue() if '_console_stderr' in dir() else ''";
        PyObject* stderr_result = PyRun_String(
            stderr_code.c_str(), Py_eval_input, main_dict, main_dict);

        if (stderr_result && PyUnicode_Check(stderr_result)) {
            const char* stderr_str = PyUnicode_AsUTF8(stderr_result);
            if (stderr_str && strlen(stderr_str) > 0) {
                spdlog::warn("[Python stderr] {}", stderr_str);
                // Clear the buffer
                python::call<void>(
                    "_console_stderr.truncate(0); _console_stderr.seek(0)");
            }
            Py_DECREF(stderr_result);
        }
    }
    catch (...) {
        // Silently ignore errors in output flushing
    }
}

void finalize()
{
    if (!initialized) {
        return;
    }

    // Clear our bound objects
    try {
        bound_objects.clear();
    }
    catch (...) {
        // Ignore cleanup errors
    }

    // Reset main module references
    main_module = nullptr;
    main_dict = nullptr;

    // Finalize Python interpreter
    Py_Finalize();

    initialized = false;
}

void import(const std::string& module_name)
{
    if (!initialized) {
        throw std::runtime_error("Python interpreter not initialized");
    }

    PyObject* module = PyImport_ImportModule(module_name.c_str());
    if (!module) {
        PyErr_Print();
        throw std::runtime_error("Failed to import module: " + module_name);
    }

    // Add module to main dict so it can be accessed
    PyDict_SetItemString(main_dict, module_name.c_str(), module);
    Py_DECREF(module);
}

bool is_boost_python_module(const std::string& module_name)
{
    // Check if this is a known Boost.Python module
    static const std::vector<std::string> boost_modules = {
        "pxr", "Vt", "Gf", "Tf", "Sdf", "Usd"
    };

    for (const auto& boost_mod : boost_modules) {
        if (module_name.find(boost_mod) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void safe_import(const std::string& module_name)
{
    if (!initialized) {
        throw std::runtime_error("Python interpreter not initialized");
    }

    try {
        // For Boost.Python modules, we need special handling
        if (is_boost_python_module(module_name)) {
            // Try to import with error suppression and compatibility mode
            std::string safe_code =
                "try:\n"
                "    import " +
                module_name +
                "\n"
                "    _import_success = True\n"
                "except Exception as e:\n"
                "    print(f'Warning: Failed to import " +
                module_name +
                ": {e}')\n"
                "    _import_success = False\n";

            PyObject* result = PyRun_String(
                safe_code.c_str(), Py_file_input, main_dict, main_dict);
            if (!result) {
                PyErr_Print();
                throw std::runtime_error(
                    "Failed to safely import module: " + module_name);
            }
            Py_DECREF(result);

            // Check if import was successful
            PyObject* success =
                PyDict_GetItemString(main_dict, "_import_success");
            if (!success || !PyObject_IsTrue(success)) {
                throw std::runtime_error(
                    "Module import failed: " + module_name);
            }
        }
        else {
            // Use regular import for non-Boost.Python modules
            import(module_name);
        }
    }
    catch (const std::exception& e) {
        throw std::runtime_error(
            "Safe import failed for " + module_name + ": " + e.what());
    }
}

// Internal helper for raw Python object return
PyObject* call_raw(const std::string& code)
{
    if (!initialized) {
        throw std::runtime_error("Python interpreter not initialized");
    }

    PyObject* result =
        PyRun_String(code.c_str(), Py_eval_input, main_dict, main_dict);
    if (!result) {
        PyErr_Print();
        throw std::runtime_error("Failed to execute Python code: " + code);
    }

    return result;  // Caller is responsible for DECREF
}

// Only keep specializations for void (which needs different PyRun_String mode)
// and primitive types that need special handling
template<>
void call<void>(const std::string& code)
{
    if (!initialized) {
        throw std::runtime_error("Python interpreter not initialized");
    }

    PyObject* result =
        PyRun_String(code.c_str(), Py_file_input, main_dict, main_dict);
    if (!result) {
        PyErr_Print();
        throw std::runtime_error("Failed to execute Python code: " + code);
    }

    Py_DECREF(result);
}

}  // namespace python

RUZINO_NAMESPACE_CLOSE_SCOPE
