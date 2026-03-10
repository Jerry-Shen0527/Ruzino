# Build and Test Workflow

This document describes the build and test workflow for the Ruzino Framework3D project.

## Project Structure

- `cmake/AddLibrary.cmake`: Thin wrapper (7 lines) that delegates to `source/Core/rznode/cmake/AddLibrary.cmake`
- Configuration flags (`RZNODE_CUDA_EXTRA_FLAGS`, `RZNODE_LINK_PYTHON_TO_NANOBIND`) are set in root `CMakeLists.txt`
- `source/Core/rznode/cmake/AddLibrary.cmake`: Canonical source containing all build logic

## Build Process

### Prerequisites
- CMake (>= 3.31.5)
- Ninja build system
- Python 3.10.11
- Vulkan SDK 1.3.296
- MSVC compiler (Windows) or Xcode (macOS)

### Initial Setup

Before building, ensure all submodules are initialized:
```bash
git submodule update --init --recursive
```

### Build Commands

The project uses CMake with Ninja generator. Follow these steps:

1. **Create build directory** (if it doesn't exist):
   ```bash
   mkdir build
   ```

2. **Configure the project** (run in build directory):
   ```bash
   cd build
   cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSTC_CG_WITH_CUDA=ON -DUSTC_HOMEWORK_PLUGINS=OFF ..
   ```

3. **Build the project**:
   ```bash
   ninja
   ```

### Quick Build (if already configured)

If the build directory is already configured (contains `build.ninja` and `CMakeCache.txt`), you can skip the cmake configuration step and directly run:

```bash
cd build
ninja
```

To check if build is ready:
- Verify `build/build.ninja` exists
- Verify `build/CMakeCache.txt` exists

## Test Process

### Running All Tests

The project includes a comprehensive test runner script: `scripts/run_all_tests.py`

This script:
1. Recursively finds all `tests/` folders under `./source/`
2. Runs pytest on any `test_*.py` files found
3. Runs corresponding C++ test executables from `./Binaries/Release/`

#### Usage

```bash
# Run all tests
python scripts/run_all_tests.py

# Run specific test by name
python scripts/run_all_tests.py <test_name>

# Examples:
python scripts/run_all_tests.py rhi_test
python scripts/run_all_tests.py cpu_slang
```

### Test Types

#### Python Tests
- Located in `source/*/tests/test_*.py`
- Run using pytest
- Must pass `-v` and `--tb=short` flags
- Timeout: 300 seconds per test file

#### C++ Tests
- Executables located in `Binaries/Release/` (or `Binaries/Debug/`)
- Naming convention: `<name>_test.exe` or `<name>.cpp` → `<name>_test.exe`
- Timeout: 300 seconds per test

### Test Output

The test runner provides:
- Progress output for each test
- Summary of passed/failed/skipped tests
- Detailed error output for failed tests (last 50 lines)
- Exit code: 0 for success, 1 for any failures

## Complete Build and Test Workflow

For a complete build and test cycle:

```bash
# 1. Navigate to project root
cd /path/to/Ruzino

# 2. Configure and build
if [ ! -f "build/build.ninja" ]; then
    echo "Configuring build..."
    mkdir -p build
    cd build
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSTC_CG_WITH_CUDA=ON -DUSTC_HOMEWORK_PLUGINS=OFF ..
else
    echo "Build already configured, running ninja..."
    cd build
fi

ninja
cd ..

# 3. Run tests
python scripts/run_all_tests.py
```

## Notes for AI Agents

When making code changes:

1. **Always build before testing** to ensure C++ test executables are up-to-date
2. **Check build readiness** by verifying `build/build.ninja` exists
3. **Use the test runner** instead of running individual tests manually
4. **Review test output** carefully - failed tests will show detailed error messages
5. **Build type matters** - ensure you're building the same type (Release/Debug) as the test runner expects

## Troubleshooting

### Build Issues
- If cmake fails, check that all dependencies are installed (see README.md)
- If ninja fails, try cleaning the build: `rm -rf build/*` and reconfigure
- Ensure Python version is exactly 3.10.11

### Test Issues
- If C++ tests are not found, ensure you've built the project first
- If Python tests fail with import errors, ensure all Python dependencies are installed
- Check that you're in the correct directory when running tests
