"""Headless tests for the HosekWilkieSky sun rig (sky -> child light linkage).

Architecture under test (no renderer involvement — the stage data must be
self-consistent for ANY consumer):

    HosekWilkieSky (DomeLight-derived codeless schema, RuzinoSky plugin)
    └── DistantLight (child; direction kept in sync by stage/hosek_sky.cpp)

Covered:
  1. schema registration: the prim type exists, IsA DomeLight, attribute
     fallbacks (incl. inputs:shader_path) resolve from the schema
  2. sync helper math under both sky conventions (Y-up identity dome and
     Z-up stage with a RotX(-90) dome transform)
  3. the live linkage: editing inputs:sunDirection on a loaded Stage
     re-points the child light (UsdNotice -> Stage events bus -> sync)
  4. persistence: the synced transform survives save()/reload through the
     modifier-layer sidecar

Environment is set up by source/tests/conftest.py.
"""
import sys
from pathlib import Path

import pytest

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent
BINARY_DIR = PROJECT_ROOT / "Binaries" / "Release"


def _build_rig(path, yup_identity=True, sun_dir=(0.5, 0.7, 0.5)):
    """Author a HosekWilkieSky + DistantLight child directly with pxr.

    yup_identity: dome without a transform (attribute is world-space).
    Otherwise a Z-up stage: dome carries RotX(-90) so its local +Y zenith
    points at world +Z and the attribute is dome-local.
    """
    from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux

    stage = Usd.Stage.CreateNew(str(path))
    sky = stage.DefinePrim("/Sky", "HosekWilkieSky")
    assert sky, "HosekWilkieSky prim definition not registered"
    if not yup_identity:
        xf = Gf.Matrix4d().SetRotate(Gf.Rotation(Gf.Vec3d(1, 0, 0), -90))
        UsdGeom.Xformable(sky).AddTransformOp().Set(xf)
    sky.CreateAttribute(
        "inputs:sunDirection", Sdf.ValueTypeNames.Float3).Set(
        Gf.Vec3f(*sun_dir))
    stage.DefinePrim("/Sky/Sun", "DistantLight")
    stage.GetRootLayer().Save()
    return stage


def _child_row2(pxr_stage):
    """Row 2 (light travel direction) of the child light's transform."""
    from pxr import Gf, UsdGeom

    sun = pxr_stage.GetPrimAtPath("/Sky/Sun")
    xform = UsdGeom.Xformable(sun)
    mat = xform.ComputeLocalToWorldTransform(0.0)
    row2 = mat.GetRow(2)
    return Gf.Vec3d(row2[0], row2[1], row2[2])


def test_schema_registration(tmp_path):
    """The RuzinoSky plugin is discovered and fallbacks resolve."""
    from pxr import Usd, UsdLux

    p = tmp_path / "schema_smoke.usda"
    stage = Usd.Stage.CreateNew(str(p))
    prim = stage.DefinePrim("/Sky", "HosekWilkieSky")
    assert prim.IsA(UsdLux.DomeLight)
    assert prim.GetAttribute("inputs:shader_path").Get() == (
        "callables/eval_dome_light_hosek_wilkie.slang")
    assert prim.GetAttribute("inputs:sunDirection").Get() == (0.5, 0.7, 0.5)
    dome = UsdLux.DomeLight(prim)
    assert dome.GetIntensityAttr().Get() == pytest.approx(1.0)


def test_sync_helper_both_conventions(tmp_path):
    """sync_sun_light / world_sun_direction under Y-up identity and Z-up
    RotX(-90) dome transforms."""
    import stage_py
    from pxr import Gf

    # Y-up identity: dome-local attr is already world-space.
    p1 = tmp_path / "rig_yup.usda"
    _build_rig(p1, yup_identity=True, sun_dir=(1.0, 0.0, 0.0))
    stage = stage_py.Stage(str(p1))
    pxr_stage = stage.get_pxr_stage()

    world = stage_py.world_sun_direction(pxr_stage, "/Sky")
    assert world == pytest.approx((1.0, 0.0, 0.0), abs=1e-5)
    stage_py.sync_sun_light(pxr_stage, "/Sky", pxr_stage.GetRootLayer())
    assert _child_row2(pxr_stage) == pytest.approx(
        Gf.Vec3d(-1.0, 0.0, 0.0), abs=1e-5)

    # Z-up stage: dome RotX(-90); the local +Y zenith maps to world +Z.
    p2 = tmp_path / "rig_zup.usda"
    _build_rig(p2, yup_identity=False, sun_dir=(0.0, 1.0, 0.0))
    stage2 = stage_py.Stage(str(p2))
    pxr_stage2 = stage2.get_pxr_stage()

    world2 = stage_py.world_sun_direction(pxr_stage2, "/Sky")
    assert world2 == pytest.approx((0.0, 0.0, 1.0), abs=1e-5)
    stage_py.sync_sun_light(pxr_stage2, "/Sky", pxr_stage2.GetRootLayer())
    assert _child_row2(pxr_stage2) == pytest.approx(
        Gf.Vec3d(0.0, 0.0, -1.0), abs=1e-5)


