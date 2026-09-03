#!/usr/bin/env python3
"""Hydra 2.0 migration Track A: points primvar data-source direct read.

Hd_RUZINO_Points::Sync reads points/widths/displayColor/debugKey primvars
straight from the terminal scene index's data source tree (bypassing the
emulation adapter's Get* translation), with an automatic fallback to the
legacy HdSceneDelegate::Get pull whenever the data-source path cannot serve
a value. This test renders a UsdGeomPoints stage in a subprocess and
asserts a lit, non-blank image (see hydra2_ab_common for the A/B history).
"""

from pathlib import Path

import pytest

import hydra2_ab_common as ab


_POINTS_STAGE = """#usda 1.0
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
        color3f inputs:diffuseColor = (0.8, 0.2, 0.2)
        float inputs:metallic = 0
        float inputs:roughness = 0.5
        float inputs:opacity = 1
        token outputs:surface
    }
}

def Xform "World"
{
    def Points "Cloud" (
        apiSchemas = ["MaterialBindingAPI"]
    )
    {
        rel material:binding = </RedMaterial>
        point3f[] points = [(-0.5, -0.5, 0), (0.5, -0.5, 0), (0, 0.5, 0), (0, 0, 0.5), (-0.3, 0.3, 0.2), (0.3, 0.3, -0.2)]
        color3f[] primvars:displayColor = [(0.8, 0.2, 0.2), (0.8, 0.2, 0.2), (0.8, 0.2, 0.2), (0.8, 0.2, 0.2), (0.8, 0.2, 0.2), (0.8, 0.2, 0.2)] (
            interpolation = "vertex"
        )
        float[] primvars:widths = [0.08, 0.08, 0.08, 0.08, 0.08, 0.08] (
            interpolation = "vertex"
        )
    }
}

def DistantLight "Sun"
{
    float inputs:intensity = 1.0
    matrix4d xformOp:transform = ( (1,0,0,0), (0,-1,0,0), (0,0,-1,0), (0,0,0,1) )
    uniform token[] xformOpOrder = ["xformOp:transform"]
}
"""


def test_points_datasource_read_renders():
    """The points primvar direct read produces a lit, non-blank render."""
    _, binary_dir = ab.prepare_env()

    if not ab.hydra_py_available(binary_dir):
        pytest.skip(f"hd_RUZINO_py not built in {binary_dir}")

    import tempfile
    with tempfile.TemporaryDirectory() as td:
        stage = Path(td) / "points_stage.usda"
        stage.write_text(_POINTS_STAGE)
        ab.assert_renders(binary_dir, stage, td, "points")
