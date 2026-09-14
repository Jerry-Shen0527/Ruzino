#!/usr/bin/env python3
"""Generate the character walking-simulator demo scene (Z-up, meters).

Scene layout (all authored in USD — nothing is created at runtime):
  /Ground         human-scale checkerboard ground (demo_ground.slang)
  /Sun            DistantLight, path-traced hard shadows
  /Sky            Hosek-Wilkie analytic sky dome (rotated so its local +Y
                  up maps onto the Z-up world zenith)
  /Looks/*        UsdPreviewSurface materials for the character
  /Character      walking BOT: `character:controller = true` marks the root;
                  joints are plain Xform prims (Hips/Spine/.../L_Foot) with
                  capsule/box Mesh prims parented under each joint. The
                  character controller FK-animates the joints every frame by
                  authoring translate/rotate overs in the session layer.
  /FreeCamera     third-person camera state incl. followTarget=/Character

Run (any python with pxr on path, or from repo root):

    python scripts/gen_character_walk_scene.py
    python scripts/gen_character_walk_scene.py -o path/to/scene.usda

Then: Binaries/Release/Ruzino.exe <scene path>
"""
import argparse
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BIN = ROOT / "Binaries" / "Release"

import sys

sys.path.insert(0, str(BIN))
import os

os.environ.setdefault("PXR_USD_WINDOWS_DLL_PATH", str(BIN))

from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade


# --------------------------------------------------------------------------
# mesh helpers (Z-up world; character faces +Y at rest)
# --------------------------------------------------------------------------
def capsule_mesh(a, b, radius, radial=10):
    """Capsule from point a to point b (tuples). Returns (points, normals,
    counts, indices)."""
    a = Gf.Vec3f(*a)
    b = Gf.Vec3f(*b)
    axis = b - a
    axis.Normalize()
    t = Gf.Vec3f(0, 0, 1) if abs(axis[2]) < 0.9 else Gf.Vec3f(1, 0, 0)
    v1 = Gf.Cross(axis, t)
    v1.Normalize()
    v2 = Gf.Cross(axis, v1)
    v2.Normalize()

    pts = []
    nrm = []

    def ring(center, radial_dir_scale, normal_ref):
        # ring of points around `center`: offset = radial_dir_scale(theta)
        for j in range(radial):
            th = 2.0 * math.pi * j / radial
            d = v1 * math.cos(th) + v2 * math.sin(th)
            p = center + d * radial_dir_scale
            pts.append(p)
            n = (p - normal_ref)
            n.Normalize()
            nrm.append(n)

    def cap_rings(center, outward):
        # pole first, then the 50-degree ring — indices below assume this
        pts.append(center + outward * radius)
        nrm.append(Gf.Vec3f(outward[0], outward[1], outward[2]))
        psi = math.radians(50.0)
        for j in range(radial):
            th = 2.0 * math.pi * j / radial
            d = v1 * math.cos(th) + v2 * math.sin(th)
            p = center + outward * (radius * math.cos(psi)) \
                + d * (radius * math.sin(psi))
            pts.append(p)
            n = (p - center)
            n.Normalize()
            nrm.append(n)

    cap_rings(a, -axis)   # bottom pole + ring
    ring(a, radius, a)     # bottom equator
    ring(b, radius, b)     # top equator
    cap_rings(b, axis)     # top ring + pole

    counts = []
    indices = []
    n_bot_pole = 1
    n_bot_ring = radial  # 50-degree ring
    # fan: pole(0) -> ring[1..1+radial)
    for j in range(radial):
        jn = 1 + (j + 1) % radial
        counts.append(3)
        indices.extend([0, 1 + j, jn])
    r0 = 1 + radial              # bottom equator ring start
    r1 = r0 + radial             # top equator ring start
    for j in range(radial):
        jn = (j + 1) % radial
        counts.append(4)
        indices.extend(
            [1 + j, 1 + jn, r1 + jn, r1 + j])   # 50-ring -> bottom equator
    for j in range(radial):
        jn = (j + 1) % radial
        counts.append(4)
        indices.extend(
            [r0 + j, r0 + jn, r1 + jn, r1 + j])  # shaft quads
    r2 = r1 + radial             # top 50-ring start
    top_pole = r2 + radial
    for j in range(radial):
        jn = (j + 1) % radial
        counts.append(4)
        indices.extend([r1 + j, r1 + jn, r2 + jn, r2 + j])
    for j in range(radial):
        jn = (j + 1) % radial
        counts.append(3)
        indices.extend([top_pole, r2 + jn, r2 + j])
    return pts, nrm, counts, indices


