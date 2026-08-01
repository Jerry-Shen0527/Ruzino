"""
Pytest configuration for integration tests (rendering + simulation).

Sets up paths, environment variables, and working directory for
test modules that need access to Ruzino binaries and USD Python bindings.
"""

import sys
import os
import platform

tests_dir = os.path.dirname(os.path.abspath(__file__))
project_root = os.path.abspath(os.path.join(tests_dir, "..", ".."))
# Respect RZ_BUILD_TYPE so tests can load Binaries/Debug for native debugging
# (PDB symbols, cdb/VS stack traces). Defaults to Release for normal runs.
_build_type = os.environ.get("RZ_BUILD_TYPE", "Release")
binary_dir = os.path.join(project_root, "Binaries", _build_type)
binary_dir = os.path.abspath(binary_dir)
if not os.path.isdir(binary_dir):
    # Fallback to Release if the requested build type isn't built.
    binary_dir = os.path.abspath(os.path.join(project_root, "Binaries", "Release"))

# All test outputs (renders, generated usdc/py/json, diagnostics) must live
# under Binaries/Release/test_output/. Binaries/ is gitignored at repo root,
# so nothing here enters the source tree or git history. Tests import this
# constant via `from conftest import TEST_OUTPUT_DIR` or read the env var.
TEST_OUTPUT_DIR = os.path.join(binary_dir, "test_output")
os.makedirs(TEST_OUTPUT_DIR, exist_ok=True)
os.environ["RZ_TEST_OUTPUT_DIR"] = TEST_OUTPUT_DIR

os.environ["PXR_USD_WINDOWS_DLL_PATH"] = binary_dir

sys.path.insert(0, binary_dir)

# Put binary_dir on PATH and register it as a DLL search directory. Node
# plugins (e.g. GPU_sph.dll) are loaded by C++ LoadLibrary(<name>) during
# load_configuration, which only searches the process PATH / app dir — not the
# cwd that os.chdir points at below — so PATH is required for them to resolve.
os.environ["PATH"] = binary_dir + os.pathsep + os.environ.get("PATH", "")

if platform.system() == "Windows":
    os.add_dll_directory(binary_dir)
    for sub in ("SDK\\python", "SDK\\OpenUSD\\Release\\lib"):
        dll_dir = os.path.join(project_root, sub)
        if os.path.isdir(dll_dir):
            os.add_dll_directory(dll_dir)

rznode_python = os.path.join(project_root, "source", "Core", "rznode", "python")
if os.path.exists(rznode_python):
    sys.path.insert(0, rznode_python)

renderer_python = os.path.join(project_root, "source", "Runtime", "renderer", "python")
if os.path.exists(renderer_python):
    sys.path.insert(0, renderer_python)

if platform.system() != "Windows":
    usd_python_path = os.path.join(project_root, "SDK", "OpenUSD", "Debug", "lib", "python")
    usd_lib_path = os.path.join(project_root, "SDK", "OpenUSD", "Debug", "lib")
    if os.path.exists(usd_python_path):
        sys.path.insert(0, usd_python_path)
    import ctypes
    import glob
    for so_file in sorted(glob.glob(os.path.join(usd_lib_path, "*.so"))):
        try:
            ctypes.CDLL(so_file, mode=ctypes.RTLD_GLOBAL)
        except OSError:
            pass

os.chdir(binary_dir)
