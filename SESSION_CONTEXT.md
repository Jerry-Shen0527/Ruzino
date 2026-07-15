# Session Context Record — Wetbrush rendering pipeline (Part A + Part B)

> Handoff note for context compression. Captures the full state as of
> 2026-07-15. The kanban doc
> (`~/Obsidian/Kanban/docs/把流式仿真接到离屏路径追踪渲染，让逐帧笔触动画能渲染成图片序列（Ongoing）.md`)
> has the paper-discussion conclusions; this file has the implementation state.

## Big picture

Building the Wetbrush §6 volume rendering pipeline in two parts:

1. **Part A (DONE, committed `f8321079` + `361a7bac`).** Refactored the sim
   storage model: 3D fluid fields from window-sized to **global grid**
   (512×512×32), deleted the 2D canvas layer + canvas_commit (paper footnote 1
   rejects 2D height-field), fixed a conservation bug in bristle_merge, and
   fixed a density explosion (W kernel dV factor). Sim is stable: 60 frames,
   max_density ≈ 0.10.

2. **Part B (DONE mechanically, rendering has a pending shader-tuning bug).**
   Built `SharedGPUBufferRegistry` — a generic cross-module GPU buffer sharing
   mechanism. commit node packs density/color into a Float4 buffer and registers
   it; render rprim looks it up (zero-copy, bypasses USD primvar round-trip).
   Registry hits successfully every frame. **But rendered PNGs are all black**
   due to a Beer-Lambert sigmaScale/step-size mismatch (see "Pending bug" below).

## Paper discussion conclusions (2026-07-14, all 5 vague points resolved)

These conclusions drive the architecture. Full reasoning in the kanban doc.

1. **Density semantics:** single-cell instantaneous concentration (0.1
   magnitude), NOT a monotonic accumulator. Threshold is data-driven (run sim,
   look at histogram, pick a percentile) — paper doesn't give a number because
   it depends on the sim's scale.
2. **Drying:** independent mechanism — dryness reaches threshold → lock
   velocity (treat as solid cell). Does NOT clear density. Dried paint stays
   in the grid.
3. **Surface definition (§6):** first-cross (density > threshold) → normal from
   density gradient → oil density determines penetration distance → blend
   pigment within penetration. NOT full Beer-Lambert integration. Penetration
   formula not given explicitly; oil-density-proportional, constant tuned.
4. **Active window + persistence:** ONE large 3D grid (entire canvas) is the
   persistent store. Window (128×128×32) is only the per-frame compute region.
   No commit, no 2D layer, no data movement. "Redraw active window only" = only
   that region changes per frame, rest keeps previous values.
5. **Particle rendering:** §6's van der Laan 2009 screen-space metaball is an
   OPTIONAL enhancement layer for interactive splatter/filaments. Not blocking.
   Grid volume rendering covers the main stroke body.

## Conservation chain (Eq.15/16) — the source of density

```
bristle sample (m_j) ──emission(Eq.14)──▶ particle (m_k)
                                              │
                    ┌─────────────────────────┘
                    ▼ Eq.16 (ρ += m_k·ΣW·dV)    particle→grid
             grid cell density (ρ_c)
                    │
                    ▼ Eq.15 (ρ -= m_k·ΣW·dV)    grid→particle
                    │
               advection (transport, no gain/loss)
                    │
               dryness (lock velocity, keep density)
```

Paint source: `sample_supply` buffer, refilled to `ink_amount` each frame in
deposit node (L872 "re-dip"). This is correct — user keeps dipping the brush.

## Part A details (global grid + conservation)

### What changed (committed `f8321079`)

- **`brush_sim_common.hpp`**: deleted `canvas_density/color_r/y/b/wetness` +
  `canvas_commit_program`. 3D fields注释 changed to "global grid".
- **`node_brush_wb_deposit.cpp`**: `alloc_win_n3d` = `resolution³` (global).
  Deleted `make_canvas`, canvas alloc, canvas zero-init. `position_window`
  gutted — only computes origin, no commit/clear.
- **`common.slangh`**: `window_map` returns **global** index
  (`grid_idx_3d(x,y,z, res, res_z)`), not window-local. `window_local_idx`
  returns global index, false only if outside global grid.
- **4 fluid stencil shaders** (divergence/jacobi/gradient/advect): neighbor
  strides `WIN`→`cb.res`, `WNN`→`cb.res*cb.res`. Boundary checks `lx`→`c.x`.
  Advect trace-back in global coords, clamp to `[0,res)`.
