"""Leaf-card material integration: tree_generate UVs + set_material node.

Verifies the C++ node chain writes everything the renderer needs for
textured alpha-cutout leaf cards, with NO script-side material authoring:
  - leaves prim carries primvars:UVMap (face-varying, one per face corner)
  - a UsdPreviewSurface material with diffuseColor <- UsdUVTexture.rgb and
    opacity <- UsdUVTexture.a (opacityMode=mask) is authored and bound
"""
import os
from ruzino_graph import RuzinoGraph
import stage_py
import geometry_py  # This triggers geometry nodes loading

from pxr import Usd, UsdGeom, UsdShade, Sdf

ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
ATLAS = os.path.join(ROOT, "source", "Plugins", "TreeGen", "assets",
                     "leaf_atlas.png")


def test_leaf_material_pipeline():
    binary_dir = os.path.join(ROOT, "Binaries", "Release")
    output_file = os.path.join(binary_dir, "leaf_material_test.usdc")

    g = RuzinoGraph("TreeLeafMaterial")
    g.loadConfiguration(os.path.join(binary_dir, "geometry_nodes.json"))
    g.loadConfiguration(
        os.path.join(binary_dir, "Plugins", "TreeGen_geometry_nodes.json"))

    tree_gen = g.createNode("tree_generate", name="tree")
    create_material = g.createNode("create_material", name="leaf_material")
    set_material = g.createNode("set_material", name="apply_leaf_material")
    to_mesh = g.createNode("tree_to_mesh", name="mesh_converter")
    write_branches = g.createNode("write_usd", name="writer_branches")
    write_leaves = g.createNode("write_usd", name="writer_leaves")

    # create_material builds the data, set_material attaches it — the
    # generic material workflow on the leaves edge
    g.addEdge(create_material, "Material", set_material, "Material")
    g.addEdge(tree_gen, "Leaves", set_material, "Geometry")
    g.addEdge(set_material, "Geometry", to_mesh, "Leaves")
    g.addEdge(tree_gen, "Tree Branches", to_mesh, "Tree Branches")
    g.addEdge(to_mesh, "Branch Mesh", write_branches, "Geometry")
    g.addEdge(to_mesh, "Leaf Mesh", write_leaves, "Geometry")

    inputs = {
        (tree_gen, "Growth Years"): 5,
        (tree_gen, "Generate Leaves"): True,
        (tree_gen, "Leaves Per Internode"): 40,
        (tree_gen, "Leaf Size"): 0.15,
        (create_material, "Texture Name"): ATLAS,
        (create_material, "Alpha Cutout"): True,
        (create_material, "Opacity Threshold"): 0.5,
        (create_material, "Roughness"): 0.55,
        (create_material, "Wrap Mode"): "clamp",
        (write_branches, "Sub Path"): "branches",
        (write_leaves, "Sub Path"): "leaves",
    }

    stage = stage_py.Stage(output_file)
    geom_payload = stage_py.create_payload_from_stage(stage, "/plastic_tree")
    g.setGlobalParams(geom_payload)
    g.prepare_and_execute(inputs, required_node=write_branches)
    g.prepare_and_execute(inputs, required_node=write_leaves)
    stage.save()

    assert os.path.exists(output_file), f"File not created: {output_file}"

    # ---- Inspect the written stage ----
    modifier_file = output_file.replace(".usdc", "_modifiers.usdc")
    composed = output_file.replace(".usdc", "_composed.usda")
    if os.path.exists(composed):
        os.remove(composed)
    layer = Sdf.Layer.CreateNew(str(composed))
    layer.subLayerPaths = [
        os.path.abspath(modifier_file), os.path.abspath(output_file)]
    layer.Save()
    st = Usd.Stage.Open(str(composed))

    leaves = UsdGeom.Mesh.Get(st, "/plastic_tree/leaves")
    assert leaves, "/plastic_tree/leaves missing"

    fvi = leaves.GetFaceVertexIndicesAttr().Get()
    uv = UsdGeom.PrimvarsAPI(leaves.GetPrim()).GetPrimvar("UVMap")
    assert uv, "primvars:UVMap missing on leaves"
    uv_values = uv.Get()
    assert uv_values and len(uv_values) == len(fvi), (
        f"UVMap size {len(uv_values) if uv_values else 0} != "
        f"face corner count {len(fvi)}")
    assert uv.GetInterpolation() == UsdGeom.Tokens.faceVarying, (
        f"UVMap interpolation = {uv.GetInterpolation()}")

    # Material authored + bound with the alpha-cutout network
    binding = UsdShade.MaterialBindingAPI(leaves.GetPrim())
    mat, _ = binding.ComputeBoundMaterial()
    assert mat, "no material bound to leaves"
    surf = mat.GetSurfaceOutput()
    assert surf and surf.GetConnectedSource(), "no surface shader network"
    shader_prim = surf.GetConnectedSource()[0]
    shader = UsdShade.Shader(shader_prim)
    assert shader.GetIdAttr().Get() == "UsdPreviewSurface"

    def _connected_float_source(shader, input_name):
        inp = shader.CreateInput(input_name, Sdf.ValueTypeNames.Float)
        src = inp.GetConnectedSource()
        if not src:
            return None
        src_shader = UsdShade.Shader(src[0])
        return src_shader.GetIdAttr().Get(), src_shader

    diffuse = shader.CreateInput(
        "diffuseColor", Sdf.ValueTypeNames.Color3f).GetConnectedSource()
    assert diffuse, "diffuseColor not textured"
    diffuse_shader = UsdShade.Shader(diffuse[0])
    assert diffuse_shader.GetIdAttr().Get() == "UsdUVTexture"

    opacity = _connected_float_source(shader, "opacity")
    assert opacity and opacity[0] == "UsdUVTexture", (
        "opacity not driven by a UsdUVTexture (alpha cutout missing)")
    mode = shader.CreateInput(
        "opacityMode", Sdf.ValueTypeNames.Token).Get()
    assert mode == "mask", f"opacityMode = {mode}"
    threshold = shader.CreateInput(
        "opacityThreshold", Sdf.ValueTypeNames.Float).Get()
    assert threshold is not None and abs(threshold - 0.5) < 1e-5, (
        f"opacityThreshold = {threshold}")

    print("\n" + "=" * 70)
    print("TEST PASSED: leaf material pipeline")
    print(f"  UVMap: {len(uv_values)} face-varying values on "
          f"{len(fvi)} face corners")
    print(f"  material: {mat.GetPath()} (diffuse+opacity UsdUVTexture, "
          f"mask@{threshold})")
    print("=" * 70)
