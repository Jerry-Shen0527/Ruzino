#!/usr/bin/env python3
"""
Run a Python script/test under cdb (Windows Console Debugger) to capture the
exact C++ call stack of an access-violation crash.

cdb is part of the Windows Debugging Tools (Windows Kits). It catches native
exceptions that Python/pytest cannot, printing the precise DLL + function +
offset where the crash happens — essential when a test dies with a bare
"Windows fatal exception: access violation" and no useful stack.

Usage:
    python scripts/cdb_crash.py <python_script_or_pytest_args...>

Examples:
    python scripts/cdb_crash.py -m pytest source/.../test_rendering.py::test_render_basic -s
    python scripts/cdb_crash.py some_crashing_script.py

Prerequisites:
    - Windows Kits Debuggers installed (cdb.exe at the path below).
    - For full symbols, build with RelWithDebInfo (Release has no PDBs).
      Without PDBs you still get the DLL name + offset, which narrows the crash
      to the right module.

The script runs the target under cdb, which is configured to:
  1. Break on access violations (first-chance AND last-chance).
  2. On break, print the exception record, the call stack of the faulting
     thread, and the loaded module list, then quit.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

CDB_CANDIDATES = [
    r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe",
    r"C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe",
]

# cdb initial commands: configure exception handling + what to dump on crash.
# All on ONE line separated by semicolons — cdb's -c flag takes a single
# command string, not a multi-line script, and subprocess must pass it as one
# arg so the shell doesn't split it.
CDB_SCRIPT = (
    ".lines;"                              # enable source line info (needs PDBs)
    "sxe av;"                              # break (first-chance) on access violation
    "g;"                                   # NOW run the target until AV or exit
    ".echo === Access violation caught ===;"
    ".exr -1;"                             # dump exception record
    ".echo === Registers ===;"
    "r;"                                   # dump all registers (rcx = 1st arg)
    ".echo === Faulting IP source ===;"
    "ln @rip;"                             # resolve faulting instr to nearest symbol
    ".echo === Faulting thread stack ===;"
    "~#s;"                                 # switch to faulting thread
    "knL;"                                 # print stack with module+frame numbers
    ".echo === Caller frame locals (frame 2) ===;"
    ".frame /r 2;"                         # switch to copyBuffer caller with regs
    "dv /i /V;"                            # dump locals with values + types
    ".echo === Loaded modules ===;"
    "lm;"                                  # list modules (name + load addr + range)
    "q"                                    # quit debugger
)


def find_cdb() -> str:
    for p in CDB_CANDIDATES:
        if Path(p).exists():
            return p
    # fallback: search PATH
    found = shutil.which("cdb")
    if found:
        return found
    raise FileNotFoundError(
        "cdb.exe not found. Install 'Debugging Tools for Windows' "
        "(Windows SDK/WDK component) or set it on PATH."
    )


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)

    cdb = find_cdb()
    target_args = sys.argv[1:]

    # Use the SAME python that is running this script (sys.executable). The
    # project's portable SDK/python/python.exe does NOT have pytest installed,
    # so it can't run tests. The caller is responsible for invoking this script
    # with a python that has pytest (e.g. scoop python313). The test's conftest
    # injects Binaries/{Release,Debug} onto PATH and PXR_USD_WINDOWS_DLL_PATH,
    # so native DLLs resolve correctly regardless of which interpreter runs.
    python_exe = sys.executable

    # cdb flags:
    #   -o          output to console
    #   -G          ignore final breakpoint on process exit
    #   -c "<cmds>" initial commands: set AV break, THEN 'g' to run. We do NOT
    #               use -g (run immediately) because that skips the -c commands
    #               entirely; the 'g' must come last inside -c so sxe av is set
    #               first.
    #   -y          symbol path (Microsoft symbol server + local cache).
    #   -logo       opens a NEW log file capturing full output (cdb truncates
    #               console output to ~64KB). NOTE: this is -logo, NOT -cfl
    #               (which does not exist — it gets misparsed as -cf "command
    #               file" and cdb tries to execute the log PATH as a script,
    #               silently aborting with "Address expression missing").
    log_file = str(Path(__file__).resolve().parent.parent / "cdb_output.log")
    cdb_cmd = [
        cdb,
        "-o", "-G",
        "-c", CDB_SCRIPT,
        "-y", "srv*C:\\symbols*https://msdl.microsoft.com/download/symbols",
        "-logo", log_file,
        python_exe,
    ] + target_args

    # Respect RZ_BUILD_TYPE so a Debug crash can be debugged with PDBs. The
    # test conftests read the same env var to load Binaries/{Debug,Release}.
    build_type = os.environ.get("RZ_BUILD_TYPE", "Release")
    project_root = Path(__file__).resolve().parent.parent
    bin_dir = project_root / "Binaries" / build_type
    if not bin_dir.exists():
        # fallback to Release if the requested build dir is absent.
        bin_dir = project_root / "Binaries" / "Release"
    cwd = str(bin_dir) if bin_dir.exists() else None

    # Symbol path: project Binaries (so cdb finds our private PDBs by DLL
    # location) BEFORE the Microsoft symbol server. Without the local path cdb
    # only queries the MS server, which never has our private symbols, so all
    # frames resolve to nearest-export names (wrong).
    sym_path = (
        f"srv*C:\\symbols*https://msdl.microsoft.com/download/symbols;"
        f"cache*C:\\symbols;{bin_dir}"
    )
    # Replace the placeholder -y value added above with the full path.
    for i, a in enumerate(cdb_cmd):
        if a == "-y":
            cdb_cmd[i + 1] = sym_path
            break

    print(f"[cdb_crash] launching: {' '.join(cdb_cmd)}")
    print(f"[cdb_crash] build={build_type} cwd={cwd}")
    print("=" * 80)

    result = subprocess.run(cdb_cmd, cwd=cwd)
    # cdb exit code: 0 = debuggee exited cleanly; non-zero = it caught something.
    print("=" * 80)
    print(f"[cdb_crash] cdb exit code: {result.returncode}")
    print("[cdb_crash] Look above for '=== Access violation caught ===' and the "
          "faulting thread stack (knL) to see which DLL/function crashed.")


if __name__ == "__main__":
    main()