def sphere_mesh(center, radius, radial=12, bands=8):
    center = Gf.Vec3f(*center)
    pts = []
    nrm = []
    for i in range(1, bands):
        psi = math.pi * i / bands
        for j in range(radial):
            th = 2.0 * math.pi * j / radial
            d = Gf.Vec3f(
                math.sin(psi) * math.cos(th),
                math.sin(psi) * math.sin(th),
                math.cos(psi))
            pts.append(center + d * radius)
            nrm.append(d)
    counts = []
    indices = []
    # bottom fan
    for j in range(radial):
        jn = (j + 1) % radial
        counts.append(3)
        indices.extend([len(pts), j, jn])
    pts.append(center + Gf.Vec3f(0, 0, -radius))
    nrm.append(Gf.Vec3f(0, 0, -1))
    for i in range(bands - 2):
        r0 = i * radial
        r1 = (i + 1) * radial
        for j in range(radial):
            jn = (j + 1) % radial
            counts.append(4)
            indices.extend([r0 + j, r0 + jn, r1 + jn, r1 + j])
    top_fan_base = len(pts)
    r_top = (bands - 2) * radial
    for j in range(radial):
        jn = (j + 1) % radial
        counts.append(3)
        indices.extend([top_fan_base, r_top + jn, r_top + j])
    pts.append(center + Gf.Vec3f(0, 0, radius))
    nrm.append(Gf.Vec3f(0, 0, 1))
    return pts, nrm, counts, indices


def box_mesh(center, size):
    cx, cy, cz = center
    sx, sy, sz = (s * 0.5 for s in size)
    faces = [
        # (normal, four corners CCW seen from outside)
        ((1, 0, 0), [(cx + sx, cy - sy, cz - sz), (cx + sx, cy + sy, cz - sz),
                     (cx + sx, cy + sy, cz + sz), (cx + sx, cy - sy, cz + sz)]),
        ((-1, 0, 0), [(cx - sx, cy + sy, cz - sz), (cx - sx, cy - sy, cz - sz),
                      (cx - sx, cy - sy, cz + sz), (cx - sx, cy + sy, cz + sz)]),
        ((0, 1, 0), [(cx + sx, cy + sy, cz - sz), (cx - sx, cy + sy, cz - sz),
                     (cx - sx, cy + sy, cz + sz), (cx + sx, cy + sy, cz + sz)]),
        ((0, -1, 0), [(cx - sx, cy - sy, cz - sz), (cx + sx, cy - sy, cz - sz),
                      (cx + sx, cy - sy, cz + sz), (cx - sx, cy - sy, cz + sz)]),
        ((0, 0, 1), [(cx + sx, cy - sy, cz + sz), (cx + sx, cy + sy, cz + sz),
                     (cx - sx, cy + sy, cz + sz), (cx - sx, cy - sy, cz + sz)]),
        ((0, 0, -1), [(cx - sx, cy - sy, cz - sz), (cx - sx, cy + sy, cz - sz),
                      (cx + sx, cy + sy, cz - sz), (cx + sx, cy - sy, cz - sz)]),
    ]
    pts = []
    nrm = []
    counts = []
    indices = []
    for normal, corners in faces:
        base = len(pts)
        for c in corners:
            pts.append(Gf.Vec3f(*c))
            nrm.append(Gf.Vec3f(*normal))
        counts.append(4)
        indices.extend([base, base + 1, base + 2, base + 3])
    return pts, nrm, counts, indices


def define_mesh(stage, path, points, normals, counts, indices, material):
    mesh = UsdGeom.Mesh.Define(stage, path)
    mesh.CreatePointsAttr().Set(points)
    mesh.CreateNormalsAttr().Set(normals)
    mesh.SetNormalsInterpolation("vertex")
    mesh.CreateFaceVertexCountsAttr().Set(counts)
    mesh.CreateFaceVertexIndicesAttr().Set(indices)
    rng = Gf.Range3f()
    for p in points:
        rng.UnionWith(p)
    mesh.CreateExtentAttr().Set(
        [Gf.Vec3f(*rng.GetMin()), Gf.Vec3f(*rng.GetMax())])
    mesh.GetPrim().ApplyAPI("MaterialBindingAPI")
    UsdShade.MaterialBindingAPI(mesh).Bind(material)
    return mesh


