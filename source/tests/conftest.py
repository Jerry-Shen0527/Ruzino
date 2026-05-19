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
binary_dir = os.path.join(project_root, "Binaries", "Release")
binary_dir = os.path.abspath(binary_dir)

os.environ["PXR_USD_WINDOWS_DLL_PATH"] = binary_dir

sys.path.insert(0, binary_dir)

rznode_python = os.path.join(project_root, "source", "Core", "rznode", "python")
if os.path.exists(rznode_python):
    sys.path.insert(0, rznode_python)

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
