"""Texture-object material flow: in-graph texture + flat-color bark.

Exercises the storage-postponed material path end to end:
  load_texture_2d -> set_material("Texture" socket) -> leaves, and a
  textureless set_material (flat Base Color) on the branches.

Asserts, with NO script-side material authoring:
  - leaves are bound to a UsdPreviewSurface whose diffuseColor samples a
    UsdUVTexture file that was materialized next to the stage
  - the materialized PNG exists on disk
  - branches carry a flat-color material (constant diffuse, no texture)
"""
import glob
import os

from ruzino_graph import RuzinoGraph
import stage_py
import geometry_py  # This triggers geometry nodes loading

from pxr import Usd, UsdGeom, UsdShade, Sdf

ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
ATLAS = os.path.join(ROOT, "source", "Plugins", "TreeGen", "assets",
                     "leaf_atlas.png")


def test_texture_object_material():
    binary_dir = os.path.join(ROOT, "Binaries", "Release")
    output_file = os.path.join(binary_dir, "texobj_material_test.usdc")

    g = RuzinoGraph("TreeTexObjMaterial")
    g.loadConfiguration(os.path.join(binary_dir, "geometry_nodes.json"))
    g.loadConfiguration(
        os.path.join(binary_dir, "Plugins", "TreeGen_geometry_nodes.json"))

    tree_gen = g.createNode("tree_generate", name="tree")
    leaf_tex = g.createNode("load_texture_2d", name="leaf_atlas_texture")
    leaf_create = g.createNode("create_material", name="leaf_material")
    leaf_apply = g.createNode("set_material", name="apply_leaf_material")
    bark_create = g.createNode("create_material", name="bark_material")
    bark_apply = g.createNode("set_material", name="apply_bark_material")
    to_mesh = g.createNode("tree_to_mesh", name="mesh_converter")
    write_branches = g.createNode("write_usd", name="writer_branches")
    write_leaves = g.createNode("write_usd", name="writer_leaves")

    g.addEdge(leaf_tex, "Texture", leaf_create, "Texture")
    g.addEdge(leaf_create, "Material", leaf_apply, "Material")
    g.addEdge(tree_gen, "Leaves", leaf_apply, "Geometry")
    g.addEdge(leaf_apply, "Geometry", to_mesh, "Leaves")
    g.addEdge(tree_gen, "Tree Branches", to_mesh, "Tree Branches")
    g.addEdge(to_mesh, "Branch Mesh", bark_apply, "Geometry")
    g.addEdge(bark_create, "Material", bark_apply, "Material")
    g.addEdge(bark_apply, "Geometry", write_branches, "Geometry")
    g.addEdge(to_mesh, "Leaf Mesh", write_leaves, "Geometry")

    inputs = {
        (tree_gen, "Growth Years"): 4,
        (tree_gen, "Generate Leaves"): True,
        (leaf_tex, "Path"): ATLAS,
        (leaf_create, "Alpha Cutout"): True,
        (leaf_create, "Roughness"): 0.55,
        (leaf_create, "Wrap Mode"): "clamp",
        (bark_create, "Base Color"): (0.42, 0.30, 0.20),
        (bark_create, "Roughness"): 0.8,
        (write_branches, "Sub Path"): "branches",
        (write_leaves, "Sub Path"): "leaves",
    }

    stage = stage_py.Stage(output_file)
    geom_payload = stage_py.create_payload_from_stage(stage, "/plastic_tree")
    g.setGlobalParams(geom_payload)
    g.prepare_and_execute(inputs, required_node=write_branches)
    g.prepare_and_execute(inputs, required_node=write_leaves)
    stage.save()

    modifier_file = output_file.replace(".usdc", "_modifiers.usdc")
    composed = output_file.replace(".usdc", "_composed.usda")
    if os.path.exists(composed):
        os.remove(composed)
    layer = Sdf.Layer.CreateNew(str(composed))
    layer.subLayerPaths = [
        os.path.abspath(modifier_file), os.path.abspath(output_file)]
    layer.Save()
    st = Usd.Stage.Open(str(composed))

    def bound_material(prim_path):
        prim = st.GetPrimAtPath(prim_path)
        assert prim, f"{prim_path} missing"
        binding = UsdShade.MaterialBindingAPI(prim)
        mat, _ = binding.ComputeBoundMaterial()
        assert mat, f"no material bound to {prim_path}"
        return mat

    # --- Leaves: textured + cutout, file materialized from the in-graph
    # texture object ---
    leaf_mat_prim = bound_material("/plastic_tree/leaves")
    surf = leaf_mat_prim.GetSurfaceOutput().GetConnectedSource()
    shader = UsdShade.Shader(surf[0])
    diffuse = shader.CreateInput(
        "diffuseColor", Sdf.ValueTypeNames.Color3f).GetConnectedSource()
    assert diffuse, "leaf diffuseColor not textured"
    diffuse_shader = UsdShade.Shader(diffuse[0])
    file_attr = diffuse_shader.CreateInput(
        "file", Sdf.ValueTypeNames.Asset)
    file_path = file_attr.Get().resolvedPath or str(file_attr.Get().path)
    assert file_path, "diffuse texture has no file path"
    assert os.path.exists(file_path), (
        f"materialized texture missing on disk: {file_path}")
    assert "textures" in file_path, (
        f"unexpected materialization location: {file_path}")

    # --- Branches: flat color, no texture network ---
    bark_mat_prim = bound_material("/plastic_tree/branches")
    bsurf = bark_mat_prim.GetSurfaceOutput().GetConnectedSource()
    bshader = UsdShade.Shader(bsurf[0])
    assert bshader.GetIdAttr().Get() == "UsdPreviewSurface"
    bdiff = bshader.CreateInput(
        "diffuseColor", Sdf.ValueTypeNames.Color3f).Get()
    assert bdiff is not None, "bark diffuseColor missing"
    assert abs(bdiff[0] - 0.42) < 0.01 and abs(bdiff[1] - 0.30) < 0.01, (
        f"bark diffuseColor = {bdiff}")
    bconn = bshader.CreateInput(
        "diffuseColor", Sdf.ValueTypeNames.Color3f).GetConnectedSource()
    assert not bconn, "bark diffuseColor should be a constant, not textured"

    print("\n" + "=" * 70)
    print("TEST PASSED: texture-object material flow")
    print(f"  leaf texture materialized: {file_path}")
    print(f"  bark: flat diffuse {tuple(round(v, 2) for v in bdiff)}")
    print("=" * 70)
