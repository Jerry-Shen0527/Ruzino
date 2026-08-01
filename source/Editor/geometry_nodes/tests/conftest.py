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

# Test output directory — mirrors source/tests/conftest.py. Tests in this
# directory do `from conftest import TEST_OUTPUT_DIR`, which resolves to *this*
# conftest (pytest's nearest-conftest-wins rule), NOT the top-level one. So
# TEST_OUTPUT_DIR must be defined here, or that import fails with ImportError.
# Compute it independently rather than importing from the top-level conftest,
# to avoid depending on conftest load order when this suite runs in isolation.
TEST_OUTPUT_DIR = os.environ.get("RZ_TEST_OUTPUT_DIR") or os.path.join(binary_dir, "test_output")
os.makedirs(TEST_OUTPUT_DIR, exist_ok=True)
os.environ["RZ_TEST_OUTPUT_DIR"] = TEST_OUTPUT_DIR

sys.path.append(binary_dir)
if sys.platform == "win32":
    os.add_dll_directory(binary_dir)
    os.add_dll_directory(project_root + r"\SDK\python")
    os.add_dll_directory(project_root + r"\SDK\OpenUSD\Release\lib")

# Set PXR_USD_WINDOWS_DLL_PATH so USD can find its DLLs
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = binary_dir

# Put binary_dir on PATH so node plugins (e.g. GPU_sph.dll), loaded by the
# C++ LoadLibrary(<name>) call inside load_configuration, can resolve. C++
# LoadLibrary searches the process PATH / app dir, NOT the cwd that
# os.chdir points at below nor the Python os.add_dll_directory entries —
# so PATH is required. (Mirrors source/tests/conftest.py.)
os.environ["PATH"] = binary_dir + os.pathsep + os.environ.get("PATH", "")

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
