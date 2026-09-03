#!/usr/bin/env python3
"""Hydra 2.0 migration Track A: mesh / material / instancer / light ingest stages.

Smoke tests for the heaviest direct-read surfaces in hydra2Ingest.h: each
stage renders once on the (only) ingest path and must produce a lit,
non-blank image (see hydra2_ab_common for the A/B history -- these stages
passed pixel/mean parity against the legacy pull before the A/B gate was
removed).

Stages:
  * mesh_geomsubsets -- a cube with two face-set GeomSubsets bound to
    different materials plus a mesh-level binding, lit by two distant
    lights and a sphere light. Exercises ReadMeshTopology (topology
    defaults + _GatherGeomSubsets face-set material bindings),
    ReadMaterialId (mesh + subsets), ReadMaterialResource (three
    UsdPreviewSurface networks through the hand-mirrored
    _ToMaterialNetworkMap translation), ReadPrimvar (points/primvar loop),
    ReadTransform, ReadVisible and the light-container param reads
    (radius/color/intensity family).
  * instancer_lights -- a PointInstancer with two mesh prototypes plus a
    sphere light and a distant light over a background quad. Exercises
    ReadInstanceIndices (ComputeInstanceIndicesForProto), the instancer's
    instance-primvar reads, and the instancer transform read. Pixel-level
    A/B is not asserted here: instancer rendering is nondeterministic
    across processes (geometry IDs assigned in sync order); only non-blank
    and coarse mean agreement are checked. See test_instancer_lights_ab.
  * invisible_occluder -- a red backdrop quad fully covered by a closer
    green quad authored `visibility = "invisible"`. Exercises ReadVisible's
    false path end to end: an invisible mesh is kept out of the TLAS
    (mesh.cpp gates updateTLAS on IsVisible()), so the red backdrop must
    dominate the image; if the visibility read regressed, the green
    occluder renders instead and the channel-dominance assertion fails.

Not covered (and not coverable from a .usda stage): the invisible-GeomSubset
branch of _GatherGeomSubsets (SetInvisibleFaces/Points union). UsdGeomSubset
is not UsdGeomImageable, so usdImaging never publishes a visibility schema
for subset prims and a subset cannot be made invisible from USD -- that
branch only serves legacy-converted chains (HdLegacyGeomSubsetSceneIndex).
hd_RUZINO additionally does not consume GetInvisibleFaces() yet, so even a
synthetic data source would not change the render.
"""

from pathlib import Path

import pytest

import hydra2_ab_common as ab


_MESH_GEOMSUBSETS_STAGE = """#usda 1.0
(
    metersPerUnit = 1
    upAxis = "Y"
)

def Camera "Camera"
{
    float2 clippingRange = (0.1, 100.0)
    float focalLength = 50.0
    float horizontalAperture = 36.0
    float verticalAperture = 20.25
    matrix4d xformOp:transform = ( (1,0,0,0), (0,1,0,0), (0,0,1,0), (0, 0, 5, 1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}

def Material "RedMaterial"
{
    token outputs:surface.connect = </RedMaterial/Shader.outputs:surface>
    def Shader "Shader"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.8, 0.15, 0.15)
        float inputs:metallic = 0
        float inputs:roughness = 0.6
        token outputs:surface
    }
}

def Material "BlueMaterial"
{
    token outputs:surface.connect = </BlueMaterial/Shader.outputs:surface>
    def Shader "Shader"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.15, 0.3, 0.8)
        float inputs:metallic = 0
        float inputs:roughness = 0.6
        token outputs:surface
    }
}

def Material "GreenMaterial"
{
    token outputs:surface.connect = </GreenMaterial/Shader.outputs:surface>
    def Shader "Shader"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.15, 0.7, 0.25)
        float inputs:metallic = 0
        float inputs:roughness = 0.6
        token outputs:surface
    }
}

def Xform "World"
{
    def Mesh "Cube" (
        apiSchemas = ["MaterialBindingAPI", "GeomSubsetAPI"]
    )
    {
        rel material:binding = </GreenMaterial>
        uniform token subdivisionScheme = "none"
        uniform token orientation = "rightHanded"
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [0, 1, 2, 3, 5, 4, 7, 6, 1, 5, 6, 2, 4, 0, 3, 7, 3, 2, 6, 7, 4, 5, 1, 0]
        point3f[] points = [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1), (-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1)]

        def GeomSubset "FrontBack"
        {
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [0, 1]
            rel material:binding = </RedMaterial>
        }

        def GeomSubset "Sides"
        {
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [2, 3, 4, 5]
            rel material:binding = </BlueMaterial>
        }
    }
}

def DistantLight "Sun"
{
    float inputs:intensity = 1.0
    matrix4d xformOp:transform = ( (1,0,0,0), (0,-1,0,0), (0,0,-1,0), (0,0,0,1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}

def DistantLight "SunBack"
{
    float inputs:intensity = 1.0
    matrix4d xformOp:transform = ( (1,0,0,0), (0,1,0,0), (0,0,1,0), (0,0,0,1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}

def SphereLight "Bulb"
{
    float inputs:intensity = 60.0
    float inputs:radius = 0.2
    color3f inputs:color = (1, 0.9, 0.8)
    matrix4d xformOp:transform = ( (1,0,0,0), (0,1,0,0), (0,0,1,0), (0, 2, 3, 1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}
"""

