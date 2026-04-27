# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Ruzino Framework3D is a node-based 3D graphics framework for USTC CG 2025 (University of Science and Technology of China Computer Graphics course). It uses USD (Universal Scene Description) for scene representation, a visual node editor for programming, real-time rendering (Vulkan/D3D12), physics simulation, and Python scripting.

## Build System

### Prerequisites
- CMake >= 3.31.5, Ninja build system
- Python 3.10.11 (exact version, from `SDK/python`)
- MSVC (Windows) or GCC/Clang (Linux), Xcode (macOS)
- Vulkan SDK 1.3.296

### Initial Setup
```bash
git submodule update --init --recursive
python configure.py --all --build_variant Debug
```

### Build
```bash
# Quick build (if already configured - build/build.ninja exists)
cd build && ninja

# Fresh configure + build
mkdir -p build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DUSTC_CG_WITH_CUDA=ON ..
ninja
```

Binaries output to `Binaries/{Debug,Release}/`.

### Full SDK Install
```bash
python scripts/build_and_install.py --install-dir ../RuzinoInstall --with-tests
```

### Key CMake Options
| Option | Purpose | Default |
|--------|---------|---------|
| `USTC_CG_WITH_CUDA` / `RUZINO_WITH_CUDA` | Enable CUDA support | OFF |
| `USTC_HOMEWORK_PLUGINS` | Homework-specific plugins | OFF |
| `RUZINO_INSTALL_TESTS` | Install test executables | OFF |
| `RUZINO_WITH_TORCH` | LibTorch integration | OFF |
| `ENABLE_SUBMISSION` | Homework submission system | OFF |

## Testing

```bash
# Run all tests
python scripts/run_all_tests.py

# Run specific test
python scripts/run_all_tests.py cpu_slang

# Test an installed SDK
python scripts/run_all_tests.py --install-dir ../RuzinoInstall
```

- Python tests: `source/*/tests/test_*.py` (via pytest)
- C++ tests: `Binaries/Release/*_test` executables (via GoogleTest)
- UI/rendering tests auto-skip in headless environments

## Architecture

### Source Layout (`source/`)

**`Core/`** — Engine internals
- `RHI/` — Rendering Hardware Interface (GPU abstraction over Vulkan/D3D12)
- `rznode/` — Node system core (git submodule with its own CMake; `AddLibrary.cmake` here is the canonical one)
- `rzsim/` — Physics simulation (soft/rigid body)
- `Solver/` — Mathematical solvers
- `GPUContext/` — GPU context management

**`Editor/`** — UI and authoring tools
- `stage/` — USD scene editor
- `renderer/` — Rendering viewport
- `polyscope_widget/` — 3D visualization widget
- `basic_nodes/` — Built-in node implementations
- `geometry_nodes/` — Geometry processing node system

**`Runtime/`** — Application runtime
- `GUI/` — UI framework (ImGui-based)
- `material/` — Material system
- `geometry/` — Geometry processing (git submodule)
- `rzconsole/` — Console interface
- `rzpython/` — Python integration (via nanobind)

**`Plugins/`** — Extensibility
- `hd_RUZINO_GL/` — Hydra render delegate (OpenGL)
- `light_field/` — Light field rendering
- `optimization/` — Performance optimization

### Key Submodules
- `source/Core/rznode` — Node system (has own CMake build; `AddLibrary.cmake` is the canonical copy)
- `source/Runtime/geometry` — Geometry processing
- `external/nvrhi` — Low-level GPU abstraction (skip in recursive commits)
- `external/googletest`, `external/nanobind`, `external/imgui`, etc.

### SDK Dependencies (`SDK/`)
Pre-built dependencies managed by `configure.py`: OpenUSD v25.05.01, Slang v2025.22.1, D3D12 Agility SDK, DXC, Embree, Python 3.10.11, NVAPI.

### Build Logic
- `cmake/AddLibrary.cmake` — Thin wrapper delegating to `source/Core/rznode/cmake/AddLibrary.cmake` (canonical)
- Custom CMake modules in `cmake/` for finding SDK packages (Slang, NVAPI, AgilitySDK, etc.)

## Scripts

| Script | Purpose |
|--------|---------|
| `configure.py` | Download/configure SDK dependencies |
| `scripts/build_and_install.py` | Full build + install + optional tests |
| `scripts/install_deps.py` | Copy runtime DLLs/resources to install dir |
| `scripts/run_all_tests.py` | Test runner (Python + C++) |
| `scripts/format_and_commit_manager.py` | Code formatting + git commit workflow |
| `scripts/recursive_commit_and_push.sh` | Recursive git commit across submodules |
| `scripts/clang_format_manager.py` | Clang-format management |

## Notes

- Recursive git commits should skip the `nvrhi` submodule
- Python bindings use nanobind; Python3 is found from `SDK/python`
- The framework supports `find_package(Ruzino)` when installed as an SDK
- Output binaries go to `Binaries/{BuildType}/` during development, `bin/` when installed

## USD Hydra Integration Notes

### SDK is pre-built — source patches have no effect
`SDK/OpenUSD/source/` contains reference source only. The actual compiled libraries are in `SDK/OpenUSD/Release/lib/`. Editing source files under `SDK/OpenUSD/source/` will NOT affect the build unless you rebuild the entire USD SDK via `configure.py`.

### Dirty notification mechanism (USD 26.x scene index)
USD 26.x uses a scene index chain. Property changes flow through `Invalidate()` on each data source. Key method: `UsdImagingDataSourceMaterialPrim::Invalidate()` in `dataSourceMaterial.cpp`.

- `inputs:*` attributes trigger dirty via `UsdShadeInput::IsInterfaceInputName()` — **this works out of the box**
- `config:*` attributes are forwarded to `HdMaterialNetworkMap::config` (data works) but do NOT trigger `Invalidate()` (no dirty notification)
- Custom attributes without prefix are not forwarded at all in USD 26.x
- **Lesson**: For custom parameters that need dirty tracking, use `inputs:*` on shader nodes (material interface inputs connected to shader inputs). Do NOT rely on `config:*` for properties the user can change at runtime.

### Custom shader_path architecture
`shader_path` is stored as `config:shader_path` on the material prim. `config:*` attributes are forwarded to `HdMaterialNetworkMap::config` — data reading works. However, `config:*` changes do NOT trigger `Invalidate()` / `Sync()` automatically. A mechanism to force material resync when `config:shader_path` changes is needed (e.g. subclassing `UsdImagingGLEngine`).
- **Data**: Read from `hdNetwork.config["shader_path"]` in materialX.cpp Sync()
- **Dirty**: NOT automatic — needs external trigger mechanism
- **UI**: The file viewer shows `config:shader_path` on the material prim in a dropdown

### Renderer dirty flags
- `Hd_RUZINO_Material::Sync()` calls `mark_materials_dirty()` which increments `material_version`
- `renderer.cpp` compares `material_version` each frame to set `DirtyMaterials` on the global payload
- Mesh material binding changes must also call `mark_materials_dirty()` in mesh.cpp (not just `_SetMaterialId`)
- Path tracing node rebuilds when `mat_dirty` is true

### Running USD Python scripts
The SDK Python (`SDK/python/python.exe`) needs `PXR_USD_WINDOWS_DLL_PATH` set to the Binaries directory, and `PYTHONPATH` must include both `Binaries/Release` and `SDK/OpenUSD/Release/lib/python`. See conftest.py files for the canonical setup.
