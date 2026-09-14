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
| `scripts/build_devshell.ps1` | Windows incremental build inside VS DevShell (fastest dev loop). Supports `-LogFile <path>` to tee ninja output to a log file and `-MachineReadable` to print a `BUILDEXIT=<code>` line — together these replace the old root-level `_build_one.bat` for headless/CI/agent use. |
| `scripts/build_and_install.py` | Full SDK-style configure → build → install → deps → test |
| `scripts/install_deps.py` | Copy runtime deps (DLLs, Python, USD, CUDA) to an install dir; called by `build_and_install.py` |
| `scripts/run_all_tests.py` | Find and run pytest + C++ tests under `source/` and `Binaries/Release/` |
| `scripts/format_and_commit_manager.py` | Format C/C++ changes + recursive commit/push (skips nvrhi) |
| `scripts/install_linux_deps.sh` | apt install Linux system dev packages |
| `scripts/format_and_commit_manager.ps1` | PowerShell launcher for the above (double-click friendly) |

## Prerequisites

- CMake (>= 3.31.5), Ninja
- Python 3.13
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

For single-target incremental builds, pass the ninja target to the same script via `-Target`:
```powershell
pwsh -File scripts/build_devshell.ps1 -Target Ruzino              # main app only
pwsh -File scripts/build_devshell.ps1 -Target node_brush_capture  # one node
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
8. **Prefer new nodes over architecture changes** — when implementing a
   feature in the node system, first try a NEW node (kept generic and
   reusable) instead of modifying existing nodes or the framework; extend
   existing nodes only when a new node genuinely cannot do the job.
   Socket asymmetry: adding extra OUTPUT sockets / output data is fine (a
   node may emit more than before), but adding INPUT sockets changes what
   callers must provide — think twice, prefer optional inputs with sane
   defaults. Remember a new node `.cpp` needs a CMake `add_nodes` update
   (re-configure) plus registration in the owning plugin's JSON.

## Simulation (headless node-graph sim)

Per-frame accumulation across ticks is driven by the **simulation-zone feedback loop**, not by per-node logic; Python-built zones must use `RuzinoGraph.createSimulationZone()` (bare `createNode` leaves `paired_node` null and silently stops accumulating). Full mechanism, `Node::storage` channels, and the three gates a headless `Stage.tick()` loop must open: `docs/simulation_mechanism.md`.

## Character walking demo (gameplay layer)

Third-person walking BOT demo (`source/Runtime/input` + `source/Runtime/character`,
`character:controller` prim marker, called from `Stage::tick`) — run commands,
architecture, headless driving, and the offline-renderer limitations it
exposed are all in `docs/character_walk_demo.md`.

## Troubleshooting

### Build Issues
- cmake fails → check dependencies (see `README.md`)
- ninja fails → try `rm -rf build/*` and reconfigure
- Python must be 3.13
- **Shader-only edits don't propagate to `Binaries/`** → runtime shaders
  (e.g. `usd/hd_RUZINO/resources/callables/*.slang`) are copied by a
  POST_BUILD `copy_directory` on the owning target (e.g. `hd_RUZINO`). If
  only a `.slang` changed, ninja sees nothing to rebuild, the target doesn't
  relink, and the POST_BUILD copy never runs — the old shader keeps loading.
  Workaround: `cmake -E copy_if_different <src.slang> <Binaries/.../dst.slang>`
  (same command the build runs), or touch a C++ file in the target first.
- **`build_devshell.ps1` BuildType corruption** → if `CMakeCache.txt` shows `CMAKE_BUILD_TYPE=$BuildType` (literal, not `Release`), the script's `$BuildType` argument was not interpolated into the cmake `-D` flag. This was fixed: the flag is now quoted (`"-DCMAKE_BUILD_TYPE=$BuildType"`). The script also self-checks the cache on configure failure and prints a diagnostic if the build type is non-standard. A corrupted cache requires deleting `build/` and reconfiguring.

### Test Issues
- C++ tests not found → build first
- Python import errors → install Python dependencies
- Wrong-directory errors → run from project root
- **GPU crash / access violation** (`0xC0000005`, "Windows fatal exception") with no useful stack → set `RZ_RHI_VALIDATION=1` and rerun. This enables the nvrhi validation layer + D3D12 debug runtime, which prints the *actual* resource-misuse (e.g. "Bindings declared in the layout are not present in the binding set: t6" → "Device Removed!") before the crash, instead of a bare access violation. Costs performance, off by default. Also dumps the reflected shader binding map + which slots are bound/NULL when set, so an unbound SRV is named directly. See `rhi.cpp` (`RZ_RHI_VALIDATION` gate) and `program_vars.cpp` (binding dump).
- **`run_all_tests.py` exit code** → 0 = all pass, 1 = any failure. The runner distinguishes `PASSED` / `○ SKIPPED` (whole file skipped, e.g. missing optional dep like torch) / `✗ FAILED (crash: access violation)` (infra crash, not a logic failure).

### Native Debugging with cdb (access-violation crashes)

When a test dies with a bare "Windows fatal exception: access violation" and no
Python-level stack, the cause is in native (C++) code. Use **cdb** (Windows
Console Debugger, ships with the Windows Kits Debuggers) to capture the precise
faulting function, DLL, and source line. This was the workflow that pinned down
the `get_output_texture` type-punning crash (`renderer.cpp:240`) in Aug 2026.

**The helper script wraps it:**
```bash
python scripts/cdb_crash.py -m pytest source/.../test_rendering.py::test_render_basic -s
```
It runs the target under cdb, breaks on the access violation, prints the
exception record + faulting-thread stack + loaded modules, then quits. Look for
`=== Access violation caught ===` and the `Call Site` column.

**For source-level stacks, you need PDBs — use the Debug build:**
1. Build Debug (has `/Zi` + PDBs; Release does not):
   ```powershell
   pwsh -File scripts/build_devshell.ps1 -BuildType Debug -BuildDir build-debug -Reconfigure
   ```
2. Run the test under cdb with `RZ_BUILD_TYPE=Debug` so Python loads the
   **Debug** DLLs (the conftests respect this env var). Mixed Debug/Release
   DLLs corrupt memory and produce false-positive assertions — a pure-Debug run
   is essential:
   ```bash
   cd Binaries/Debug
   PATH="$PWD:$PATH" RZ_BUILD_TYPE=Debug python scripts/cdb_crash.py -m pytest <test> -s
   ```
   With `-lines` and the Debug PDBs, frames annotate as
   `module!Function+0xNN [C:\...\file.cpp @ LINE]`.

**Two gotchas learned the hard way:**
- `run_all_tests.py` captures stdout, so a crash mid-test loses the last logs.
  Run the single failing test directly with `-s` (unbuffered) for pre-crash output.
- A Debug-mode *assertion* (e.g. entt `is_power_of_two`) can be a **false
  positive** if any DLL loaded as Release. Confirm *every* relevant DLL in the
  cdb `ModLoad` lines comes from `Binaries\Debug` before trusting an assertion
  as the root cause. The Release build skips `_ASSERT` (`NDEBUG`), so a real
  logic bug may surface only as a downstream access violation in Release.

### CRITICAL: never write `&someRefCountPtr`

nvrhi's `RefCountPtr<T>` overloads unary `operator&()` to return `T**` (the
inner `ptr_` member address), not `RefCountPtr*` — it compiles fine and
corrupts silently; two confirmed access-violation cases are dissected in
`docs/nvrhi_refcountptr_pitfall.md`. Rule: take addresses with
`std::addressof(handle)` so swaps go through the proper move operators, and
grep `make_pair(&` / `&field->` / `&it->` whenever an `nvrhi::*Handle` is
involved.