_INSTANCER_STAGE = """#usda 1.0
(
    metersPerUnit = 1
    upAxis = "Y"
)

def Camera "Camera"
{
    float2 clippingRange = (0.1, 100.0)
    float focalLength = 50.0
    float horizontalAperture = 36.0
    float verticalAperture = 20.25
    matrix4d xformOp:transform = ( (1,0,0,0), (0,1,0,0), (0,0,1,0), (0, 0, 5, 1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}

def Material "RedMaterial"
{
    token outputs:surface.connect = </RedMaterial/Shader.outputs:surface>
    def Shader "Shader"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.8, 0.15, 0.15)
        float inputs:metallic = 0
        float inputs:roughness = 0.6
        token outputs:surface
    }
}

def Material "BlueMaterial"
{
    token outputs:surface.connect = </BlueMaterial/Shader.outputs:surface>
    def Shader "Shader"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.15, 0.3, 0.8)
        float inputs:metallic = 0
        float inputs:roughness = 0.6
        token outputs:surface
    }
}

def Material "GreenMaterial"
{
    token outputs:surface.connect = </GreenMaterial/Shader.outputs:surface>
    def Shader "Shader"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.15, 0.7, 0.25)
        float inputs:metallic = 0
        float inputs:roughness = 0.9
        token outputs:surface
    }
}

def Xform "World"
{
    def Mesh "ProtoTri" (
        apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </RedMaterial>
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (0, 1, 0)]
        matrix4d xformOp:transform = ( (0.4,0,0,0), (0,0.4,0,0), (0,0,0.4,0), (0,0,0,1) )
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }

    def Mesh "ProtoQuad" (
        apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </BlueMaterial>
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]
        matrix4d xformOp:transform = ( (0.4,0,0,0), (0,0.4,0,0), (0,0,0.4,0), (0,0,0,1) )
        uniform token[] xformOpOrder = ["xformOp:transform"]
    }

    def PointInstancer "Swarm"
    {
        point3f[] positions = [(-1.5, -0.5, 0), (-0.5, -0.5, 0), (0.5, -0.5, 0), (1.5, -0.5, 0), (0, 0.8, 0)]
        int[] protoIndices = [0, 1, 0, 1, 0]
        rel prototypes = [</World/ProtoTri>, </World/ProtoQuad>]
    }

    def Mesh "Backdrop" (
        apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </GreenMaterial>
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-4, -2.5, -1.5), (4, -2.5, -1.5), (4, 2.5, -1.5), (-4, 2.5, -1.5)]
    }
}

def SphereLight "Bulb"
{
    float inputs:intensity = 60.0
    float inputs:radius = 0.2
    color3f inputs:color = (1, 0.9, 0.8)
    matrix4d xformOp:transform = ( (1,0,0,0), (0,1,0,0), (0,0,1,0), (0, 2, 1, 1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}

def DistantLight "Sun"
{
    float inputs:intensity = 1.0
    matrix4d xformOp:transform = ( (1,0,0,0), (0,-1,0,0), (0,0,-1,0), (0,0,0,1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}
"""


