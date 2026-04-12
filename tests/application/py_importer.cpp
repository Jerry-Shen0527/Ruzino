// Call Python import, see what happened during the importing with VS debugger

#include <spdlog/spdlog.h>

#include "nodes/core/logging.hpp"
#include <rzpython/rzpython.hpp>

using namespace Ruzino;
int main()
{
    initialize_framework_logging("py_importer", spdlog::level::info);

    try {
        python::initialize();
        python::call<void>("import hd_RUZINO_py");
        python::finalize();
    }
    catch (const std::exception& e) {
        log_exception_with_context("py_importer failed", e);
        return 1;
    }
    catch (...) {
        log_current_exception_with_context("py_importer failed");
        return 1;
    }

    return 0;
}