# --------------------------------------------------------------------------
# scene
# --------------------------------------------------------------------------
def _matrix_xform(translate, rotate_euler=(0.0, 0.0, 0.0)):
    # Gf row-vector convention: rotate first, then translate (R * T).
    m = Gf.Matrix4d().SetTranslate(Gf.Vec3d(*translate))
    if any(abs(a) > 1e-9 for a in rotate_euler):
        rot = Gf.Rotation(Gf.Vec3d(1, 0, 0), rotate_euler[0])
        rot *= Gf.Rotation(Gf.Vec3d(0, 1, 0), rotate_euler[1])
        rot *= Gf.Rotation(Gf.Vec3d(0, 0, 1), rotate_euler[2])
        rm = Gf.Matrix4d().SetRotate(rot)
        m = rm * m
    return m


def define_joint(stage, path, translate):
    # Single matrix xformOp per joint: some render paths mis-evaluate
    # multi-op CommonAPI stacks, matrix ops work everywhere.
    prim = UsdGeom.Xform.Define(stage, path)
    UsdGeom.Xformable(prim).MakeMatrixXform().Set(_matrix_xform(translate))
    return prim


def make_material(stage, path, color):
    mat = UsdShade.Material.Define(stage, path)
    out = mat.CreateSurfaceOutput()
    shader = UsdShade.Shader.Define(
        stage, Sdf.Path(str(path) + "/PreviewSurface"))
    shader.CreateIdAttr("UsdPreviewSurface")
    shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(*color))
    shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.6)
    out.ConnectToSource(shader.ConnectableAPI(), "surface",
                        UsdShade.AttributeType.Output)
    return mat


def build_character(stage):
    root = UsdGeom.Xform.Define(stage, "/Character")
    prim = root.GetPrim()
    prim.CreateAttribute("character:controller", Sdf.ValueTypeNames.Bool,
                         True).Set(True)
    prim.CreateAttribute("character:moveSpeed", Sdf.ValueTypeNames.Float,
                         True).Set(3.0)
    prim.CreateAttribute("character:runMultiplier", Sdf.ValueTypeNames.Float,
                         True).Set(1.9)
    prim.CreateAttribute("character:strideLength", Sdf.ValueTypeNames.Float,
                         True).Set(1.35)
    # Face -Y at start: the fixed Y-up sky convention leaves the bright sky
    # toward -Y, so the opening camera looks into the lit half. (Matrix op —
    # same reason as the joints.)
    UsdGeom.Xformable(prim).MakeMatrixXform().Set(
        _matrix_xform((0.0, 0.0, 0.0), (0.0, 0.0, 180.0)))

    body = make_material(stage, "/Looks/BodyMat", (0.80, 0.36, 0.12))
    limb = make_material(stage, "/Looks/LimbMat", (0.22, 0.25, 0.32))
    skin = make_material(stage, "/Looks/SkinMat", (0.87, 0.68, 0.55))

    # ---- torso chain ----------------------------------------------------
    define_joint(stage, "/Character/Hips", (0, 0, 0.98))
    define_mesh(stage, "/Character/Hips/PelvisMesh",
                *box_mesh((0, 0, -0.03), (0.30, 0.19, 0.14)), body)

    define_joint(stage, "/Character/Hips/Spine", (0, 0.02, 0.10))
    define_joint(stage, "/Character/Hips/Spine/Chest", (0, 0.01, 0.15))
    define_mesh(stage, "/Character/Hips/Spine/Chest/TorsoMesh",
                *box_mesh((0, 0.01, 0.13), (0.34, 0.20, 0.30)), body)

    define_joint(stage, "/Character/Hips/Spine/Chest/Neck", (0, 0, 0.28))
    define_joint(stage, "/Character/Hips/Spine/Chest/Neck/Head", (0, 0, 0.08))
    define_mesh(stage, "/Character/Hips/Spine/Chest/Neck/Head/HeadMesh",
                *sphere_mesh((0, 0.01, 0.08), 0.105), skin)

    # ---- arms (hang along -Z at rest) ------------------------------------
    for side, sx in (("L", 1), ("R", -1)):
        shoulder = f"/Character/Hips/Spine/Chest/{side}_UpperArm"
        define_joint(stage, shoulder, (sx * 0.215, 0, 0.21))
        define_mesh(stage, shoulder + "/UpperArmMesh",
                    *capsule_mesh((0, 0, 0), (0, 0, -0.26), 0.042), limb)
        elbow = f"{shoulder}/{side}_Forearm"
        define_joint(stage, elbow, (0, 0, -0.27))
        define_mesh(stage, elbow + "/ForearmMesh",
                    *capsule_mesh((0, 0, 0), (0, 0, -0.24), 0.037), skin)
        hand = f"{elbow}/{side}_Hand"
        define_joint(stage, hand, (0, 0, -0.25))
        define_mesh(stage, hand + "/HandMesh",
                    *box_mesh((0, 0, -0.07), (0.075, 0.05, 0.15)), skin)

    # ---- legs -------------------------------------------------------------
    for side, sx in (("L", 1), ("R", -1)):
        thigh = f"/Character/Hips/{side}_Thigh"
        define_joint(stage, thigh, (sx * 0.10, 0, -0.05))
        define_mesh(stage, thigh + "/ThighMesh",
                    *capsule_mesh((0, 0, 0), (0, 0, -0.42), 0.068), limb)
        shin = f"{thigh}/{side}_Shin"
        define_joint(stage, shin, (0, 0, -0.44))
        define_mesh(stage, shin + "/ShinMesh",
                    *capsule_mesh((0, 0, 0), (0, 0, -0.40), 0.054), limb)
        foot = f"{shin}/{side}_Foot"
        define_joint(stage, foot, (0, 0, -0.42))
        define_mesh(stage, foot + "/FootMesh",
                    *box_mesh((0, 0.05, -0.035), (0.09, 0.26, 0.07)), limb)


