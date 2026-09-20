"""
Test Stage::import_materialx — .mtlx file -> UsdShade prims in the stage.

The function translates a MaterialX document into the stage via the SDK's
UsdMtlxRead: materials land under ``<prim_path>/Materials/<MaterialName>``,
node graphs under ``<prim_path>/NodeGraphs``. See stage.cpp.

Validates:
1. A .mtlx document with one standard_surface material imports as an
   UsdShadeMaterial with a surface output (and the translated shader node).
2. The imported prims are authored on the ROOT layer (imports are scene
   content, not modifier overrides).
3. The import survives save + reopen.
4. A malformed .mtlx fails gracefully: no prims authored, no exception
   raised into Python.
"""

import os
import sys
from pathlib import Path

_STAGE_TESTS = Path(__file__).parent
_PROJECT_ROOT = _STAGE_TESTS.parents[3]
BINARY_DIR = _PROJECT_ROOT / "Binaries" / "Release"
RZNODE_PYTHON = _PROJECT_ROOT / "source" / "Core" / "rznode" / "python"

sys.path.insert(0, str(BINARY_DIR))
sys.path.insert(0, str(RZNODE_PYTHON))
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(BINARY_DIR)
# MaterialX standard library (standard_surface et al.) — same env the
# renderer tests set (see test_hydra_renderer.py).
_mtlx_stdlib = BINARY_DIR / "libraries"
if _mtlx_stdlib.exists():
    os.environ.setdefault(
        "PXR_MTLX_STDLIB_SEARCH_PATHS", str(_mtlx_stdlib))

from pxr import Sdf, UsdShade  # noqa: E402

import stage_py  # noqa: E402

OUTPUT_DIR = BINARY_DIR / "test_output" / "import_materialx"

# One MaterialX material driving a standard_surface — exercises material
# translation, stdlib nodedef resolution (standard_surface), and interface
# input translation in one document.
#
# NOTE: the SDK's UsdMtlxRead only treats a node as a material when its type
# is literally "material" (reader.cpp getMaterialNodes), so surfacematerial
# MUST carry the explicit type="material" attribute — a bare
# <surfacematerial> element is silently ignored (verified against the SDK
# source and empirically).
MTLX_DOC = """<?xml version="1.0"?>
<materialx version="1.39" >
  <standard_surface name="SR_test" type="surfaceshader" >
    <input name="base_color" type="color3" value="0.8,0.2,0.1" />
  </standard_surface>
  <surfacematerial name="TestMaterial" type="material" >
    <input name="surfaceshader" type="surfaceshader" nodename="SR_test" />
  </surfacematerial>
</materialx>
"""

MALFORMED_MTLX = """<?xml version="1.0"?>
<materialx version="1.39">
  <surfacematerial name="Broken">
    <input name="surfaceshader" type="surfaceshader" nodename="does_not_exist"/>
"""


def _write_mtlx(name: str, text: str) -> Path:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    path = OUTPUT_DIR / name
    path.write_text(text, encoding="utf-8")
    return path


def test_import_materialx_creates_usdshade_material():
    mtlx_path = _write_mtlx("test_material.mtlx", MTLX_DOC)

    stage = stage_py.Stage(str(OUTPUT_DIR / "import_mtlx.usdc"))
    stage.import_materialx(str(mtlx_path), "/MaterialX")

    pxr_stage = stage.get_pxr_stage()
    mat_prim = pxr_stage.GetPrimAtPath("/MaterialX/Materials/TestMaterial")
    assert mat_prim.IsValid(), (
        "import_materialx did not create /MaterialX/Materials/TestMaterial; "
        f"stage children: "
        f"{[str(p.GetPath()) for p in pxr_stage.Traverse()]}"
    )
    assert mat_prim.IsA(UsdShade.Material)

    material = UsdShade.Material(mat_prim)
    surface = material.GetSurfaceOutput()
    assert bool(surface), "imported material has no surface output"

    # The translated shader node must exist inside the material.
    shader_paths = [
        str(p.GetPath()) for p in mat_prim.GetChildren()
        if p.IsA(UsdShade.Shader)
    ]
    assert shader_paths, (
        f"no UsdShade.Shader child under the imported material "
        f"({[str(p.GetPath()) for p in mat_prim.GetChildren()]})"
    )

    # Imports are scene content -> root layer, not the modifier sidecar.
    root_spec = pxr_stage.GetRootLayer().GetPrimAtPath(
        Sdf.Path("/MaterialX/Materials/TestMaterial"))
    assert root_spec is not None, "imported material is not on the root layer"


def test_import_materialx_survives_save_and_reopen():
    mtlx_path = _write_mtlx("roundtrip_material.mtlx", MTLX_DOC)
    usd_path = OUTPUT_DIR / "roundtrip.usdc"

    stage = stage_py.Stage(str(usd_path))
    stage.import_materialx(str(mtlx_path), "/MaterialX")
    stage.save()

    from pxr import Usd

    reopened = Usd.Stage.Open(str(usd_path))
    assert reopened.GetPrimAtPath(
        "/MaterialX/Materials/TestMaterial").IsValid(), (
        "imported material did not survive save+reopen")


def test_import_materialx_malformed_file_is_graceful():
    mtlx_path = _write_mtlx("broken.mtlx", MALFORMED_MTLX)

    stage = stage_py.Stage(str(OUTPUT_DIR / "broken.usdc"))
    # Must not raise into Python; errors go through spdlog.
    stage.import_materialx(str(mtlx_path), "/MaterialX")

    pxr_stage = stage.get_pxr_stage()
    prims = [str(p.GetPath()) for p in pxr_stage.Traverse()]
    assert "/MaterialX/Materials/Broken" not in prims, (
        f"malformed mtlx should not author material prims (got {prims})")
