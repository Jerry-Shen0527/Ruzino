# Session Context Record — Wetbrush decomposition + rendering

> This file is a handoff note for context compression. It captures the full
> state of work as of 2026-07-02, so a compacted session (or a fresh machine)
> can resume without re-deriving everything. Keep it in the repo working tree
> (it is gitignored implicitly — not committed).

## Big picture

Two threads this session:

1. **Rendering pipeline (DONE, committed).** Got the hd_RUZINO path tracer to
   actually render the simulated translating box, end to end, including a
   60-frame animation sequence. Fixed black image + red NaN artifacts + moved
   to pure-Python in-process rendering.

2. **Wetbrush decomposition (DONE — physics lifted, verified, ALL COMMITTED &
   PUSHED).** Splitting the monolithic `brush_paint_sim` (2909 lines) into a
   simulation-zone chain of per-physics nodes. The full physics is now lifted
   1:1 from the monolith into 4 streaming nodes (deposit → bristle → fluid →
   commit) + an init-state seed node, and verified end-to-end: 14 sim/fidelity
   tests pass (gridbox 3 + repro 4 + wb-zone 3 + monolith fidelity 4), the
   canvas accumulates a real, finite, NaN-free stroke, and the monolith is
   untouched. See "Thread 2: Wetbrush decomposition — DONE" below.

### Commits this thread (all on `colbot`, pushed to origin)
- `95cca620` streaming Wetbrush decomposition skeleton (zone chain)
- `206bf147` resolve Wetbrush streaming tick-1 crash; zone feedback robust
- `2d931d79` lift full Wetbrush physics into streaming zone chain
- `301b2beb` isolate brush_capture tests from disk-cache leakage (test fix)
- `f8b04c82` Wetbrush point-cloud diagnostic + statistical analysis scripts

## Thread 1: Rendering — DONE

### What was fixed (root causes, not hacks)

1. **Black image = light direction.** `DistantLight` with no transform →
   `light.cpp:119,127` derives direction from transform's Z-row = identity
   `(0,0,1)` → light shines along +Z, from *behind* the box (visible faces
   face +Z toward camera). Fix: give the light a transform so Z-row points
   toward the visible faces (scene-level, in `render_gridbox.py`). NOT a
   triangulation bug (earlier misleading A/B test had blamed auto
   quad→triangle triangulation — disproved via shader diagnostics).

2. **Red NaN artifacts = smooth shading on hard edges.** ~0.8% pure-red pixels
   = path tracer's NaN-debug color (`path_tracing.slang:235`,
   `accumulated_color = float3(1,0,0)` when throughput is NaN/Inf). Confirmed
   via diagnostics: ALL red is at depth ≥1 (secondary bounces), sampled
   direction is never NaN (0% blue). Root cause: no authored normals →
   `Hd_RUZINO_Mesh` computes SMOOTH normals that blend to grazing directions at
   the box's sharp edges → MaterialX BSDF NaN. Fix at the SOURCE:
   `write_geometry_as_over_spec` (geometry submodule) now authors correct
   flat per-face (`faceVarying`) normals for every mesh, computed from
   `(p1-p0)×(p2-p0)`. So `normalW == faceNormalW` everywhere. The renderer's
   red debug color and ray bias were **left untouched** (user insisted: don't
   hack the renderer, fix the scene).

3. **In-process animation render.** `HydraRenderer::render(double time_code)`
   overload added (`renderer.cpp`) — old `render()` forwards time_code=0.
   `render_gridbox.py` rewritten to NOT fork `rz_render.exe`: stage loads
   once, render graph builds once, each frame rendered by passing a different
   time code. Hydra incrementally updates only animated primvars (logs:
   `geom_dirty=true, mat_dirty=false`). 60 frames → `Binaries/Release/box_sequence/`.

### Key files (rendering)
- `source/Runtime/renderer/python/renderer.cpp` — `HydraRenderer`, render(time_code)
- `source/Runtime/renderer/source/renderer.cpp` — texture extraction fix (RTTI name match)
- `source/Runtime/stage/python/stage.cpp` — tick/set_render_time bindings
- `source/tests/render_gridbox.py` — in-process animation renderer
- `source/Editor/geometry/usd_extension/usd_extension.cpp` — flat normals in write_geometry_as_over_spec
- `Binaries/Release/box_sequence/frame_0000.png`…`frame_0059.png` — the output

