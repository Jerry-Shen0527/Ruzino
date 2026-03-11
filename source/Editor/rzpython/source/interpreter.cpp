#include <rzconsole/ConsoleObjects.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <rzpython/interpreter.hpp>
#include <rzpython/rzpython.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

#include "rzconsole/string_utils.h"

RUZINO_NAMESPACE_OPEN_SCOPE
namespace python {

namespace {
    std::filesystem::path get_scripts_dir()
    {
        static std::filesystem::path cached_path;
        static bool initialized = false;

        if (initialized) {
            return cached_path;
        }
        initialized = true;

        std::filesystem::path exe_dir;

#ifdef _WIN32
        char path_buf[MAX_PATH];
        DWORD len = GetModuleFileNameA(NULL, path_buf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            exe_dir = std::filesystem::path(path_buf).parent_path();
        }
#else
        exe_dir = std::filesystem::current_path();
#endif

        std::vector<std::filesystem::path> search_paths = {
            exe_dir / "scripts",
            exe_dir / ".." / ".." / "source" / "Plugins" / "scripts",
        };

        for (const auto& p : search_paths) {
            if (std::filesystem::exists(p) &&
                std::filesystem::is_directory(p)) {
                cached_path = std::filesystem::canonical(p);
                spdlog::debug("Scripts directory: {}", cached_path.string());
                return cached_path;
            }
        }

        cached_path = "scripts";
        return cached_path;
    }
}  // namespace

PythonInterpreter::PythonInterpreter() : python_initialized_(false)
{
    try {
        initialize();
        python_initialized_ = true;

        // Register Python-specific commands
        console::CommandDesc python_cmd = {
            "python",
            "Execute Python code interactively",
            [this](console::Command::Args const& args)
                -> console::Command::Result {
                if (args.size() < 2) {
                    return { false, "Usage: python <code>\n" };
                }

                std::string code;
                for (size_t i = 1; i < args.size(); ++i) {
                    if (i > 1)
                        code += " ";
                    code += args[i];
                }

                auto result = ExecutePythonCode(code);
                return { result.status, result.output };
            }
        };
        console::RegisterCommand(python_cmd);

        console::CommandDesc pyexec_cmd = {
            "exec",
            "Execute Python file",
            [this](console::Command::Args const& args)
                -> console::Command::Result {
                if (args.size() != 2) {
                    return { false, "Usage: exec <filename>\n" };
                }

                std::string filename = args[1];
                std::filesystem::path script_path(filename);

                // If path is relative, search in default scripts directory
                if (script_path.is_relative()) {
                    std::filesystem::path scripts_dir = get_scripts_dir();
                    std::filesystem::path full_path = scripts_dir / script_path;
                    if (std::filesystem::exists(full_path)) {
                        script_path = full_path;
                    }
                }

                // Check if file exists
                if (!std::filesystem::exists(script_path)) {
                    std::string msg =
                        "File not found: " + script_path.string() + "\n";
                    spdlog::error("{}", msg);
                    return { false, msg };
                }

                spdlog::info(
                    "Executing Python script: {}", script_path.string());

                // Wrap script execution with print statements for feedback
                std::string code =
                    "print('\\n=== Starting script execution ===')\n"
                    "exec(open('" +
                    script_path.string() +
                    "').read())\n"
                    "print('\\n=== Script execution completed ===')\n";

                auto result = ExecutePythonCode(code);

                // Always return output with status
                std::string output = result.output;
                if (output.empty()) {
                    output = "Script executed (no output produced)\n";
                }

                spdlog::info(
                    "Script result - Status: {}, Output: {}",
                    result.status,
                    result.output);
                return { result.status, output };
            }
        };
        console::RegisterCommand(pyexec_cmd);
    }
    catch (const std::exception& e) {
        spdlog::error("Failed to initialize Python interpreter: {}", e.what());
    }
}

PythonInterpreter::~PythonInterpreter()
{
    if (python_initialized_) {
        try {
            python::finalize();
        }
        catch (...) {
            // Ignore cleanup errors
        }
    }
}

bool PythonInterpreter::ShouldHandleCommand(std::string_view command) const
{
    // Only handle as Python if interpreter is initialized
    if (!python_initialized_) {
        return false;
    }

    // Parse the command to get the first token
    auto tokens = ds::split(command);
    if (tokens.empty()) {
        return false;
    }

    std::string first_token(tokens[0]);

    // Don't handle if it's a registered console command
    if (console::FindCommand(first_token)) {
        return false;  // Let console handle it
    }

    // Don't handle our Python-specific commands (they're console commands now)
    if (first_token == "python" || first_token == "exec") {
        return false;  // Let console handle it
    }

    // Don't handle 'help' command
    if (first_token == "help") {
        return false;
    }

    // Handle everything else as Python code
    return true;
}

PythonInterpreter::Result PythonInterpreter::HandleDirectExecution(
    std::string_view cmdline)
{
    if (!python_initialized_) {
        return { false, "Python interpreter not initialized" };
    }

    return ExecutePythonCode(cmdline);
}

PythonInterpreter::Result PythonInterpreter::Execute(
    std::string_view const cmdline)
{
    // First try Python execution for Python-like code
    if (ShouldHandleCommand(cmdline)) {
        return HandleDirectExecution(cmdline);
    }

    // Fall back to base interpreter for console commands
    return console::Interpreter::Execute(cmdline);
}

std::vector<std::string> PythonInterpreter::Suggest(
    std::string_view const cmdline,
    size_t cursor_pos)
{
    std::vector<std::string> ret;

    // Check if this is a console command (python or exec)
    auto tokens = ds::split(cmdline);
    bool is_console_command =
        !tokens.empty() && (tokens[0] == "python" || tokens[0] == "exec" ||
                            console::FindCommand(tokens[0]));

    // Only use Python completion for non-console commands
    if (!is_console_command && python_initialized_) {
        ret = SuggestPythonCompletion(cmdline);
    }

    // Always get console suggestions (will route to SuggestCommand for exec)
    auto cons = console::Interpreter::Suggest(cmdline, cursor_pos);
    ret.insert(ret.end(), cons.begin(), cons.end());
    return ret;
}

PythonInterpreter::Result PythonInterpreter::ExecuteCommand(
    std::string_view command,
    const std::vector<std::string>& args)
{
    return console::Interpreter::ExecuteCommand(command, args);
}

std::vector<std::string> PythonInterpreter::SuggestCommand(
    std::string_view command,
    std::string_view cmdline,
    size_t cursor_pos)
{
    if (command == "python") {
        return SuggestPythonCompletion(cmdline);
    }

    if (command == "exec") {
        return SuggestExecCompletion(cmdline);
    }

    return console::Interpreter::SuggestCommand(command, cmdline, cursor_pos);
}

bool PythonInterpreter::IsValidCommand(std::string_view command) const
{
    return command != "python" && command != "exec" &&
           console::Interpreter::IsValidCommand(command);
}

PythonInterpreter::Result PythonInterpreter::ExecutePythonCode(
    std::string_view code)
{
    if (!python_initialized_) {
        return { false, "Python interpreter not initialized\n" };
    }

    try {
        std::string code_str(code);

        std::string captured_output;
        std::string error_output;

        try {
            // Redirect stdout/stderr to our capture buffers before execution
            python::call<void>(
                "sys.stdout = _console_stdout\n"
                "sys.stderr = _console_stderr\n");

            // First try as expression
            PyObject* result = PyRun_String(
                code_str.c_str(),
                Py_eval_input,
                python::main_dict,
                python::main_dict);

            if (result) {
                // If it's not None, print the result
                if (result != Py_None) {
                    PyObject* repr_result = PyObject_Repr(result);
                    if (repr_result) {
                        const char* repr_str = PyUnicode_AsUTF8(repr_result);
                        if (repr_str) {
                            captured_output = std::string(repr_str) + "\n";
                        }
                        Py_DECREF(repr_result);
                    }
                }
                Py_DECREF(result);
            }
            else {
                // Clear the error and try as statement
                PyErr_Clear();

                PyObject* stmt_result = PyRun_String(
                    code_str.c_str(),
                    Py_file_input,
                    python::main_dict,
                    python::main_dict);

                if (stmt_result) {
                    // Statement executed successfully
                    Py_DECREF(stmt_result);
                }
                else {
                    // Get the error
                    if (PyErr_Occurred()) {
                        PyErr_Print();  // This will print to our captured
                                        // stderr
                    }
                }
            }

            // Get captured output
            bool has_error = false;
            try {
                std::string stdout_content =
                    python::call<std::string>("_console_stdout.getvalue()");
                std::string stderr_content =
                    python::call<std::string>("_console_stderr.getvalue()");

                // Check if there were any errors
                has_error = !stderr_content.empty();

                // Combine stdout and stderr
                if (!stdout_content.empty()) {
                    captured_output += stdout_content;
                }
                if (!stderr_content.empty()) {
                    if (!captured_output.empty() &&
                        captured_output.back() != '\n') {
                        captured_output += "\n";
                    }
                    captured_output += stderr_content;
                }

                // Clear the buffers for next execution
                python::call<void>(
                    "_console_stdout.truncate(0)\n"
                    "_console_stdout.seek(0)\n"
                    "_console_stderr.truncate(0)\n"
                    "_console_stderr.seek(0)\n");
            }
            catch (const std::exception& e) {
                captured_output +=
                    "Error getting output: " + std::string(e.what()) + "\n";
                has_error = true;
            }

            // Restore stdout/stderr
            python::call<void>(
                "sys.stdout = _original_stdout\n"
                "sys.stderr = _original_stderr\n");

            return { !has_error, captured_output };
        }
        catch (const std::exception& e) {
            // Make sure to restore stdout/stderr
            try {
                python::call<void>(
                    "sys.stdout = _original_stdout\n"
                    "sys.stderr = _original_stderr\n");
            }
            catch (...) {
            }

            return { false,
                     std::string("Python execution error: ") + e.what() +
                         "\n" };
        }
    }
    catch (const std::exception& e) {
        return { false, std::string("Python error: ") + e.what() + "\n" };
    }
}

std::vector<std::string> PythonInterpreter::SuggestPythonCompletion(
    std::string_view code)
{
    if (!python_initialized_) {
        return {};
    }

    std::string code_str(code);

    try {
        // Send the code string to Python
        python::send("_completion_text", code_str);

        // Execute completion logic step by step to avoid raw string issues
        python::call<void>("import sys");
        python::call<void>("import os");
        python::call<void>("completions = []");

        // Try jedi
        try {
            python::call<void>("import jedi");
            python::call<void>("cwd = os.getcwd()");
            python::call<void>(
                "if cwd not in sys.path: sys.path.insert(0, cwd)");
            python::call<void>(
                "project = jedi.Project(path=cwd, added_sys_path=[cwd])");
            python::call<void>(
                "interpreter = jedi.Interpreter(_completion_text, "
                "namespaces=[globals()], project=project)");
            python::call<void>("jedi_completions = interpreter.complete()");
            python::call<void>(
                "completions = [c.name for c in jedi_completions]");

            spdlog::debug("Jedi completion executed successfully");
        }
        catch (const std::exception& jedi_error) {
            spdlog::debug(
                "Jedi failed: {}, trying rlcompleter", jedi_error.what());

            // Fallback to rlcompleter
            try {
                python::call<void>("import rlcompleter");
                python::call<void>(
                    "completer = rlcompleter.Completer(globals())");
                python::call<void>("i = 0");
                python::call<void>(R"(
while i < 100:
    completion = completer.complete(_completion_text, i)
    if completion is None:
        break
    completions.append(completion)
    i += 1
)");
            }
            catch (...) {
                spdlog::debug("rlcompleter also failed");
            }
        }

        // If still no completions, try simple matching
        python::call<void>(R"(
if not completions:
    import keyword
    import re
    text = _completion_text
    match = re.search(r'\w+$', text)
    if match:
        prefix = match.group(0)
        completions.extend([kw for kw in keyword.kwlist if kw.startswith(prefix)])
        completions.extend([name for name in dir(__builtins__) if name.startswith(prefix)])
        completions.extend([name for name in globals().keys() if name.startswith(prefix) and not name.startswith('_')])
)");

        // Sort and deduplicate
        python::call<void>("completions = sorted(list(set(completions)))");

        // Filter out problematic names like WindowsError (Python 2 legacy)
        python::call<void>(R"(
completions = [c for c in completions if c not in ['WindowsError', 'WindowsPath']]
)");

        // Filter out double underscore private members unless user typed __
        python::send("_user_input", code_str);
        python::call<void>(R"(
if not _user_input.lstrip().endswith('__'):
    completions = [c for c in completions if not c.startswith('__')]
)");

        auto suggestions =
            python::call<std::vector<std::string>>("completions");
        return suggestions;
    }
    catch (const std::exception& e) {
        spdlog::debug(
            "Python completion error (jedi/rlcompleter): {}", e.what());
        // Final fallback: simple prefix matching on globals
        try {
            auto globals = python::call<std::vector<std::string>>(
                "list(globals().keys())");

            std::vector<std::string> suggestions;
            std::string prefix;

            // Extract the last word as prefix
            auto pos = code_str.find_last_of(" \t\n()[]{}.,");
            if (pos != std::string::npos) {
                prefix = code_str.substr(pos + 1);
            }
            else {
                prefix = code_str;
            }

            for (const auto& name : globals) {
                if (!prefix.empty() && name.size() >= prefix.size() &&
                    name.substr(0, prefix.size()) == prefix) {
                    suggestions.push_back(name);
                }
            }

            return suggestions;
        }
        catch (...) {
            return {};
        }
    }
}

std::vector<std::string> PythonInterpreter::SuggestExecCompletion(
    std::string_view cmdline)
{
    std::vector<std::string> suggestions;

    // Extract the prefix after "exec " command
    std::string cmd_str(cmdline);
    size_t exec_pos = cmd_str.find("exec");
    if (exec_pos == std::string::npos) {
        return suggestions;
    }

    // Find the start of the filename argument (skip "exec" and whitespace)
    size_t arg_start = exec_pos + 4;
    while (arg_start < cmd_str.length() &&
           (cmd_str[arg_start] == ' ' || cmd_str[arg_start] == '\t')) {
        arg_start++;
    }

    std::string prefix = cmd_str.substr(arg_start);

    // Get default scripts directory
    std::filesystem::path scripts_dir = get_scripts_dir();

    try {
        if (std::filesystem::exists(scripts_dir) &&
            std::filesystem::is_directory(scripts_dir)) {
            for (const auto& entry :
                 std::filesystem::directory_iterator(scripts_dir)) {
                if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename().string();

                    // Filter by prefix
                    if (prefix.empty() ||
                        (filename.size() >= prefix.size() &&
                         filename.substr(0, prefix.size()) == prefix)) {
                        suggestions.push_back(filename);
                    }
                }
            }
        }
    }
    catch (const std::exception& e) {
        spdlog::debug("Error scanning scripts directory: {}", e.what());
    }

    // Sort suggestions
    std::sort(suggestions.begin(), suggestions.end());

    return suggestions;
}

std::shared_ptr<console::Interpreter> CreatePythonInterpreter()
{
    return std::make_shared<PythonInterpreter>();
}

}  // namespace python

RUZINO_NAMESPACE_CLOSE_SCOPE
