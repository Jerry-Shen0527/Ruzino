"""
Visual verification for the terrain node family (ASCII previews, no image
deps): terrain_heightfield -> terrain_erode_hydraulic (GPU virtual pipes) ->
transform -> write_usd, following the same stage_py recipe as
test_terrain_usd_grid.py. The eroded mesh is read back from the written USD
and the height field is printed as an ASCII relief map — the drainage
network carved by the pipes solver shows up as dark dendritic lines.

Run from anywhere: python source/Plugins/TerrainGen/tests/render_terrain_preview.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BINARY_DIR = os.path.abspath(os.path.join(HERE, "..", "..", "..", "..",
                                          "Binaries", "Release"))
sys.path.insert(0, BINARY_DIR)
sys.path.insert(0, os.path.abspath(os.path.join(
    HERE, "..", "..", "..", "Core", "rznode", "python")))
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = BINARY_DIR
os.chdir(BINARY_DIR)

import numpy as np  # noqa: E402
import stage_py  # noqa: E402
from ruzino_graph import RuzinoGraph  # noqa: E402

RES = int(os.environ.get("TERRAIN_RES", "2049"))  # (RES-1) % 16 == 0
TEX_RES = int(os.environ.get("TERRAIN_TEXRES", "2048"))
# Curvature (sag) tolerance in world units; 0 = off (relief/slope only).
SAG_TOL = float(os.environ.get("TERRAIN_SAG", "0"))
NRM_STR = float(os.environ.get("TERRAIN_NRM", "1.0"))
# Optional thermal relaxation after hydraulic (kills the pipes solver's
# 2-cell checkerboard); iterations, 0 = off.
THERMAL_IT = int(os.environ.get("TERRAIN_THERMAL", "0"))
OUT_USD = os.path.join(BINARY_DIR, "test_output", "terrain_preview.usdc")


def build_and_execute():
    g = RuzinoGraph("TerrainPreview")
    g.loadConfiguration(os.path.join(
        BINARY_DIR, "Plugins", "TerrainGen_geometry_nodes.json"))
    g.loadConfiguration(os.path.join(BINARY_DIR, "geometry_nodes.json"))

    hf = g.createNode("terrain_heightfield", name="hf")
    erode = g.createNode("terrain_erode_hydraulic", name="erode")
    ada = g.createNode("terrain_adaptive_mesh", name="ada")
    g.addEdge(hf, "Height Field", erode, "Height Field")
    last_eroder = erode
    if THERMAL_IT > 0:
        thermal = g.createNode("terrain_erode_thermal", name="thermal")
        g.addEdge(erode, "Height Field", thermal, "Height Field")
        last_eroder = thermal
    g.addEdge(last_eroder, "Height Field", ada, "Height Field")

    # Bake height/slope/wear into an albedo PNG, then bind it through the
    # create_material -> set_material pair (MaterialObject flow).
    bake = g.createNode("terrain_texture_bake", name="bake")
    g.addEdge(ada, "Height Field", bake, "Height Field")
    create_mat = g.createNode("create_material", name="create_material")
    g.addEdge(bake, "Texture Path", create_mat, "Texture Name")
    set_mat = g.createNode("set_material", name="set_material")
    g.addEdge(bake, "Height Field", set_mat, "Geometry")
    g.addEdge(create_mat, "Material", set_mat, "Material")

    writer = g.createNode("write_usd", name="writer")
    g.addEdge(set_mat, "Geometry", writer, "Geometry")

    stage = stage_py.Stage(OUT_USD)
    payload = stage_py.create_payload_from_stage(stage, "/terrain")
    g.setGlobalParams(payload)

    inputs = {
        (hf, "Resolution"): RES,
        (hf, "Size"): 200.0,
        (hf, "Height"): 40.0,
        (hf, "Seed"): 1234,
        (hf, "Octaves"): 6,
        (hf, "Persistence"): 0.45,
        (hf, "Ridge Blend"): 0.55,
        (hf, "Warp Strength"): 0.5,
        (erode, "Method"): "Virtual Pipes (GPU)",
        (erode, "Iterations"): 250,
        (ada, "Detail"): 0.3,
        (ada, "Slope Weight"): 1.0,
        (ada, "Max Subdiv"): 4,
        **({} if SAG_TOL <= 0 else {(ada, "Sag Tolerance"): SAG_TOL}),
        **({} if THERMAL_IT <= 0
           else {(thermal, "Iterations"): THERMAL_IT}),
        (bake, "Texture Resolution"): TEX_RES,
        (bake, "Rock Slope Angle"): 38.0,
        (bake, "Snow Line"): 0.58,
        (bake, "Sediment Strength"): 0.5,
        # Absolute: the node resolves relative paths against the process
        # executable, which under headless python is the interpreter dir.
        (bake, "Output Path"): os.path.join(
            BINARY_DIR, "test_output", "terrain_albedo.png"),
        (bake, "Normal Output Path"): os.path.join(
            BINARY_DIR, "test_output", "terrain_normal.png"),
        (bake, "Normal Strength"): NRM_STR,
        (bake, "Detail Strength"): 0.0,
        (create_mat, "Roughness"): 0.95,
        (create_mat, "Wrap Mode"): "clamp",
    }
    g.prepare_and_execute(inputs, required_node=writer)
    stage.save()
    print(f"stage saved: {OUT_USD}")


def read_heights():
    from pxr import Usd, UsdGeom
    # The write_usd node puts the mesh as an over in the _modifiers sidecar.
    stage = Usd.Stage.Open(OUT_USD.replace(".usdc", "_modifiers.usdc"))
    prim = stage.GetPrimAtPath("/terrain")
    if not prim or prim.GetTypeName() != "Mesh":
        raise RuntimeError("/terrain mesh not found in modifier layer")
    pts = np.array(UsdGeom.Mesh(prim).GetPointsAttr().Get())
    res = int(round(len(pts) ** 0.5))
    print(f"mesh: {len(pts)} verts -> res {res}")
    return pts[:, 1].reshape(res, res)


def ascii_preview(field, title, rows=48, cols=96):
    lo, hi = float(field.min()), float(field.max())
    span = max(hi - lo, 1e-6)
    ramp = " .:-=+*#%@"
    ry = max(1, field.shape[0] // rows)
    rx = max(1, field.shape[1] // cols)
    small = field[::ry, ::rx]
    print(f"\n=== {title}  (min {lo:.2f}  max {hi:.2f}) ===")
    for row in small:
        print("".join(
            ramp[min(int((v - lo) / span * (len(ramp) - 1)), len(ramp) - 1)]
            for v in row))


if __name__ == "__main__":
    os.makedirs(os.path.dirname(OUT_USD), exist_ok=True)
    print("building terrain graph (res 256, pipes 250 iters, GPU)...")
    build_and_execute()
    heights = read_heights()
    ascii_preview(heights, "ERODED HEIGHT (darker = lower; valleys/channels)")
    ascii_preview(-heights, "INVERTED (peaks dark)") if False else None