_INVISIBLE_OCCLUDER_STAGE = """#usda 1.0
(
    metersPerUnit = 1
    upAxis = "Y"
)

def Camera "Camera"
{
    float2 clippingRange = (0.1, 100.0)
    float focalLength = 50.0
    float horizontalAperture = 36.0
    float verticalAperture = 20.25
    matrix4d xformOp:transform = ( (1,0,0,0), (0,1,0,0), (0,0,1,0), (0, 0, 5, 1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}

def Material "RedMaterial"
{
    token outputs:surface.connect = </RedMaterial/Shader.outputs:surface>
    def Shader "Shader"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.8, 0.15, 0.15)
        float inputs:metallic = 0
        float inputs:roughness = 0.6
        token outputs:surface
    }
}

def Material "GreenMaterial"
{
    token outputs:surface.connect = </GreenMaterial/Shader.outputs:surface>
    def Shader "Shader"
    {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0.15, 0.7, 0.25)
        float inputs:metallic = 0
        float inputs:roughness = 0.6
        token outputs:surface
    }
}

def Xform "World"
{
    def Mesh "Backdrop" (
        apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </RedMaterial>
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-1.5, -1.5, 0), (1.5, -1.5, 0), (1.5, 1.5, 0), (-1.5, 1.5, 0)]
    }

    # Closer to the camera than the backdrop and covering it exactly: renders
    # green (and fails the test) if the visibility read regresses.
    def Mesh "Occluder" (
        apiSchemas = ["MaterialBindingAPI"]
    )
    {
        uniform token visibility = "invisible"
        rel material:binding = </GreenMaterial>
        uniform token subdivisionScheme = "none"
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-1.5, -1.5, 1), (1.5, -1.5, 1), (1.5, 1.5, 1), (-1.5, 1.5, 1)]
    }
}

def DistantLight "Sun"
{
    float inputs:intensity = 1.0
    matrix4d xformOp:transform = ( (1,0,0,0), (0,-1,0,0), (0,0,-1,0), (0,0,0,1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}
"""


def test_mesh_geomsubsets_ab():
    """Mesh topology + GeomSubsets + material networks ingest and render."""
    _, binary_dir = ab.prepare_env()

    if not ab.hydra_py_available(binary_dir):
        pytest.skip(f"hd_RUZINO_py not built in {binary_dir}")

    import tempfile
    with tempfile.TemporaryDirectory() as td:
        stage = Path(td) / "mesh_geomsubsets.usda"
        stage.write_text(_MESH_GEOMSUBSETS_STAGE)
        ab.assert_renders(binary_dir, stage, td, "mesh-geomsubsets")


def test_instancer_lights_ab():
    """The instancer scene ingests and renders non-blank.

    Note: the renderer's instancer path is nondeterministic across
    processes regardless of ingest path (prototype geometry IDs are
    assigned in rprim sync order, which varies run to run; same-scene
    renders differ by up to ~0.11 mean abs diff, 2026-09-02). Pixel-level
    assertions are therefore not possible for this stage until that is
    fixed.
    """
    _, binary_dir = ab.prepare_env()

    if not ab.hydra_py_available(binary_dir):
        pytest.skip(f"hd_RUZINO_py not built in {binary_dir}")

    import tempfile
    with tempfile.TemporaryDirectory() as td:
        stage = Path(td) / "instancer_lights.usda"
        stage.write_text(_INSTANCER_STAGE)
        ab.assert_renders(binary_dir, stage, td, "instancer-lights")


def test_invisible_occluder_hidden():
    """ReadVisible's false path: an invisible mesh stays out of the render.

    The green occluder fully covers the red backdrop from the camera; with
    a working visibility read it is kept out of the TLAS and the image must
    be dominated by the red backdrop. If the read regresses, the green
    occluder renders instead and red-minus-green goes negative.
    """
    _, binary_dir = ab.prepare_env()

    if not ab.hydra_py_available(binary_dir):
        pytest.skip(f"hd_RUZINO_py not built in {binary_dir}")

    import tempfile
    with tempfile.TemporaryDirectory() as td:
        stage = Path(td) / "invisible_occluder.usda"
        stage.write_text(_INVISIBLE_OCCLUDER_STAGE)
        img = ab.render_to_array(binary_dir, stage, td)

    mean_r = float(img[:, :, 0].mean())
    mean_g = float(img[:, :, 1].mean())
    assert mean_r > 1e-3, (
        f"invisible-occluder: render blank (meanR={mean_r})")
    assert mean_r - mean_g > 0.05, (
        f"invisible-occluder: image is not red-dominated "
        f"(meanR={mean_r:.3f}, meanG={mean_g:.3f}) -- the invisible green "
        f"occluder appears to be rendered")