- **8 world→grid shaders** (brush_deposit/bristle_rasterize/bristle_merge/
  particle_rasterize/particle_to_grid/grid_to_particle/particle_flip_pic/
  bristle_liquid_transfer/particle_update): deleted window-local conversion,
  index directly into global grid.
- **`node_brush_wb_commit.cpp`**: deleted canvas_commit dispatch + 2D readback.
  "Paint Particles" port emits empty geometry (socket compat). "Paint Field 3D"
  iterates the FULL global grid (not just window). Readback uses `grid_n3d`.
- **`canvas_commit.slang`**: deleted.
- **`render_wetbrush.py`**: max-accumulate → last-write (global grid is
  persistent, each frame's window data is the current state).

### Conservation fix (`f8321079`)

`bristle_merge.slang:65`: `density[idx] = min(density[idx] + bd *
cb.ink_amount, 3.0)` → `density[idx] = density[idx] + bd`. The `bd` is already
the mass-weighted rasterized density (m_k·ΣW); multiplying by ink_amount again
was a net-injection bug. The `min(...,3.0)` hard cap was a band-aid for this;
removed.

### Density explosion fix (`361a7bac`)

**Root cause:** `W_smooth_3d` at r≈0 returns ~317,000 (normalization divides by
h³ ≈ 2e-7 for cell-size h). `particle_to_grid` (Eq.16) did `density += m_k·W`
without the cell-volume factor → single particle injected 158K density →
explosion at frame 31 (single-frame jump from 0.15 to 2.9M). Velocity was
unaffected (0.002) — it was a direct density injection, not advection-driven.

**Fix:** multiply both Eq.15 and Eq.16 by `dV = cell_size² × cell_z`. Brings
kernel contribution from 317K to ~0.02. Verified: 60 frames stable,
max_density=0.10.

**Why it was masked before:** `bristle_merge`'s `min(...,3.0)` cap truncated
the merge path, but `particle_to_grid` (a different shader) was never capped
and silently injected huge values. Removing the cap (conservation fix) exposed
it when the global grid let the explosion accumulate.

## Part B details (SharedGPUBufferRegistry — zero-copy sim→render)

### Architecture (user-approved design principles)

- **Registry is generic / semantics-free:** `SharedGPUBufferRegistry` maps
  `string key → (BufferHandle, byteSize, version, opaque metadata blob)`. It
  lives in `source/Core/RHI/` (both sim and render link RHI). Does NOT know
  about density, gridRes, Wetbrush — those are in the opaque metadata blob,
  interpreted by the two sides out-of-band.
- **Metadata agreement:** both sides define an identical POD struct
  `{uint32 resX,resY,resZ; float cellSize; float gridMinX,Y,Z}` and pass it as
  the metadata blob. This is Wetbrush-layer knowledge, not RHI-layer.
- **Two-phase lookup in rprim:** `create_gpu_resources` checks registry first.
  Hit → use external buffer (skip createBuffer + writeBuffer), pull grid dims
  from metadata blob. Miss → fall back to primvar path (unchanged).

### Files

- `source/Core/RHI/include/RHI/shared_buffer_registry.hpp` — generic registry
  (Entry: buffer + byteSize + version + metadata vector<uint8_t>). RHI_API
  exported, Meyers singleton in shared_buffer_registry.cpp.
- `source/Core/RHI/source/shared_buffer_registry.cpp` — singleton impl.
- `source/Editor/geometry_nodes/BrushSimulation/shaders/pack_float4.slang` —
  reads density/color_r/y/b (4 float buffers) → writes Float4 packed_out.
- `brush_sim_common.hpp`: added `packed_paint` (BufferHandle, Float4, global
  grid sized) + `pack_program`.
- `node_brush_wb_deposit.cpp`: allocates `packed_paint` via
  `brush_create_typed_buffer(alloc_win_n3d, sizeof(float)*4, ...)`. ensure_prog.
- `node_brush_wb_commit.cpp`: dispatches pack_float4, registers packed_paint
  to `SharedGPUBufferRegistry` with key `"wetbrush_paint_field"` + metadata blob.
- `wetbrush_volume.h/cpp`: `registryVersion` member. `create_gpu_resources` does
  two-phase lookup. `Sync` checks registry version to trigger re-bind.
  Includes `RHI/shared_buffer_registry.hpp`.

### Key = `"wetbrush_paint_field"` (fixed string)

Not prim_path — because render rprim's SdfPath (`/BrushPaint`, created by
Python bake) differs from sim's prim_path (`/Brush`). Only one Wetbrush sim
runs at a time, so a fixed key is fine.