def build_ground(stage):
    mat = UsdShade.Material.Define(stage, "/GroundChecker")
    mat.GetPrim().CreateAttribute("config:shader_path",
                                  Sdf.ValueTypeNames.String).Set(
        "callables/demo_ground.slang")
    out = mat.CreateSurfaceOutput()
    dummy = UsdShade.Shader.Define(stage, "/GroundChecker/PreviewSurface")
    dummy.CreateIdAttr("UsdPreviewSurface")
    dummy.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(0.5, 0.5, 0.5))
    out.ConnectToSource(dummy.ConnectableAPI(), "surface",
                        UsdShade.AttributeType.Output)

    plane = UsdGeom.Mesh.Define(stage, "/Ground")
    DIV, HALF = 12, 60.0
    step = (2.0 * HALF) / DIV
    # Z-up stage: the character walks on the z=0 plane, so the ground spans
    # X/Y with z pinned to 0 — NOT the (x, 0, z) layout of the Y-up cloud
    # template, which is a vertical wall here. Quad winding BL->BR->TR->TL
    # keeps the right-hand face normal along +Z, matching the authored
    # normals so the floor is front-facing for a camera above it.
    pts, idx = [], []
    for iy in range(DIV + 1):
        for ix in range(DIV + 1):
            pts.append((ix * step - HALF, iy * step - HALF, 0.0))
    for iy in range(DIV):
        for ix in range(DIV):
            base = iy * (DIV + 1) + ix
            idx.extend([base, base + 1, base + (DIV + 1) + 1, base + (DIV + 1)])
    plane.CreatePointsAttr().Set(pts)
    plane.CreateFaceVertexCountsAttr().Set([4] * (DIV * DIV))
    plane.CreateFaceVertexIndicesAttr().Set(idx)
    plane.CreateNormalsAttr().Set([(0, 0, 1)] * len(pts))
    plane.SetNormalsInterpolation("vertex")
    plane.CreateExtentAttr().Set(
        [Gf.Vec3f(-HALF, -HALF, 0.0), Gf.Vec3f(HALF, HALF, 0.0)])
    plane.GetPrim().ApplyAPI("MaterialBindingAPI")
    UsdShade.MaterialBindingAPI(plane).Bind(mat)


