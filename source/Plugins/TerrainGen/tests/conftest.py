"""
pytest configuration for TerrainGen tests
Sets up paths and environment variables
"""
import sys
import os
import platform
import pytest

# Get binary directory
tests_dir = os.path.dirname(os.path.abspath(__file__))
binary_dir = os.path.abspath(os.path.join(tests_dir, "..", "..", "..", "..", "Binaries", "Release"))
project_root = os.path.abspath(os.path.join(tests_dir, "..", "..", "..", ".."))

# Set PXR_USD_WINDOWS_DLL_PATH so USD can find its DLLs
os.environ['PXR_USD_WINDOWS_DLL_PATH'] = binary_dir
print(f"Set PXR_USD_WINDOWS_DLL_PATH={binary_dir}")

# Add to Python path
sys.path.insert(0, binary_dir)

# Add rznode python path
rznode_python = os.path.abspath(os.path.join(tests_dir, "..", "..", "..", "Core", "rznode", "python"))
sys.path.insert(0, rznode_python)

# Linux: Add USD Python bindings and preload shared libraries
if platform.system() != 'Windows':
    usd_python_path = os.path.join(project_root, "SDK", "OpenUSD", "Debug", "lib", "python")
    usd_lib_path = os.path.join(project_root, "SDK", "OpenUSD", "Debug", "lib")
    if os.path.exists(usd_python_path):
        sys.path.insert(0, usd_python_path)
    # Preload USD shared libraries using ctypes
    import ctypes
    import glob
    for so_file in sorted(glob.glob(os.path.join(usd_lib_path, "*.so"))):
        try:
            ctypes.CDLL(so_file, mode=ctypes.RTLD_GLOBAL)
        except OSError:
            pass

# Change to binary dir so DLLs can be loaded
os.chdir(binary_dir)
print(f"Changed working directory to: {os.getcwd()}")


@pytest.fixture
def binary_dir():
    """Provide the binary directory path"""
    return binary_dir
