#!/usr/bin/env python3
"""Volume checkpoint — persist a live grid-field simulation state into USD.

Two halves of the checkpoint round-trip, tested independently:

1. ``test_sim_checkpoint_authors_primvars`` (save path): drive the real
   WetBrush sim zone for a few ticks (this registers the packed
   (density,r,g,b) Float4 buffer under "grid_field/paint" in the
   SharedGPUBufferRegistry), then call
   ``HydraRenderer.save_volume_checkpoint`` and assert the exported root
   layer carries the grid metadata + paintField primvars with real paint in
   them — pure data assertions, no rendering.

The load-path counterpart (rendering checkpoint primvars with no sim)
lives in test_volume_checkpoint_load.py — it must run in a process where NO
simulation ever ran, so the process-global "grid_field/paint" registry entry
can't shadow the primvar fallback path.
"""


import os
import sys
from pathlib import Path

import numpy as np
import pytest


def _prepare_env():
    script_dir = Path(__file__).parent.resolve()
    workspace_root = script_dir.parent.parent.parent.parent
    binary_dir = workspace_root / "Binaries" / "Release"

    os.environ.setdefault('PXR_USD_WINDOWS_DLL_PATH', str(binary_dir))
    os.environ['PATH'] = str(binary_dir) + os.pathsep + os.environ.get('PATH', '')
    if hasattr(os, 'add_dll_directory'):
        try:
            os.add_dll_directory(str(binary_dir))
        except Exception:
            pass
    if str(binary_dir) not in sys.path:
        sys.path.insert(0, str(binary_dir))

    return workspace_root, binary_dir


OUTPUT_DIR_NAME = "volume_checkpoint"
NUM_FRAMES = 6
DT = 1.0 / 60.0


# ---------------------------------------------------------------------------
# Save path: real sim -> registry -> checkpoint primvars
# ---------------------------------------------------------------------------

def _build_wb_zone_graph():
    """The streaming Wetbrush zone graph (same topology as
    test_brush_sim.py, small resolution to keep the test fast)."""
    from ruzino_graph import RuzinoGraph

    g = RuzinoGraph("CheckpointSim")
    g.loadConfiguration(str(Path(_prepare_env()[1]) / "geometry_nodes.json"))

    pen = g.createNode("mock_pen_motion", name="PenMotion")
    init_state = g.createNode("brush_wb_init_state", name="InitState")
    sim_in, sim_out = g.createSimulationZone()
    sim = g.createNode("brush_wb_sim", name="Sim")
    commit = g.createNode("brush_wb_commit", name="Commit")
    write = g.createNode("write_usd", name="Output")

    g.addEdge(init_state, "State", sim_in, "Simulation In")
    g.addEdge(pen, "Stroke Sample", sim, "Stroke Sample")
    g.addEdge(sim_in, "Simulation Out", sim, "State")
    g.addEdge(sim, "State", commit, "State")
    g.addEdge(commit, "Paint Particles", write, "Geometry")
    g.addEdge(commit, "State", sim_out, "Simulation In")

    g.setSocketDefaults({
        (pen, "Enable"): True,
        (pen, "Length"): 0.3,
        (pen, "Amplitude"): 0.05,
        (pen, "Speed"): 0.15,
        (sim, "Resolution"): 256,
        (sim, "Paper Size"): 1.0,
        (sim, "Brush Radius"): 0.02,
        (sim, "Brush Pressure"): 1.0,
        (sim, "Ink Amount"): 0.8,
    })
    return g


def _author_marker_scene(path: Path):
    """Camera + a light for the HydraRenderer; the volume prim is created by
    save_volume_checkpoint itself."""
    from pxr import Usd, UsdGeom, UsdLux, Gf

    stage = Usd.Stage.CreateNew(str(path))
    UsdGeom.SetStageUpAxis(stage, "Z")

    cam = UsdGeom.Camera.Define(stage, "/Camera")
    cam.CreateFocalLengthAttr(35.0)
    cam.CreateHorizontalApertureAttr(36.0)
    cam.CreateVerticalApertureAttr(20.25)
    cam.CreateClippingRangeAttr((0.1, 100.0))
    xf = UsdGeom.XformCommonAPI(cam)
    xf.SetTranslate((0.0, 0.0, 3.0))

    light = UsdLux.DistantLight.Define(stage, "/Sun")
    light.CreateIntensityAttr(3.0)
    light.CreateAngleAttr(0.53)

    stage.GetRootLayer().Save()


