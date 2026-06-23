# Build and Test Workflow

This document describes the build and test workflow for the Ruzino Framework3D project.
It is a quick reference for AI agents; see `INSTALL.md` for the full installation guide.

## Project Structure

- `cmake/AddLibrary.cmake`: Thin wrapper (7 lines) that delegates to `source/Core/rznode/cmake/AddLibrary.cmake`
- Configuration flags (`RZNODE_CUDA_EXTRA_FLAGS`, `RZNODE_LINK_PYTHON_TO_NANOBIND`) are set in root `CMakeLists.txt`
- `source/Core/rznode/cmake/AddLibrary.cmake`: Canonical source containing all build logic

## Scripts Quick Reference

| Script | Purpose |
|--------|---------|
| `scripts/build_devshell.ps1` | Windows incremental build inside VS DevShell (fastest dev loop) |
| `scripts/build_and_install.py` | Full SDK-style configure → build → install → deps → test |
| `scripts/install_deps.py` | Copy runtime deps (DLLs, Python, USD, CUDA) to an install dir; called by `build_and_install.py` |
| `scripts/run_all_tests.py` | Find and run pytest + C++ tests under `source/` and `Binaries/Release/` |
| `scripts/format_and_commit_manager.py` | Format C/C++ changes + recursive commit/push (skips nvrhi) |
| `scripts/install_linux_deps.sh` | apt install Linux system dev packages |
| `scripts/format_and_commit_manager.ps1` | PowerShell launcher for the above (double-click friendly) |

### Windows quick-build `.bat` scripts (repo root)

All `.bat` scripts load the MSVC environment (`vcvars64.bat`) themselves, then run `ninja` in the preconfigured `build/` directory. Prefer these for incremental builds — no PowerShell, no reconfigure step.

| Script | ninja target |
|--------|--------------|
| `build.bat` | passes args through; no args = full build (`ninja`) |
| `build_ruzino.bat` | `Ruzino` (main app) |
| `gen_json.bat` | `geometry_nodes_json_target` |
| `reconfig.bat` | re-run CMake configure in `build/` (no rebuild) |

All of them require an existing `build/build.ninja`. Each prints `BUILDEXIT=<code>` (`reconfig.bat` → `CONFIGEXIT`, `gen_json.bat` → `JSONEXIT`) so the build result is machine-checkable. For any other single target, use `build.bat <target>` (e.g. `build.bat node_brush_capture`).

## Prerequisites

- CMake (>= 3.31.5), Ninja
- Python 3.10 or 3.11
- Vulkan SDK 1.3.296
- MSVC (Windows) or Xcode (macOS)

Initial setup:
```bash
git submodule update --init --recursive
```

## Build

### Incremental build (daily development)

If `build/build.ninja` exists, just rebuild:
```bash
cd build
ninja
```

On Windows, `build_devshell.ps1` loads the VS DevShell and runs an incremental build in one step:
```powershell
pwsh -File scripts/build_devshell.ps1            # incremental
pwsh -File scripts/build_devshell.ps1 -Reconfigure # wipe + reconfigure
```

For single-target incremental builds, the repo-root `.bat` scripts (see "Windows quick-build `.bat` scripts" above) are faster and self-contained — they set up the MSVC environment themselves and skip the reconfigure step. Example:
```cmd
build.bat                 :: full incremental build (ninja, no args)
build.bat node_brush_capture   :: node_brush_capture only
```

### Fresh configure

```bash
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DRUZINO_WITH_CUDA=ON -DUSTC_HOMEWORK_PLUGINS=OFF ..
ninja
```

Build readiness check: `build/build.ninja` and `build/CMakeCache.txt` must exist.

### SDK installation

Use `build_and_install.py` for a self-contained, `find_package(Ruzino)`-ready install. See `INSTALL.md` for details:
```bash
python scripts/build_and_install.py --install-dir ../RuzinoInstall
python scripts/build_and_install.py --install-dir ../RuzinoInstall --with-tests
```

## Test

`run_all_tests.py` finds `tests/` folders under `./source/`, runs pytest on `test_*.py`, and runs matching `*_test.exe` from `./Binaries/Release/`.

```bash
python scripts/run_all_tests.py                 # all tests
python scripts/run_all_tests.py rhi_test        # filter by name
```

- Python tests: `source/*/tests/test_*.py`, run via pytest, 300s timeout each.
- C++ tests: `Binaries/Release/*_test.exe`, 300s timeout each.
- Exit code 0 = all pass, 1 = any failure; failures print their last 50 lines.

## Commit

`format_and_commit_manager.py` formats modified C/C++ files and commits recursively across the repo and submodules (depth-first, submodules before root), **always skipping nvrhi**.

- **Quick mode**: auto-formats and only prompts for commit messages.
- **Interactive mode**: lets you select which files to format and confirm each commit/push.

```bash
python scripts/format_and_commit_manager.py
```

## Conventions for Code Changes

1. **Build before testing** so C++ test executables are up to date.
2. **Check build readiness** by verifying `build/build.ninja` exists.
3. **Use the test runner** rather than invoking individual tests manually.
4. **Review failed-test output** — failures show detailed error messages.
5. **Match build type** — build the same type (Release/Debug) the test runner expects.
6. **Installation deps** — run `install_deps.py` after `cmake --install` to copy SDK runtime libraries.
7. **Recursive commits** — use `format_and_commit_manager.py`; it skips the nvrhi submodule.

## Troubleshooting

### Build Issues
- cmake fails → check dependencies (see `README.md`)
- ninja fails → try `rm -rf build/*` and reconfigure
- Python must be 3.10 or 3.11

### Test Issues
- C++ tests not found → build first
- Python import errors → install Python dependencies
- Wrong-directory errors → run from project root
