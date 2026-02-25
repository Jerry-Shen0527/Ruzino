"""
pytest configuration for modifier mode tests

This sets up the Python path and environment for testing the modifier architecture.
"""
import sys
import os

# Get the binary directory (where DLLs and Python modules are)
# From tests/python/ to Binaries/Release
tests_dir = os.path.dirname(os.path.abspath(__file__))
binary_dir = os.path.abspath(os.path.join(tests_dir, '..', '..', 'Binaries', 'Release'))

# Add to path BEFORE importing anything
if binary_dir not in sys.path:
    sys.path.insert(0, binary_dir)

# Set environment for USD DLLs
os.environ['PXR_USD_WINDOWS_DLL_PATH'] = binary_dir

# Change working directory to binary_dir so DLLs can be loaded
if os.path.exists(binary_dir):
    os.chdir(binary_dir)

print(f"Test environment configured:")
print(f"  Binary dir: {binary_dir}")
print(f"  Working dir: {os.getcwd()}")
