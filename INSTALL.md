# CMake Installation Guide

## Overview

The Ruzino project supports full CMake installation with applications, tests, dependencies, and resources.

**Recommended Method:** Use the `build_and_install.py` script for a complete automated installation.

## Quick Start (Recommended)

The easiest way to build and install Ruzino is using the provided script:

```bash
# Basic installation
python scripts/build_and_install.py --install-dir /path/to/RuzinoInstall

# Installation with tests
python scripts/build_and_install.py --install-dir /path/to/RuzinoInstall --with-tests

# With CUDA support
python scripts/build_and_install.py --install-dir /path/to/RuzinoInstall --with-cuda

# Dry run to see what would happen
python scripts/build_and_install.py --install-dir /path/to/RuzinoInstall --dry-run
```

The script will:
1. Configure CMake with proper installation prefix
2. Build the project using Ninja
3. Install CMake targets (executables, libraries, headers)
4. Install all runtime dependencies (DLLs, resources, Python, etc.)
5. Run tests if `--with-tests` is specified

## Manual Installation Steps

If you prefer manual installation or need more control:

### 1. Configure with Install Prefix
```bash
cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DRUZINO_WITH_CUDA=ON \
  -DUSTC_HOMEWORK_PLUGINS=OFF \
  -DRUZINO_INSTALL_TESTS=ON \
  -DCMAKE_INSTALL_PREFIX=/path/to/RuzinoInstall \
  ..
```

### 2. Build
```bash
cmake --build .
```

### 3. Install
```bash
cmake --install .
```

### 4. Install Dependencies
```bash
python scripts/install_deps.py --install-dir /path/to/RuzinoInstall --build-type Release
```

## Build and Install Script Options

The `build_and_install.py` script supports the following options:

| Option | Description | Default |
|--------|-------------|---------|
| `--install-dir` | Installation directory (required) | - |
| `--build-type` | Build type: Release, Debug, RelWithDebInfo | Release |
| `--with-tests` | Install and run tests | Off |
| `--with-cuda` | Enable CUDA support | Off |
| `--build-dir` | Build directory | build |
| `--dry-run` | Print actions without executing | Off |
| `--skip-prerequisites` | Skip prerequisites check | Off |
| `--no-color` | Disable colored output | Off |

## What Gets Installed

### Applications (Required)
Installed to `<install_prefix>/bin/`:
- Ruzino.exe
- USTC_CG_polyscope.exe
- clear_time_samples.exe
- headless_geom_node_executor.exe
- headless_render.exe
- node_editor.exe
- py_importer.exe
- rz_render.exe
- rz_simulate.exe

### Tests (Optional)
Installed to `<install_prefix>/bin/tests/` when `RUZINO_INSTALL_TESTS=ON`:
- All test executables (*_test.exe)

### Libraries
Installed to `<install_prefix>/bin/` and `<install_prefix>/lib/`:
- All project DLLs
- All static libraries (.lib)

### Dependencies (via install_deps.py)
Installed to `<install_prefix>/bin/`:
- OpenUSD binaries, libraries, and Python bindings
- Slang compiler and runtime
- D3D12 Agility SDK (Windows)
- DXC compiler
- Embree runtime
- Python runtime
- CUDA runtime libraries (if available)
- Resources (libraries/, resources/)

### Headers
Installed to `<install_prefix>/include/`:
- geometry_nodes/
- All public headers

### CMake Config Files
Installed to `<install_prefix>/lib/cmake/Ruzino/`:
- RuzinoConfig.cmake
- Find*.cmake modules
- rznode cmake scripts

## Testing the Installation

`run_all_tests.py` runs from the current build tree (`./Binaries/Release/` and `./source/`); it does not take an install directory:

```bash
# All tests
python scripts/run_all_tests.py

# Filter by name
python scripts/run_all_tests.py cpu_slang
```

(UI/rendering tests are automatically skipped in headless environments.)

## Installation Structure

```
RuzinoInstall/
├── bin/
│   ├── *.exe                 # Applications
│   ├── *.dll                 # Libraries and dependencies
│   ├── tests/               # Test executables (optional)
│   │   └── *_test.exe
│   ├── libraries/           # USD libraries
│   ├── resources/           # USD resources
│   ├── python.exe           # Python runtime
│   ├── Lib/                 # Python standard library
│   └── Scripts/             # Python scripts (pip, etc.)
├── lib/
│   ├── *.lib                # Static libraries
│   └── cmake/
│       └── Ruzino/          # CMake config files
├── include/
│   ├── geometry_nodes/      # Geometry nodes headers
│   └── */                   # Other public headers
└── share/                   # Additional resources
```

## Options

### RUZINO_INSTALL_TESTS
- Default: OFF
- Description: Install test executables along with the project
- Usage: `-DRUZINO_INSTALL_TESTS=ON`

### CMAKE_INSTALL_PREFIX
- Required: Yes
- Description: Installation directory
- Usage: `-DCMAKE_INSTALL_PREFIX=/path/to/install`

## Notes

1. **Dependencies**: The `install_deps.py` script copies all runtime dependencies from the SDK directory. This must be run after `cmake --install`.

2. **Tests**: Test installation is optional. Enable with `-DRUZINO_INSTALL_TESTS=ON`.

3. **Cross-Platform**: Works on Windows, Linux, and macOS.

4. **Python Tests**: `run_all_tests.py` runs from the build tree (`./Binaries/Release/` and `./source/`); run it from the project root.

5. **Headless Testing**: UI/rendering tests are automatically skipped in headless environments.

## Example Complete Installation

### Using the Automated Script (Recommended)

```bash
# Full installation with tests
python scripts/build_and_install.py \
  --install-dir ../RuzinoInstall \
  --with-tests \
  --build-type Release

# The script handles all steps automatically
```

### Manual Installation

```bash
# 1. Configure
cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DRUZINO_INSTALL_TESTS=ON \
  -DCMAKE_INSTALL_PREFIX=../RuzinoInstall \
  ..

# 2. Build
cmake --build .

# 3. Install CMake targets
cmake --install .

# 4. Install dependencies
cd ..
python scripts/install_deps.py --install-dir ../RuzinoInstall --build-type Release

# 5. Test
python scripts/run_all_tests.py
```

## Verification

After installation, verify:

1. **Applications**: Check `bin/` directory for expected executables
2. **Dependencies**: Check `bin/` contains OpenUSD, Slang, Python, etc.
3. **Tests**: Check `bin/tests/` contains test executables (if enabled)
4. **Functionality**: Run `python scripts/run_all_tests.py` from the project root

## Troubleshooting

### Missing DLLs
- Ensure `install_deps.py` was run
- Check CUDA_PATH environment variable for CUDA libraries
- Verify SDK directory contains required dependencies

### Test Failures
- Some tests may require display/GPU (automatically skipped in headless)
- Check test output for specific error messages
- Verify all dependencies are in the installation bin/ directory

### Python Tests Fail
- Ensure Python runtime is installed (via install_deps.py)
- Check that bin/python.exe exists and is executable
