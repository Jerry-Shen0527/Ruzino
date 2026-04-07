// Call Python import, see what happened during the importing with VS debugger

#include <rzpython/rzpython.hpp>

using namespace Ruzino;
int main()
{
    python::initialize();
    python::call<void>("import hd_RUZINO_py");
    python::finalize();
}