def build_lights(stage):
    elev = math.radians(50.0)
    azim = math.radians(135.0)
    toward_sun = Gf.Vec3f(
        math.cos(elev) * math.cos(azim),
        math.cos(elev) * math.sin(azim),
        math.sin(elev))

    # HosekWilkieSky rig: the sky prim carries the atmosphere; its child
    # DistantLight is the sun. The renderer treats them as fully independent
    # lights — the child's direction is synced HERE (authoring time) via
    # stage_py.sync_sun_light, and the editor re-syncs it on sky edits, so
    # the saved stage is self-consistent for any render delegate.
    #
    # The sky evaluates in the dome's LOCAL frame (+Y up); this Z-up stage
    # authors a -90 deg X rotation to point the dome zenith at world +Z.
    # sunDirection is dome-local: the world toward-sun vector mapped through
    # the same transform. All attribute fallbacks (shader_path/turbidity/
    # albedo) come from the RuzinoSky codeless schema; intensity 2.0 keeps
    # the accepted daylight feel without the noon wash-out (LPM clips 0%).
    sky = stage.DefinePrim("/Sky", "HosekWilkieSky")
    dome = UsdLux.DomeLight(sky)
    dome_xf = Gf.Matrix4d().SetRotate(Gf.Rotation(Gf.Vec3d(1, 0, 0), -90))
    UsdGeom.Xformable(sky).AddTransformOp().Set(dome_xf)
    sun_local = dome_xf.TransformDir(Gf.Vec3d(toward_sun[0], toward_sun[1], toward_sun[2]))
    dome.CreateIntensityAttr().Set(2.0)
    sky.CreateAttribute(
        "inputs:turbidity", Sdf.ValueTypeNames.Float).Set(3.0)
    sky.CreateAttribute(
        "inputs:groundAlbedo", Sdf.ValueTypeNames.Float).Set(0.3)
    sky.CreateAttribute(
        "inputs:sunDirection", Sdf.ValueTypeNames.Float3).Set(
        Gf.Vec3f(sun_local[0], sun_local[1], sun_local[2]))

    sun = UsdLux.DistantLight.Define(stage, "/Sky/Sun")
    sun.CreateIntensityAttr().Set(5.0)
    sun.CreateAngleAttr().Set(0.53)

    # Sync the child light's direction from the sky (single xformOp on the
    # root layer so the generated file is self-contained; the math mirrors
    # stage/hosek_sky.cpp).
    import stage_py
    stage_py.sync_sun_light(stage, "/Sky", stage.GetRootLayer())


def build_camera(stage):
    cam = UsdGeom.Camera.Define(stage, "/FreeCamera")
    cam.CreateFocalLengthAttr().Set(35.0)
    cam.CreateHorizontalApertureAttr().Set(20.955)
    cam.CreateVerticalApertureAttr().Set(11.785)
    cam.CreateClippingRangeAttr().Set(Gf.Vec2f(0.05, 2000.0))

    prim = cam.GetPrim()
    # Third-person orbit state (read by the viewport's ThirdPersonCamera):
    # camera behind the character (which faces -Y, so behind = +Y offset,
    # yaw = pi/2), framed on its chest and locked on as a follow target.
    prim.CreateAttribute("third_person:target",
                         Sdf.ValueTypeNames.Double3).Set(Gf.Vec3d(0, 0, 1.05))
    prim.CreateAttribute("third_person:distance",
                         Sdf.ValueTypeNames.Double).Set(5.5)
    prim.CreateAttribute("third_person:yaw",
                         Sdf.ValueTypeNames.Double).Set(1.5708)
    prim.CreateAttribute("third_person:pitch",
                         Sdf.ValueTypeNames.Double).Set(0.31)
    prim.CreateAttribute("third_person:followTarget",
                         Sdf.ValueTypeNames.String, True).Set("/Character")
    prim.CreateAttribute("third_person:followHeightOffset",
                         Sdf.ValueTypeNames.Double, True).Set(1.05)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-o", "--output",
        default=str(BIN / "demo_scenes" / "character_walk.usda"))
    args = parser.parse_args()

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()

    stage = Usd.Stage.CreateNew(str(out))
    stage.SetMetadata("metersPerUnit", 1.0)
    stage.SetMetadata("upAxis", "Z")

    build_ground(stage)
    build_lights(stage)
    build_character(stage)
    build_camera(stage)

    stage.GetRootLayer().Save()
    print(f"[character_walk] wrote {out}")


if __name__ == "__main__":
    main()