def test_sim_checkpoint_authors_primvars():
    workspace_root, binary_dir = _prepare_env()

    try:
        import stage_py
        import hd_RUZINO_py as renderer  # noqa: F401
    except ImportError as e:
        pytest.skip(f"required module not available: {e}")

    from pxr import Sdf, Usd, UsdGeom
    from pxr.UsdGeom import PrimvarsAPI as UsdGeomPrimvarsAPI

    out_dir = binary_dir / "test_output" / OUTPUT_DIR_NAME
    out_dir.mkdir(parents=True, exist_ok=True)
    marker_scene = out_dir / "marker_scene.usda"
    checkpoint_scene = out_dir / "paint_checkpoint.usdc"
    sim_usd = out_dir / "sim.usdc"
    for f in (marker_scene, checkpoint_scene, sim_usd):
        if f.exists():
            f.unlink()

    _author_marker_scene(marker_scene)

    # 1. Drive the sim — each tick's commit packs the paint field and
    #    registers it under "grid_field/paint" (process-global registry).
    g = _build_wb_zone_graph()
    stage = stage_py.Stage(str(sim_usd))
    UsdGeom.Mesh.Define(stage.get_pxr_stage(), "/Brush")
    g.apply_to_stage(stage, "/Brush")
    prim = stage.get_pxr_stage().GetPrimAtPath(Sdf.Path("/Brush"))
    prim.CreateAttribute("Animatable", Sdf.ValueTypeNames.Bool).Set(True)

    for _ in range(NUM_FRAMES):
        stage.tick(DT)
        stage.finish_tick()
        stage.set_render_time((NUM_FRAMES + 1) * DT)

    # 2. Checkpoint onto a fresh prim in the marker scene, exporting the
    #    root layer as the standalone checkpoint file.
    hydra = renderer.HydraRenderer(str(marker_scene), 64, 48)
    voxels = hydra.save_volume_checkpoint(
        "/PaintVolume",
        key="grid_field/paint",
        export_path=str(checkpoint_scene),
    )
    assert voxels > 0, "checkpoint reported zero voxels"

    # 3. The exported file must be self-contained: metadata + paint data.
    reopened = Usd.Stage.Open(str(checkpoint_scene))
    vol = reopened.GetPrimAtPath("/PaintVolume")
    assert vol.IsValid(), "checkpoint did not define /PaintVolume"
    assert vol.GetTypeName() == "Volume"

    pv = UsdGeomPrimvarsAPI(vol)

    def _primvar(name):
        attr = pv.GetPrimvar(name)
        assert attr.IsDefined(), f"missing primvar {name}"
        return attr.Get()

    res_x = int(_primvar("gridResX"))
    res_y = int(_primvar("gridResY"))
    res_z = int(_primvar("gridResZ"))
    assert res_x > 0 and res_y > 0 and res_z > 0
    assert voxels == res_x * res_y * res_z, "voxel count != res product"

    # Per-axis spacing must be authored (registry GridFieldDesc is
    # per-axis) and positive.
    for name in ("cellSizeX", "cellSizeY", "cellSizeZ"):
        v = float(_primvar(name))
        assert v > 0.0, f"{name} not positive: {v}"

    # The paint field must contain actual paint (non-zero density voxels)
    # — the sim deposited ink over NUM_FRAMES ticks.
    paint = pv.GetPrimvar("paintField").Get()
    assert len(paint) == res_x * res_y * res_z, "paintField size mismatch"
    density = np.asarray([p[0] for p in paint], dtype=np.float32)
    painted = int((density > 0.0).sum())
    assert painted > 0, "checkpoint captured an empty paint field"
    print(f"[checkpoint] {voxels} voxels, {painted} painted "
          f"(max density {float(density.max()):.3f})")


