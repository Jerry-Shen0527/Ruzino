"""
pytest configuration for renderer tests
Sets up paths and environment variables
"""
import sys
import os
import platform

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

# DLL loading is handled by PATH/add_dll_directory above;
# do NOT os.chdir() here — the shader cache and config loading
# depend on the working directory being the project root.

# MaterialX's getDefaultDataSearchPath() uses getModulePath() which returns
# the Python exe directory, not the MaterialX DLL directory. So the standard
# library search fails. Setting CWD to binary_dir ensures that
# std::filesystem::current_path() (added as a MaterialX search path in
# materialX.cpp init) resolves to Binaries/Release/ where libraries/ exists.
os.chdir(binary_dir)