### Shader cache note
`ShaderFactory::cache_enabled = false` (`source/Core/RHI/source/shader.cpp:81,83`).
Comment says staleness check is broken so it's off. **Editing a .slang shader
takes effect on next render** (recompiled each run). Keep this in mind when
touching path_tracing.slang etc.

---

## Thread 2: Wetbrush decomposition — DONE (committed & pushed)

### COMPLETION SUMMARY (2026-06-29 lift; 2026-07-02 re-verified) — physics lifted & verified

The decomposition is **DONE**. All Wetbrush physics was lifted 1:1 from the
monolith (`node_brush_paint_sim.cpp`) into a streaming simulation-zone chain
that cooks correctly end-to-end. **14 sim/fidelity tests pass.**

**What shipped (uncommitted in working tree):**
- `brush_sim_common.hpp`: rewrote `WetbrushSimState` to carry the FULL
  persistent buffer set (3D window fields + 2D canvas + bristle + particle +
  compiled shaders), mirroring the monolith's `WetbrushState` field-for-field.
  The earlier "lean subset" (only density/color/wetness/oil_density/canvas)
  was physically wrong: the Stable-Fluids velocity field, bristle spring
  positions, and FLIP/PIC particles MUST persist frame-to-frame, or momentum,
  bristle dynamics and particle mass reset every tick. Fix at the source.
- **Deleted `WetbrushFrame`** (the old bundle of {stroke_curves, bp, state}).
  It mixed an ephemeral per-frame input with cross-frame accumulated state.
  The user flagged this ("WetbrushFrame 没在我们设计中出现过"); the clean
  design keeps only the paint FIELD on the boundary.
- **Deleted `node_brush_wb_entry.cpp`** (the Geometry→Frame packer, obsolete).
- **New `node_brush_wb_init_state.cpp`**: seeds the zone boundary's [State]
  slot with an empty field on the init frame (resolves the chicken-and-egg:
  multi-slot zone feedback needs every boundary slot filled, even frame 1).
- `node_brush_wb_deposit.cpp`: full buffer alloc + brush-pose frame
  finite-differencing + position_window (commit+clear old window) + deposit_at
  (bristle_simulate → density_constraint → resample → raster → merge) +
  sub-step loop. Lifted from monolith ~463-1478.
- `node_brush_wb_bristle.cpp`: §5.1 bristle↔particle liquid transfer/emit
  (ABSORB + EMIT, ping-pong). Lifted from monolith ~1525-1612.
- `node_brush_wb_fluid.cpp`: particle cycle (emit/update/raster/merge) +
  stable-fluids solve (advect/Jacobi/divergence/pressure/gradient/damp-dry) +
  post-fluid particle maintenance. Lifted from monolith ~1614-2566.
- `node_brush_wb_commit.cpp`: final canvas commit + readback stats + Paint
  Particles output (one point per painted canvas cell). Lifted from monolith
  ~2568-2904.
- `node_mock_point_emitter.cpp`: removed the obsolete `Frame` input; reads
  stroke from the `Stroke Curves` socket directly.

**New topology (the zone carries TWO boundary slots — verified the zone
supports multi-typed slots; the old "must be single-typed" was a misdiagnosis):**
```
mock_stroke --Stroke Curves--> [ simulation_in ]   (slot A: static input)
brush_wb_init_state --State--> [ simulation_in ]   (slot B: seed, frame 1 only)
  [ simulation_in ] --Stroke Curves--> mock_point_emitter (zone interior)
  mock_point_emitter --BrushPoint--> brush_wb_deposit   (interior, NOT boundary)
  [ simulation_in ] --State--> brush_wb_deposit
  brush_wb_deposit --State--> brush_wb_bristle --State--> brush_wb_fluid
    --State--> brush_wb_commit
  brush_wb_deposit --BrushPoint--> brush_wb_fluid  (so fluid knows pen up/down)
  [ simulation_in ] --Stroke Curves--> brush_wb_commit (stroke passthrough —
    the zone group-sync mirrors every slot to sim_out, so commit forwards it)
  brush_wb_commit --Paint Particles--> write_usd   (interior)
  brush_wb_commit --State-->        [ simulation_out ]   (fed back)
  brush_wb_commit --Stroke Curves--> [ simulation_out ] (fed back, re-fed)
```

