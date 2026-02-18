"""
pytest configuration for stage tests

This sets up the Python path and environment for testing.
"""
import sys
import os
import platform

# Get the binary directory (where DLLs and Python modules are)
binary_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..', '..', 'Binaries', 'Release'))
project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..', '..'))
rznode_python = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..', 'Core', 'rznode', 'python'))

# Add to path
sys.path.insert(0, binary_dir)
sys.path.insert(0, rznode_python)

# Set environment for USD DLLs
os.environ['PXR_USD_WINDOWS_DLL_PATH'] = binary_dir

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

# Change working directory to binary_dir so DLLs can be loaded
os.chdir(binary_dir)

print(f"Test environment configured:")
print(f"  Binary dir: {binary_dir}")
print(f"  RZNode Python: {rznode_python}")
print(f"  Working dir: {os.getcwd()}")
