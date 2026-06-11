"""
pytest configuration for geometry node tests
Sets up paths and environment variables
"""

import sys
import os

# Get binary directory
binary_dir = os.path.join(
    os.path.dirname(__file__), "..", "..", "..", "..", "Binaries", "Release"
)
binary_dir = os.path.abspath(binary_dir)

project_root = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "..", "..")
)

sys.path.append(binary_dir)
if sys.platform == "win32":
    os.add_dll_directory(project_root + r"\SDK\python")
    os.add_dll_directory(project_root + r"\SDK\OpenUSD\Release\lib")

# Set PXR_USD_WINDOWS_DLL_PATH so USD can find its DLLs
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = binary_dir

# Add to Python path
sys.path.insert(0, binary_dir)

# Add rznode python path
rznode_python = os.path.join(
    os.path.dirname(__file__), "..", "..", "..", "Core", "rznode", "python"
)
sys.path.insert(0, os.path.abspath(rznode_python))

# Change to binary dir so DLLs can be loaded
os.chdir(binary_dir)
print(f"Test environment: binary_dir={binary_dir}")