**Verification:** `test_wetbrush_zone.py` has 3 tests — graph builds, sim runs
12 frames without crash, AND a physics-correctness test that reads the final
frame's painted points off the stage (non-empty, no NaN/Inf). Per-frame C++
logging during dev confirmed density≈12, mean divergence≈0.001, 10240 particles
— all finite, matching the monolith's character.

**Gotchas hit & fixed (so you don't re-derive):**
1. MSVC's `initializer_list` deduction chokes on `RefCountPtr<IBuffer>*`
   element types (C2440 "IBuffer** → BufferHandle*"). Don't loop
   `for (BufferHandle* b : {&field->x, ...})` — use a variadic helper
   `auto f=[&](auto&... bufs){(g(bufs),...);};`.
2. Multi-slot zone feedback: every boundary slot must be filled on the init
   frame or `simulation_in` is skipped ("missing required input"). Seed the
   field slot with `brush_wb_init_state`; carry the stroke slot through commit
   (group-sync mirrors it to sim_out, which then needs it filled too).
3. Optional socket reads: the executor sets an unwired optional input's pointer
   to `nullptr`. NEVER call `get_input` on it — guard with `has_input` first
   (deposit's `State` is optional; absent on the init frame).
4. Stage-tick test can't read node output ports via `g.getOutput`/`get_output`
   (different executor path, returns NaN). Read the Paint Particles that
   write_usd baked into the in-memory stage's time-sampled `points` attr
   instead — that's the source of truth.

### Goal
Split `brush_paint_sim` (monolith, `source/Editor/geometry_nodes/node_brush_paint_sim.cpp`, 2909 lines)
into a simulation-zone chain:
```
brush_wb_deposit → brush_wb_bristle → brush_wb_fluid → brush_wb_commit
```
Each node = one Wetbrush physics stage. State shared via a lean
`WetbrushSimState` socket value; the simulation zone feeds it back frame-to-frame.

### State design rule (USER-APPROVED, IMPORTANT)
A field goes into `WetbrushSimState` **iff accessed by ≥3 of the 4 nodes**, OR
it's the persistent canvas layer. Otherwise:
- 1-node field → `rc.create()` (resource allocator, auto-recycled every cook)
- 2-node field → regular socket (e.g. `BristleSampleOutputs`, `Height Field`)

**Buffer access matrix (verified against monolith line numbers):**
| Field | deposit | bristle | fluid | commit | →归属 |
|-------|:-:|:-:|:-:|:-:|------|
| density, color_r/y/b | W | R | RW | R | **State (4)** |
| wetness | ctx | RW | RW | R | **State (4)** |
| oil_density | alloc | R/W | RW | — | **State (3)** |
| canvas_density/color_r/y/b/wetness | — | — | — | RW | **State (persistent)** |
| grid control + brush kinematics | — | — | — | — | **State (control)** |
| vel_x/y/z, pressure_a/b, divergence, *_tmp | — | — | RW | — | rc.create (1) |
| height_field | W | R | — | — | socket (2) |
| sample_pos/color/frame | — | W | R | — | socket (2) |
| ptcl_* | — | — | RW | — | rc.create (1) |

The legacy `WetbrushState` (full ~60 buffers) is **left unchanged** for the
monolith — used as the parity baseline. Do NOT modify it.

### What's committed (skeleton)
- `brush_sim_common.hpp`: added `WetbrushSimState` (lean) + `BristleSampleOutputs`
  + `WetbrushFrame`. Added `grid_alloc_res`/`grid_alloc_res_z` to WetbrushSimState.
- `node_brush_wb_deposit.cpp` / `_bristle` / `_fluid` / `_commit.cpp`: skeletons,
  all `ALWAYS_DIRTY`, socket-complete, physics stubbed (`emit_empty`).
- `node_brush_wb_entry.cpp`: packs Geometry→WetbrushFrame (zone boundary must be
  single-typed).
- `node_mock_point_emitter.cpp`: added optional `Frame` (WetbrushFrame) input;
  reads `frame.stroke_curves` if wired, else raw "Stroke Curves".
- `source/tests/test_wetbrush_zone.py`: builds the graph, drives stage.tick.
- commit `95cca620` "feat(brush): streaming Wetbrush decomposition skeleton".

### THE BLOCKER: tick-1 crash — RESOLVED (2026-06-29)

**Symptom:** `test_streaming_simulation_runs` crashed on **tick 1** with
access violation inside `stage.tick`. Tick 0 (init frame) succeeded; tick 1
crashed before any node exec ran. Serialization / apply_to_stage were fine.

**Resolution method:** wrote a minimal reproducer
(`source/tests/test_sim_zone_repro.py`) that bisects the working
`test_sim_gridbox` topology one variable at a time:
- **A** (write_usd OUTSIDE the zone) — WORKS (the gridbox shape).
- **B** (write_usd INSIDE the zone as a sink, branching off transform) —
  **REPRODUCES the tick-1 crash.**
- **C/D** (3/4-node interior chain, NO write_usd) — WORKS.

This proved the crash was NOT about `WetbrushFrame` (all variants carry a
plain `Geometry`) — it was the zone topology + REQUIRED propagation.

**Root cause (two independent bugs, both fixed):**

1. **`simulation_out` did not always execute.** The zone feedback move
   (`node_exec_eager.cpp:~356`, `simulation_in->storage = std::move(node->storage)`)
   only runs when `simulation_out` executes. In the wb graph `write_usd` is an
   interior sink branching off `commit`, so nothing REQUIRED was downstream of
   `sim_out` → `sim_out` was never marked REQUIRED → it never ran → feedback
   storage stayed empty → tick 1's `simulation_in` replayed an empty/invalid
   `SimulationStorage` → access violation.
   **Fix:** added `NODE_DECLARATION_REQUIRED(simulation_out)` in
   `simulation_zone.cpp`. This is correct in general: the zone's feedback node
   MUST always run to populate next-frame storage, regardless of downstream
   demand. Variant B went from crash → 5/5 frames.

2. **`brush_wb_deposit` / `brush_wb_bristle` had a required `glm::vec3`
   "Ink Color" input that can't carry a `default_val`** (vec3 sockets can't be
   defaulted through serialization), so it was MISSING_INPUT → deposit skipped
   → the whole wb chain skipped → `sim_out` got no input → (same empty-storage
   crash path). The `execute_node` diagnostics (now a permanent warn log on
   MISSING_INPUT) named the culprit socket immediately.
   **Fix:** made "Ink Color" `.optional(true)` on both nodes (exec applies a
   red fallback when unwired). Also made `mock_point_emitter`'s "Stroke Curves"
   `.optional(true)` — in the zone path the curves arrive via the "Frame"
   input, so the raw Geometry socket must not be required.

**After both fixes:** `test_wetbrush_zone.py` cooks all 10 frames, every wb
node runs every tick (`brush_wb_deposit: advance pos=...` shows the brush
moving), `sim_out` stores the feedback each frame. All 9 sim tests pass
(gridbox 3 + repro 4 + wb zone 2). **The streaming chain is unblocked — ready
to lift physics node-by-node.**

### Key files (decomposition, now unblocked)
- `source/Editor/geometry_nodes/simulation_zone.cpp` — sim_in/out exec +
  `NODE_DECLARATION_REQUIRED(simulation_out)` (the zone feedback node must
  always run)
- `source/Core/rznode/core/node_exec_eager.cpp:~213` — `execute_node`, now logs
  `[exec] node '<id>' skipped: missing required input [<names>]` on
  MISSING_INPUT (permanent diagnostic; the feedback move is at ~356)
- `source/Core/rznode/core/node_exec_eager.cpp:166-201` — `prepare_params`
  (MISSING_INPUT set at 198)
- `source/tests/test_sim_zone_repro.py` — the bisect reproducer (A/B/C/D);
  keep as a regression guard for zone topology
- `source/Editor/geometry_nodes/node_brush_wb_deposit.cpp` /
  `node_brush_wb_bristle.cpp` — "Ink Color" now `.optional(true)`
- `source/Editor/geometry_nodes/node_mock_point_emitter.cpp` — "Stroke Curves"
  now `.optional(true)` (zone path reads curves from "Frame")

### Key files (decomposition blocker)
- `source/Core/rznode/core/node_exec_eager.cpp:213-229` — `execute_node`,
  MISSING_INPUT check (line 220), the feedback move (line ~354-357)
- `source/Core/rznode/core/node_exec_eager.cpp:166-201` — `prepare_params`
  (MISSING_INPUT set at 198)
- `source/Core/rznode/core/node_exec_eager.cpp:232-300` — `forward_output_to_input`
  (type mismatch handling at 278-289)
- `source/Editor/geometry_nodes/simulation_zone.cpp` — simulation_in/out exec
  (init forwards upstream, advance replays storage)
- `source/Runtime/stage/source/animation.cpp:119-275` — stage tick /
  update_modifier_stack (deserialize + execute path)
- `source/Core/rznode/core/include/nodes/core/api.hpp:36-57` — get_socket_type /
  register_cpp_type (auto type registration — user says this is fine)
- `source/Core/rznode/python/ruzino_graph.py:216-284` — createSimulationZone
  (add_sync_group pairs all 4 boundary groups — forces single typed slot)
- `source/Core/rznode/python/ruzino_graph.py:688-783` — apply_to_stage (serialize
  to node_json, set GeomPayload)

### Uncommitted working-tree changes (the WetbrushFrame refactor)
- `brush_sim_common.hpp`: WetbrushFrame struct (with stroke_curves/bp/state)
- `node_brush_wb_deposit.cpp`: reads WetbrushFrame "Frame" input, allocates
  SimState, has buffer allocation code lifted from monolith (compiles)
- `node_brush_wb_bristle.cpp` / `_fluid` / `_commit.cpp`: rewritten to take
  WetbrushFrame
- `node_brush_wb_entry.cpp`: NEW packer node
- `node_mock_point_emitter.cpp`: optional Frame input
- `test_wetbrush_zone.py`: markers added (MARKER: before/after apply_to_stage,
  before/after tick i) for crash localization

**If reverting:** the last committed state is `95cca620` (skeleton with the
old dual-type SimState wiring). The uncommitted WetbrushFrame refactor is an
attempt to fix the no-cook bug; it didn't fully work but is closer.

---

## User preferences / process notes

- **Don't hack the renderer to mask symptoms.** When the box had red NaN
  artifacts, user explicitly said: don't remove the debug color, don't use
  oversized ray bias — fix the scene (flat normals at the source). Applied.
- **Fix things at the source, not downstream.** Flat normals belong in
  `write_geometry_as_over_spec` (so every consumer benefits), not in the
  render script.
- **Out-image verification rule:** programmatically check for degenerate
  results (tiny file / all-black / all-white / all-red); if "has content",
  STOP and let the user eyeball it. Don't trust the vision MCP for subtle
  judgments.
- **State design:** only ≥3-node-shared fields go in shared state; everything
  else is allocator-local or socket-carried. User corrected this twice (first
  "persistent + control", then tightened to "≥3 nodes").
- **Build:** repo-root `.bat` scripts (`build.bat`, `reconfig.bat`) load MSVC
  themselves. I use a `_build_one.bat` helper (gitignored, recreated as needed)
  that runs ninja with a target and captures output to `_build_out.txt`. After
  adding new node `.cpp` files, run `reconfig.bat` (CMake reglob) then build;
  **also build `geometry_nodes_json_target`** or the new nodes won't appear in
  `Binaries/Release/geometry_nodes.json`.
- **nvrhi submodule:** upstream, can't push. Local uncommitted content there is
  expected; `format_and_commit_manager.py` skips it. I commit manually:
  format with `clang-format` (scoop llvm at `~/scoop/apps/llvm/current/bin`),
  depth-first submodules then root, skip `.zcode/` (local agent state) and
  `nul`.
- **Weekly reports:** `C:\Users\Jerry\Obsidian\Weekly\`. `gen_report.py`
  (ReportLab, Times font, Finished/Ongoing/Todo sections) generates PDF from
  inline content; there's a matching `.md`. Just did `Weekly Report 2026-06-29`.

## Test runner
- `python scripts/run_all_tests.py <name>` from repo root; finds
  `source/*/tests/test_*.py` and `Binaries/Release/*_test.exe`. Python tests
  need `Binaries/Release` in path (conftest sets it up). Brush/wb tests run
  from there so node DLLs resolve.
- `test_sim_gridbox.py` is the REFERENCE working simulation-zone test (single
  Geometry slot, accumulates X to 6.0). `test_wetbrush_zone.py` is the
  Wetbrush streaming-zone test (physics lifted — 3 tests, all passing).
- **brush_capture test leak (FIXED 2026-07-02, commit `301b2beb`):**
  `brush_capture` persists its trajectory to `brush_capture_cache.bin` on every
  cook and reloads it on the first cook of a session, so points from one test
  leaked into the next. Added an autouse `_clean_capture_cache` fixture that
  clears the cache before each test. This was the old "pre-existing failure"
  noted here previously — now resolved.

---

## 2026-07-02 session notes (re-verification + point-cloud stats)

### What was done today
1. **Re-verified the whole wb test suite is still green** (not just trusting
   the 6-29 record): ran `test_wetbrush_zone` (3) + `test_sim_gridbox` (3) +
   `test_sim_zone_repro` (4) + `test_brush_sim_fidelity` (4) = **14 pass**.
2. **Committed `301b2beb`** — the brush_capture disk-cache isolation fixture
   + test_brush_sim_replay degenerate-capture skip (the two test files were
   sitting uncommitted in the working tree).
3. **Committed `f8b04c82`** — two new diagnostic scripts under `source/tests/`:
   - `inspect_wetbrush_points.py` — runs 12-frame wb zone, exports `.usdc`,
     prints per-frame point accumulation + final-frame bbox / NaN check.
   - `stats_wetbrush_points.py` — reads that `.usdc`, reports deep statistics
     (accumulation deltas, cell quantization, density/fill, displayColor
     spread, Z paint-layer thickness, PCA stroke shape).
   Both run from `Binaries/Release` and write to `source/tests/data/output/`.
4. **Point-cloud statistical verification** (final frame, 600 painted points):
   - monotonic accumulation (0→600, delta mean 54.5, no regressions)
   - Z paint-layer span = 0.0061 = 1.5 cells, all `z>0` (thin film on paper)
   - PCA minor/major axis ratio = 0.34 → elongated line/curve stroke (not blob)
   - 600 raw pts → 211 unique grid cells (389 dup = overlapping deposits, OK)
   - displayColor: 594 distinct colors, luminosity 0.30~0.93 → real
     concentration gradient (center dark, edge light), not flat fill
   - no NaN/Inf
   All consistent with a physically valid wet-brush sinusoidal stroke.

### Remaining work (the open kanban card)
The "逐帧物理统计对齐到单体基线" card is **partially done**. The 7 paper
constants (β_B, spring_k, D0, D1, flip_gamma, μ, ε) have been lifted into the
streaming nodes with the values listed in that card's table, but a systematic
per-constant streaming-vs-monolith diff hasn't been run yet — current
verification is "character matches", not "every constant identical". That diff
is the remaining work on that card.

### Diagnostic scripts caveat
`inspect_wetbrush_points.py` and `stats_wetbrush_points.py` are NOT pytest
tests (no `test_` prefix) — they're standalone diagnostics. They need the pxr
path setup that `source/tests/conftest.py` normally provides. Run them like:
```
cd Binaries/Release
python ../../source/tests/inspect_wetbrush_points.py   # needs conftest paths
```
If `import stage_py` fails when run directly, it's the path — invoke via the
conftest mechanism or set `PYTHONPATH`/`PXR_USD_WINDOWS_DLL_PATH` manually
(see how `stats_wetbrush_points.py` does `sys.path.insert` + env at top).

---

## Switching machines (2026-07-02 → tomorrow, different box)
- All wb work is **committed and pushed** to `origin/colbot`. `git pull` on the
  new machine gets everything. No uncommitted wb state anywhere.
- The new machine needs a **full build** before tests can run: the wb node
  DLLs (`node_brush_wb_*.dll`) and `geometry_nodes.json` are build artifacts,
  not in git. After `git pull`: `build.bat` (or `scripts/build_and_install.py`),
  and confirm `Binaries/Release/node_brush_wb_deposit.dll` etc. exist + the 5
  wb nodes are listed in `Binaries/Release/geometry_nodes.json`.
- Verify the new-machine build with: `python scripts/run_all_tests.py
  test_wetbrush_zone` (from `Binaries/Release`). 3 tests should pass.
- `SESSION_CONTEXT.md` and `.zcode/` are local-only (not committed); they won't
  carry over. This file is the handoff — if you lose it, the kanban cards
  (`~/Obsidian/Kanban/docs/...`) + `git log origin/colbot` reconstruct it.
- **Kanban updated** (in `~/Obsidian/`, separate git repo synced separately):
  - `把单体求解器拆成 deposit→bristle→fluid→commit 四节点 zone 链...` →
    **Agents Finished** (High, 2026-07-02)
  - `把流式管线逐帧物理统计对齐到单体基线...` → **Ongoing** (Medium) — the
    remaining per-constant diff work lives here.