def test_live_linkage_on_edit(tmp_path):
    """Editing the sky's sunDirection on a loaded Stage re-points the child
    light through the notice -> events bus -> sync chain."""
    import stage_py
    from pxr import Gf, Sdf

    p = tmp_path / "rig_live.usda"
    _build_rig(p, yup_identity=True, sun_dir=(1.0, 0.0, 0.0))
    stage = stage_py.Stage(str(p))
    pxr_stage = stage.get_pxr_stage()
    # Untouched so far: the child has no transform, so row 2 is the identity
    # convention's local +Z.
    assert _child_row2(pxr_stage) == pytest.approx(
        Gf.Vec3d(0.0, 0.0, 1.0), abs=1e-5)

    sky = pxr_stage.GetPrimAtPath("/Sky")
    sky.GetAttribute("inputs:sunDirection").Set(Gf.Vec3f(0.0, 0.0, 1.0))

    # The sync is deferred to the next tick (mutating the stage inside its
    # own UsdNotice dispatch aborts USD); the app frame loop does this at
    # 60 Hz.
    stage.tick(1.0 / 60.0)
    assert _child_row2(pxr_stage) == pytest.approx(
        Gf.Vec3d(0.0, 0.0, -1.0), abs=1e-4)

    # A no-op second edit (unrelated attribute) must not corrupt the child.
    sky.CreateAttribute("inputs:turbidity", Sdf.ValueTypeNames.Float).Set(4.0)
    assert _child_row2(pxr_stage) == pytest.approx(
        Gf.Vec3d(0.0, 0.0, -1.0), abs=1e-4)


def test_persistence_via_modifier_sidecar(tmp_path):
    """The editor path authors into the session layer; save() + reload
    restores the synced direction from the _modifiers sidecar."""
    import stage_py
    from pxr import Gf

    p = tmp_path / "rig_persist.usda"
    _build_rig(p, yup_identity=True, sun_dir=(0.0, 1.0, 0.0))
    stage = stage_py.Stage(str(p))
    pxr_stage = stage.get_pxr_stage()

    sky = pxr_stage.GetPrimAtPath("/Sky")
    sky.GetAttribute("inputs:sunDirection").Set(Gf.Vec3f(1.0, 0.0, 0.0))
    stage.tick(1.0 / 60.0)
    assert _child_row2(pxr_stage) == pytest.approx(
        Gf.Vec3d(-1.0, 0.0, 0.0), abs=1e-4)
    assert stage.save()

    reloaded = stage_py.Stage(str(p))
    assert _child_row2(reloaded.get_pxr_stage()) == pytest.approx(
        Gf.Vec3d(-1.0, 0.0, 0.0), abs=1e-4)


def test_rig_without_sun_child_is_valid(tmp_path):
    """A sky without a DistantLight child syncs as a no-op success."""
    import stage_py
    from pxr import Gf, Sdf, Usd

    p = tmp_path / "rig_no_sun.usda"
    stage_file = Usd.Stage.CreateNew(str(p))
    stage_file.DefinePrim("/Sky", "HosekWilkieSky")
    stage_file.GetRootLayer().Save()

    stage = stage_py.Stage(str(p))
    pxr_stage = stage.get_pxr_stage()
    sky = pxr_stage.GetPrimAtPath("/Sky")
    sky.GetAttribute("inputs:sunDirection").Set(Gf.Vec3f(1.0, 0.0, 0.0))
    # No exception: the emit fires, tick applies the sync, finds no child
    # and returns success.


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
