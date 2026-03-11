# Ruzino Installation and Export Guide

This document describes how to install the Ruzino SDK and use it in external projects via CMake's `find_package`.

## Table of Contents

- [Prerequisites](#prerequisites)
- [Building and Installing](#building-and-installing)
- [Installation Structure](#installation-structure)
- [Using Ruzino in Your Project](#using-ruzino-in-your-project)
- [Available CMake Targets](#available-cmake-targets)
- [Output Directory Configuration](#output-directory-configuration)
- [Troubleshooting](#troubleshooting)

## Prerequisites

Before building and installing Ruzino, ensure you have:

- CMake >= 3.20
- Ninja build system (recommended)
- C++20 compatible compiler (MSVC on Windows, Clang/GCC on Linux/macOS)
- Python 3.8+
- Vulkan SDK (for graphics)

## Building and Installing

### Quick Install

Use the provided build script for a complete SDK installation:

```bash
python scripts/build_and_install.py --install-dir /path/to/RuzinoInstall
```

This script will:
1. Configure CMake with the specified install prefix
2. Build all targets
3. Install libraries, headers, and executables
4. Copy all runtime dependencies (DLLs, shaders, resources)

### Manual Install

If you prefer manual control:

```bash
# 1. Configure
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/path/to/RuzinoInstall ..

# 2. Build
ninja

# 3. Install
cmake --install .

# 4. Copy dependencies (optional but recommended)
python ../scripts/install_deps.py --install-dir /path/to/RuzinoInstall
```

### Build Options

| Option | Description | Default |
|--------|-------------|---------|
| `CMAKE_INSTALL_PREFIX` | Installation directory | Required |
| `CMAKE_BUILD_TYPE` | Build configuration | `Release` |
| `RUZINO_INSTALL_TESTS` | Install test executables | `OFF` |

## Installation Structure

After installation, the directory structure is:

```
RuzinoInstall/
├── bin/                          # Executables and DLLs
│   ├── Ruzino.exe               # Main application
│   ├── node_editor.exe          # Node editor
│   ├── RHI.dll                  # Runtime libraries
│   ├── GUI.dll
│   ├── imgui.dll
│   ├── shaders/                 # Shader files
│   │   ├── renderer/
│   │   ├── gpu_assembler/
│   │   └── geom_nodes/
│   ├── SDK/                     # Third-party SDKs
│   │   └── slang/
│   └── Plugins/                 # Plugin descriptors
├── lib/                          # Static libraries and import libs
│   ├── RHI.lib
│   ├── GUI.lib
│   ├── nvrhi.lib
│   └── cmake/
│       └── Ruzino/              # CMake package config
│           ├── RuzinoConfig.cmake
│           ├── RuzinoConfigVersion.cmake
│           └── rznode/          # Build tools for plugins
│               └── cmake/
│                   ├── AddLibrary.cmake
│                   └── AddNodes.cmake
└── include/                      # Public headers
    ├── RHI/
    │   ├── rhi.hpp
    │   └── shaderCompiler.h
    ├── GUI/
    │   ├── window.h
    │   └── widget.h
    ├── imgui/
    ├── nvrhi/
    └── ...
```

## Using Ruzino in Your Project

### Basic Setup

Create a `CMakeLists.txt` in your project:

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Point to Ruzino installation
set(CMAKE_PREFIX_PATH "/path/to/RuzinoInstall")

# Find Ruzino package
find_package(Ruzino REQUIRED)

# Create your executable
add_executable(my_app main.cpp)

# Link against Ruzino libraries
target_link_libraries(my_app PRIVATE Ruzino::GUI)

# Use Ruzino's output directory (recommended)
set_target_properties(my_app PROPERTIES ${RUZINO_OUTPUT_DIR})
```

### Available Targets

Ruzino exports the following CMake targets under the `Ruzino::` namespace:

**Core Libraries:**
- `Ruzino::RHI` - Rendering Hardware Interface
- `Ruzino::GUI` - ImGui-based GUI framework
- `Ruzino::GPUContext` - GPU context management
- `Ruzino::nvrhi` - NVIDIA Rendering Hardware Interface

**Node System:**
- `Ruzino::nodes_core` - Core node functionality
- `Ruzino::nodes_system` - System nodes
- `Ruzino::nodes_ui_imgui` - ImGui UI nodes

**Scene/Stage:**
- `Ruzino::stage` - USD stage management
- `Ruzino::stage_listener` - Stage event handling
- `Ruzino::hd_RUZINO` - Hydra render delegate

**Utilities:**
- `Ruzino::rzconsole` - Console/logging utilities
- `Ruzino::rzpython` - Python bindings
- `Ruzino::blueprints` - Node editor blueprints

**Domain-Specific:**
- `Ruzino::geometry` - Geometry processing
- `Ruzino::MCore` - Mesh core utilities
- `Ruzino::RZSolver` - Solver framework
- `Ruzino::RZFemBem` - FEM/BEM solver

**Third-Party (bundled):**
- `Ruzino::imgui` - Dear ImGui
- `Ruzino::nvrhi` - NVIDIA VRHI

### Example: Creating a Window Application

```cpp
// main.cpp
#include <GUI/window.h>
#include <iostream>

class MyWidget : public Ruzino::IWidget {
public:
    bool BuildUI() override {
        ImGui::Text("Hello from Ruzino!");
        return true;
    }
};

int main() {
    Ruzino::Window window;
    
    // Add custom widget
    window.register_widget(std::make_unique<MyWidget>());
    
    // Auto-close after 100 frames
    window.register_function_after_frame([](Ruzino::Window* w) {
        static int frames = 0;
        if (++frames > 100) w->close();
    });
    
    window.run();
    return 0;
}
```

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(MyWindowApp LANGUAGES CXX)

set(CMAKE_PREFIX_PATH "/path/to/RuzinoInstall")
find_package(Ruzino REQUIRED)

add_executable(my_window main.cpp)
target_link_libraries(my_window PRIVATE Ruzino::GUI)
set_target_properties(my_window PROPERTIES ${RUZINO_OUTPUT_DIR})
```

### Example: Using RHI for Rendering

```cpp
#include <RHI/rhi.hpp>
#include <RHI/shaderCompiler.h>

int main() {
    // Initialize RHI
    RHI::init(true);
    
    // Get shader directory
    auto shader_dir = Ruzino::SlangShaderCompiler::get_shader_dir(
        Ruzino::ShaderDirType::Renderer);
    
    // Use device
    auto device = RHI::get_device();
    // ... rendering code ...
    
    RHI::shutdown();
    return 0;
}
```

## Output Directory Configuration

Ruzino exports `RUZINO_OUTPUT_DIR` to help downstream projects place their executables in the correct location:

```cmake
# After find_package(Ruzino REQUIRED)
# RUZINO_OUTPUT_DIR is available

# Apply to your targets
set_target_properties(my_app PROPERTIES ${RUZINO_OUTPUT_DIR})
```

This ensures:
- Executables are placed in `RuzinoInstall/bin/`
- Libraries are placed in `RuzinoInstall/lib/`
- All DLLs are co-located, eliminating PATH issues

## Plugin Development

For developing Ruzino plugins, additional CMake modules are available:

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyPlugin LANGUAGES CXX)

set(CMAKE_PREFIX_PATH "/path/to/RuzinoInstall")
find_package(Ruzino REQUIRED)

# Include Ruzino's build utilities
include("${RUZINO_CMAKE_DIR}/rznode/cmake/AddLibrary.cmake")
include("${RUZINO_CMAKE_DIR}/rznode/cmake/AddNodes.cmake")

# Use RUZINO_ADD_LIB to create a plugin library
RUZINO_ADD_LIB(
    my_plugin
    SHARED
    PUBLIC_LIBS Ruzino::stage Ruzino::geometry
    PRIVATE_LIBS spdlog::spdlog
)
```

## Dependency Management

### Automatic Dependencies

When linking against Ruzino targets, transitive dependencies are handled automatically:

```cmake
# This automatically links: GUI -> RHI -> nvrhi -> slang
target_link_libraries(my_app PRIVATE Ruzino::GUI)
```

### Include Paths

Ruzino automatically configures include paths for:
- Ruzino headers (`include/`)
- Third-party headers (imgui, nvrhi, etc.)
- SDK headers (slang)

No manual `target_include_directories` needed for Ruzino dependencies.

### Shader Paths

Ruzino provides runtime shader path resolution:

```cpp
#include <RHI/shaderCompiler.h>

// Works in both development and installed environments
auto renderer_shaders = Ruzino::SlangShaderCompiler::get_shader_dir(
    Ruzino::ShaderDirType::Renderer);
// Returns: install_dir/bin/shaders/renderer/

auto gpu_assembler_shaders = Ruzino::SlangShaderCompiler::get_shader_dir(
    Ruzino::ShaderDirType::GPUAssembler);
// Returns: install_dir/bin/shaders/gpu_assembler/

auto geom_nodes_shaders = Ruzino::SlangShaderCompiler::get_shader_dir(
    Ruzino::ShaderDirType::GeomNodes);
// Returns: install_dir/bin/shaders/geom_nodes/
```

## Troubleshooting

### "RuzinoConfig.cmake not found"

Ensure `CMAKE_PREFIX_PATH` points to the installation directory:
```cmake
set(CMAKE_PREFIX_PATH "C:/path/to/RuzinoInstall")
# or
set(CMAKE_PREFIX_PATH "/usr/local/RuzinoInstall")
```

### "DLL not found" at runtime

Option 1: Use `RUZINO_OUTPUT_DIR` (recommended)
```cmake
set_target_properties(my_app PROPERTIES ${RUZINO_OUTPUT_DIR})
```

Option 2: Add to PATH (Windows)
```cmd
set PATH=C:\path\to\RuzinoInstall\bin;%PATH%
my_app.exe
```

Option 3: Set environment variable
```bash
export RUZINO_SDK_PATH=/path/to/RuzinoInstall
```

### "Cannot open include file: 'imgui.h'"

This is handled automatically when using Ruzino targets. Ensure you're linking properly:
```cmake
target_link_libraries(my_app PRIVATE Ruzino::GUI)
```

### Shaders not found

Verify shader installation:
```bash
ls RuzinoInstall/bin/shaders/
# Should show: renderer/  gpu_assembler/  geom_nodes/
```

If missing, re-run the install step:
```bash
cd build && cmake --install .
```

## Advanced Topics

### Custom Installation Layout

To customize the installation layout, modify `CMAKE_INSTALL_*` variables:

```cmake
cmake -DCMAKE_INSTALL_PREFIX=/opt/ruzino \
      -DCMAKE_INSTALL_BINDIR=bin \
      -DCMAKE_INSTALL_LIBDIR=lib64 \
      -DCMAKE_INSTALL_INCLUDEDIR=include ..
```

### Version Constraints

Ruzino follows semantic versioning. Request a specific version:

```cmake
find_package(Ruzino 1.0 REQUIRED)
```

### Components

Ruzino doesn't currently use components, but you can require specific targets:

```cmake
find_package(Ruzino REQUIRED)
if(NOT TARGET Ruzino::GUI)
    message(FATAL_ERROR "GUI component not available")
endif()
```

## Contributing

When modifying the installation system:
1. Update `cmake/RuzinoConfig.cmake.in` for new targets
2. Update `external/CMakeLists.txt` for third-party dependencies
3. Add install rules in library CMakeLists.txt files
4. Test with a clean install directory

## Support

For issues or questions:
- GitHub Issues: https://github.com/anomalyco/opencode/issues
- Documentation: `docs/` directory
- Build scripts: `scripts/build_and_install.py`
