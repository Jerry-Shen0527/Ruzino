# Flame-graph profiling with py-spy (no admin, works on Release)

Quick "where does the time go" for any Python-driven workload that spends its
life in native code (render scripts, node-graph sims) — no elevated shell, no
special build, no VS UI. Validated 2026-09-16 on the WetBrush render loop
(`source/tests/render_wetbrush.py`); artifacts from that session are archived
in `Binaries/Release/test_output/wb_profile/`.

## Why not the VS / ETW profilers

- The VS 18 install ships only `VSPerfReport.exe` (report tool); there is no
  CLI collector (`VSPerfCmd`), and the IDE flame graph is GUI-only.
- Both VS sampling and ETW (`wpr` / xperf) require an **admin** shell; agent
  shells are non-admin (see AGENTS.md "Environment").
- py-spy needs no elevation and samples **native** stacks too (`--native`).

## The recipe

```bash
cd Binaries/Release
# NOT the python313 shim: scoop shims break tools that spawn/inspect the
# python process (py-spy dies with "Failed to find python version").
PY=/c/Users/Jerry/scoop/apps/python313/current/python.exe

# 1) Python-level phase split — zero setup, do this first.
WB_FRAMES=12 "$PY" -m cProfile -o wb_cprofile.out \
    ../../source/tests/render_wetbrush.py
#    read it with pstats: sort by cumtime, top entries are the phases.
#    Note: extension calls made through nanobind may be folded into the
#    caller's tottime — big tottime in the loop function means "inside
#    native code", go to step 2.

# 2) Native flame graph — SVG opens in any browser (same view as VS).
WB_FRAMES=10 py-spy record -o wb_flame.svg --format flamegraph \
    -r 100 --native --idle -- "$PY" ../../source/tests/render_wetbrush.py

# 3) Folded stacks for your own aggregation (per-module inclusive time,
#    leaf histograms). One "frame;frame;...;leaf" line per unique stack.
WB_FRAMES=8 py-spy record -o wb_stacks.txt --format raw \
    -r 100 --native --idle -- "$PY" ../../source/tests/render_wetbrush.py
```

`--idle` is important for GPU-bound workloads: threads blocked on fences /
`NtWaitForSingleObject` are exactly the signal (a large wait share = the GPU
is the bottleneck, the CPU is just babysitting it).

`render_wetbrush.py` knobs: `WB_FRAMES` (frames), `WB_SPP` (samples per
pixel, default 32), `WB_OUT_DIR` (output dir), plus the pen-shape/`WB_CAM_*`
variables documented in the script header. Keep frames low (8–12); sampling
is statistical, more frames only add wall time.

## Reading results with Release DLLs — symbolication caveats

Release DLLs carry no PDBs, so py-spy resolves unknown PCs to the **nearest
exported symbol** in the module. Treat such names as *module-level anchors,
not real functions*. Worked example from the 2026-09-16 session:
`nvrhi::d3d12::createDevice (nvrhi.dll)` showed 45% inclusive — impossible
for a one-shot init call, and the same "function" appeared under three
different parents (render loop, `brush_wb_commit`, `wb_sim`) always with leaf
`NtWaitForSingleObject`. Reality: unsymbolated internal nvrhi wait/fence code,
neighboring the `createDevice` export. What survives symbolication noise:

- **leaf frames in system DLLs** (`ntdll`, `KERNELBASE`, `win32u`) — reliable,
  and waits there are meaningful;
- **module-level inclusive totals** (aggregate a raw capture: a stack counts
  once for every module appearing in it);
- **python frames** (`render_wetbrush.py:450` etc.) — reliable.

Correct conclusion in that session: ~63% of samples were kernel waits → the
32spp path-traced render is GPU-bound; the largest real CPU consumer was the
sim tick (~22%, `brush_wb_commit` + `wb_sim`); ~7% was file stat'ing
(suspected per-frame shader hot-reload checks).

## A/B knob isolation

Confirm a hypothesis by varying one knob and comparing wall time:

```bash
time WB_FRAMES=12 WB_SPP=8 "$PY" ../../source/tests/render_wetbrush.py
```

SPP 32→8 took 3.5 s/frame to 2.1 s/frame: GPU work scales with SPP; the
remaining ~1 s/frame floor is sim tick + readback/PNG — consistent with the
flame graph. Per-frame math beats guessing.

## Disproving a symbol suspicion

When a PDB-less name looks implausible (e.g. `createDevice` dominating),
set a **cdb breakpoint on the export and count real hits** — works without
admin because we debug our own child process:

```bash
CDB="/c/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe"
"$CDB" -logo hits.log \
    -c 'bu nvrhi!nvrhi::d3d12::createDevice ".echo ===HIT_createDevice===; gc"; g' \
    "$PY" probe_script.py
```

Count the `===HIT===` lines *excluding the `bu ...` command echo at the top
of the log* (`grep -c` over- counts by exactly that line). Run the probe
with zero iterations of the suspected loop: 2026-09-16, an init + reset +
stop probe rendered 0 frames and hit `createDevice` exactly **once** (RHI
init, right after the nvapi/nvcuda ModLoads) — the "createDevice = 45% of
samples" flame-graph bar was unsymbolated nvrhi fence-wait code, not device
creation.

## When py-spy is not enough

Function/line-level attribution inside *our* DLLs needs symbols: build a
RelWithDebInfo slot (`pwsh -File scripts/build_devshell.ps1 -BuildType
RelWithDebInfo -BuildDir build-reldeb -Reconfigure`) and run the same scripts
against `Binaries/RelWithDebInfo` with `RZ_BUILD_TYPE=RelWithDebInfo`
(mirroring the Debug workflow in AGENTS.md). Only worth it when the hotspot
is real CPU work in our code — waits stay waits with or without PDBs.