### Test architecture caveat

`render_wetbrush.py` runs sim (stage 1) → bake (stage 2) → render (stage 3)
as SEPARATE loops. The sim graph `g` must be kept alive until render finishes
(added `return ..., g` from `run_streaming_zone`, held in `main`). Otherwise
`WetbrushSimState` is GC'd, `packed_paint` freed, registry has dangling handle.
All render frames see v60 (sim's final frame) — this is a test-architecture
limitation, not a design flaw. In production, sim and render run interleaved
per-frame.

## Pending bug: rendered PNGs are all black

### Bug 1 (Part A leftover, FIXED): shader field references

`wetbrush_render.slang` referenced `vd.gridPaper` and `vd.gridRes` which DON'T
EXIST in the VolumeDesc struct (Part A changed it to `gridResX/Y/Z` +
`cellSize`). Slang silently defaults missing struct fields to 0 → `cell_sz =
0/0 = NaN` → raymarch produces nothing. **Fixed:** changed to `vd.cellSize`
(2 occurrences in VolumeClosestHit + VolumeShadowHit) and `vd.cellSize * 10.0`
for aoRadius. Must copy fixed shader to
`Binaries/Release/usd/hd_RUZINO/resources/shaders/wetbrush_render.slang`.

### Bug 2 (NOT fixed): Beer-Lambert sigmaScale vs step vs layer thickness

After Bug 1 fix, still all black. Root cause is numerical:
- Paint layer Z thickness ≈ 3 cells (0.004m). Raymarch step = cellSize × 0.25
  = 0.001. So ~4 steps through the paint.
- `sigma_t = density × sigmaScale = 0.1 × 8.0 = 0.8`
- `stepAlpha = 1 - exp(-0.8 × 0.001) = 0.0008` per step → effectively transparent
- 4 steps accumulate ≈ 0.003 extinction → invisible

`sigmaScale = 8.0` was tuned for the old 2D slab (much thicker). The 3D grid's
paint is thin (few Z layers). Fix options:
- Increase sigmaScale dramatically (1000+?) — hacky, needs empirical tuning.
- Re-examine paper §6: it says "first-cross surface + penetration blend", NOT
  full Beer-Lambert. The current shader does full integration which produces
  near-zero for thin layers. Should switch to first-cross + surface emission
  model (find density>threshold, emit color there, modulate by penetration).
- The current VolumeClosestHit (Beer-Lambert) doesn't match paper §6's described
  algorithm. This is the render correctness work that remains.

### What IS verified working

- Registry mechanism: every frame logs `registry hit (v60, 256x256x32,
  33554432 bytes, bindless=0)`. The external buffer is bound correctly.
- Sim produces valid density (max 0.10, 2870 painted voxels in bake).
- Pipeline runs end-to-end (60 frames render, just black due to Bug 2).
- PIL (Pillow) is now installed in the bundled python (was missing, blocked
  the render loop).

## Render pipeline sequence (current)

```
Stage 1 (sim loop, 60 frames, RuzinoGraph zone):
  deposit → bristle → fluid → commit
  commit每帧:
    1. pack_float4 dispatch (density/color_r/y/b → packed_paint Float4)
    2. SharedGPUBufferRegistry.register("wetbrush_paint_field", packed_paint, meta)
    3. readback全grid → emit Paint Field 3D点云 → write_usd → USD time samples

Stage 2 (bake, once):
  读USD点云 → rasterize成 UsdVolVolume paintField primvar (fallback用)

Stage 3 (render loop, 60 frames, HydraRenderer):
  WetbrushVolume rprim.Sync:
    → 查 SharedGPUBufferRegistry("wetbrush_paint_field") → HIT
    → 用外部buffer注册bindless descriptor (零拷贝)
    → gridRes/cellSize/gridMin 从 registry metadata blob读
  wetbrush_render节点 dispatch:
    → VolumeIntersection (AABB ray test)
    → VolumeClosestHit (Beer-Lambert raymarch) ← Bug 2: 产出全黑
```

## Key files index

| Concern | File | Notes |
|---------|------|-------|
| Registry (generic) | `source/Core/RHI/include/RHI/shared_buffer_registry.hpp` | key→buffer+meta |
| Registry impl | `source/Core/RHI/source/shared_buffer_registry.cpp` | Meyers singleton |
| Pack shader | `source/Editor/geometry_nodes/BrushSimulation/shaders/pack_float4.slang` | 4 float→1 float4 |
| Sim state | `source/Editor/geometry_nodes/brush_sim_common.hpp` | WetbrushSimState + packed_paint |
| Sim nodes | `node_brush_wb_{deposit,bristle,fluid,commit}.cpp` | global grid, no canvas |
| Shader indexing | `BrushSimulation/shaders/common.slangh` | window_map returns global idx |
| Render rprim | `source/Runtime/renderer/source/geometries/wetbrush_volume.{h,cpp}` | two-phase lookup |
| Render node | `source/Runtime/renderer/nodes/wetbrush_render.cpp` | SBT + volume hit groups 4/5 |
| Volume shader | `source/Runtime/renderer/nodes/shaders/shaders/wetbrush_render.slang` | Bug 2 lives here |
| Volume helpers | `source/Runtime/renderer/nodes/shaders/shaders/volume_intersection.slang` | samplePaintField, intersectSlab |
| VolumeDesc | `source/Runtime/renderer/nodes/shaders/shaders/Scene/SceneTypes.slang` | gridResX/Y/Z + cellSize |
| Test driver | `source/tests/render_wetbrush.py` | 3-stage: sim→bake→render |

## Build commands

- `build.bat RHI` — after touching shared_buffer_registry.{hpp,cpp}
- `build.bat node_brush_wb_deposit` / `_commit` / `_fluid` / `_bristle`
- `build.bat hd_RUZINO` — after touching wetbrush_volume.{h,cpp}
- `build.bat wetbrush_render` — render node (if shader entry points change)
- `reconfig.bat` — after adding NEW .cpp files (CMake reglob)
- **Shaders are runtime-compiled** (not build-time). Edit `.slang` in
  `source/Runtime/renderer/nodes/shaders/shaders/`, but ALSO copy to
  `Binaries/Release/usd/hd_RUZINO/resources/shaders/` (the deployed copy the
  renderer loads). ShaderFactory cache is disabled (always recompiles).

## Run command

```
cd Binaries/Release
python ../../source/tests/render_wetbrush.py
```
Outputs to `Binaries/Release/wetbrush_sequence/frame_XXXX.png` (currently all
black due to Bug 2). Logs go to stderr (spdlog).

## Commits (on `colbot`)

- `f8321079` Part A: global 3D grid + remove 2D canvas + conservation fix
- `361a7bac` fix: cell-volume factor in particle-grid transfer (Eq.15/16)
- `ca6f68ec` feat: Hd_RUZINO_WetbrushVolume raymarch rprim (paper §6)
- Part B (registry + pack + two-phase lookup): **uncommitted in working tree**

## User preferences (still valid)

- **Don't hack the renderer to mask symptoms.** Fix at the source.
- **Don't pollute generic layers with domain semantics.** Registry must stay
  clean (user corrected this: "RHI或者Stage模块应该不要被wetbrush的内容污染").
- **Follow the paper, ask before hacking.** The Beer-Lambert model in the shader
  doesn't match paper §6's "first-cross + penetration blend" — this needs to be
  reconciled, not band-aided with a giant sigmaScale.
- **Out-image verification:** check for degenerate (all-black/all-white) before
  trusting render output.
- Performance is NOT a priority yet — user will do profiling themselves later.

## Next steps (when resuming)

1. **Fix Bug 2 (rendering all-black).** The VolumeClosestHit shader should be
   reworked to match paper §6: first-cross surface detection (density >
   threshold) + normal from gradient + pigment emission at surface +
   oil-density-modulated penetration blend. The current Beer-Lambert full
   integration is wrong for thin paint layers. This may need an
   EnterPlanMode + paper re-read.
2. **Commit Part B** (registry + pack + two-phase lookup) once rendering
   produces visible output.
3. **Simplify render_wetbrush.py** — once registry path is verified, the Python
   bake stage can be removed (metadata still needs to be written as primvars
   for the fallback path, but the heavy paintField primvar upload can be
   skipped).
4. **Per-frame interleaving** — the test currently runs sim-then-render as
   separate loops (all frames see v60). For proper animation, sim and render
   should interleave per-frame. This is a test-architecture change, not a
   production blocker.
