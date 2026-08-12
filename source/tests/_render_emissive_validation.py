#!/usr/bin/env python3
"""Plan A + B validation: render and compare emissive mesh correctness.

Plan A: emissive panel vs RectLight equivalence.
  Renders two scenes that differ ONLY in light source type (RectLight vs
  emissive mesh rectangle, same size, same Le). Compares floor brightness.
  Ratio should be ≈ 1.0.

Plan B: mirror reflection symmetry.
  Renders a glowing box above a mirror floor. Compares the box's brightness
  seen directly (upper half) vs via mirror reflection (lower half).
  Ratio should be ≈ 1.0.
"""
import os
import sys
from pathlib import Path

import numpy as np


def prepare_env():
    workspace_root = Path(__file__).resolve().parent.parent.parent
    binary_dir = workspace_root / "Binaries" / "Release"
    os.environ.setdefault('PXR_USD_WINDOWS_DLL_PATH', str(binary_dir))
    mtlx = binary_dir / "libraries"
    if mtlx.exists():
        os.environ.setdefault('PXR_MTLX_STDLIB_SEARCH_PATHS', str(mtlx))
    os.environ['PATH'] = str(binary_dir) + os.pathsep + os.environ.get('PATH', '')
    if hasattr(os, 'add_dll_directory'):
        try:
            os.add_dll_directory(str(binary_dir))
        except Exception:
            pass
    if str(binary_dir) not in sys.path:
        sys.path.insert(0, str(binary_dir))
    sys.path.insert(0, str(workspace_root / "source" / "tests"))
    return workspace_root, binary_dir


def render_scene(scene_path, w, h, spp):
    """Render one scene in a FRESH subprocess, return float32 HxWx4 image.

    Rendering multiple scenes in one process corrupts the renderer's global
    state (RHI / MaterialX shared document), causing the 3rd scene to render
    black. A subprocess per scene sidesteps this entirely. Results are
    exchanged via a temp .npy file.
    """
    import subprocess
    import sys
    import tempfile

    workspace_root = Path(__file__).resolve().parent.parent.parent
    out_npz = tempfile.NamedTemporaryFile(
        suffix=".npy", delete=False, dir=str(workspace_root / "Binaries" / "Release"))
    out_npz.close()

    script = f'''
import sys, os
from pathlib import Path
import numpy as np
binary_dir = Path(r"{workspace_root / "Binaries" / "Release"}")
os.environ["PATH"] = str(binary_dir) + os.pathsep + os.environ.get("PATH", "")
os.environ.setdefault("PXR_USD_WINDOWS_DLL_PATH", str(binary_dir))
mtlx = binary_dir / "libraries"
if mtlx.exists():
    os.environ.setdefault("PXR_MTLX_STDLIB_SEARCH_PATHS", str(mtlx))
if hasattr(os, "add_dll_directory"):
    try: os.add_dll_directory(str(binary_dir))
    except Exception: pass
sys.path.insert(0, str(binary_dir))
sys.path.insert(0, r"{workspace_root / "source" / "tests"}")
from test_render_materials import _build_render_graph
import hd_RUZINO_py as renderer
hydra = renderer.HydraRenderer(r"{scene_path}", {w}, {h})
_build_render_graph(hydra, {spp})
for _ in range({spp}):
    hydra.render()
data = hydra.get_output_texture()
img = np.array(data, dtype=np.float32).reshape({h}, {w}, 4)
img = np.flipud(img)
np.save(r"{out_npz.name}", img)
'''
    result = subprocess.run(
        [sys.executable, "-c", script],
        capture_output=True, text=True,
        cwd=str(workspace_root / "Binaries" / "Release"))
    if result.returncode != 0:
        print("  SUBPROCESS FAILED:")
        print(result.stderr[-2000:])
        return np.zeros((h, w, 4), dtype=np.float32)
    img = np.load(out_npz.name)
    os.unlink(out_npz.name)
    return img


def save_png(img, path):
    from PIL import Image
    rgb = np.clip(img[:, :, :3], 0, 1)
    Image.fromarray((rgb * 255 + 0.5).astype(np.uint8)).save(path)


def main():
    workspace_root, binary_dir = prepare_env()
    scenes = workspace_root / "source" / "tests" / "data" / "scenes"
    out = binary_dir / "test_output" / "emissive_validation"
    out.mkdir(parents=True, exist_ok=True)

    W, H, SPP = 384, 256, 512

    # =====================================================================
    # Plan A: emissive vs RectLight equivalence
    # =====================================================================
    print("=" * 60)
    print("PLAN A: emissive mesh vs RectLight equivalence")
    print("=" * 60)

    rect_scene = scenes / "emissive_vs_rectlight_rect.usda"
    emis_scene = scenes / "emissive_vs_rectlight_emissive.usda"

    if not rect_scene.exists() or not emis_scene.exists():
        print("SKIP: scene files missing")
    else:
        print(f"\nRendering RectLight scene ({W}x{H} @ {SPP} SPP)...", flush=True)
        img_rect = render_scene(rect_scene, W, H, SPP)
        save_png(img_rect, out / "planA_rectlight.png")
        print(f"  Saved planA_rectlight.png")

        print(f"\nRendering emissive mesh scene ({W}x{H} @ {SPP} SPP)...", flush=True)
        img_emis = render_scene(emis_scene, W, H, SPP)
        save_png(img_emis, out / "planA_emissive.png")
        print(f"  Saved planA_emissive.png")

        # Compare floor brightness. The floor is the large flat area in the
        # lower ~60% of the image. Sample center-bottom region.
        rgb_rect = img_rect[:, :, :3]
        rgb_emis = img_emis[:, :, :3]

        # Floor region: bottom 60%, center 60% (avoid edges/corners)
        y0, y1 = int(H * 0.4), int(H * 0.95)
        x0, x1 = int(W * 0.2), int(W * 0.8)
        floor_rect = rgb_rect[y0:y1, x0:x1]
        floor_emis = rgb_emis[y0:y1, x0:x1]

        mean_rect = float(floor_rect.mean())
        mean_emis = float(floor_emis.mean())
        ratio = mean_emis / mean_rect if mean_rect > 1e-6 else 0.0

        print(f"\n  RectLight  floor mean = {mean_rect:.6f}")
        print(f"  Emissive   floor mean = {mean_emis:.6f}")
        print(f"  Ratio (emissive/rect) = {ratio:.4f}")
        print(f"  Target: ratio ≈ 1.0 (±0.05 for MC noise)")

        # Per-channel comparison
        for ch, name in enumerate("RGB"):
            cr = float(floor_rect[:, :, ch].mean())
            ce = float(floor_emis[:, :, ch].mean())
            r = ce / cr if cr > 1e-6 else 0
            print(f"    {name}: rect={cr:.6f} emis={ce:.6f} ratio={r:.4f}")

    # =====================================================================
    # Plan B: mirror reflection symmetry
    # =====================================================================
    print("\n" + "=" * 60)
    print("PLAN B: mirror reflection symmetry")
    print("=" * 60)

    mirror_scene = scenes / "emissive_mirror_symmetry.usda"
    if not mirror_scene.exists():
        print("SKIP: scene file missing")
    else:
        print(f"\nRendering mirror scene ({W}x{H} @ {SPP} SPP)...", flush=True)
        img_mir = render_scene(mirror_scene, W, H, SPP)
        save_png(img_mir, out / "planB_mirror.png")
        print(f"  Saved planB_mirror.png")

        rgb = img_mir[:, :, :3]
        h, w = rgb.shape[:2]

        # Down-looking camera: the emissive box (y=1.6) appears in the UPPER
        # part of the frame (direct view), and its reflection in the mirror
        # floor (y=0) appears in the LOWER part. Find bright pixels in both
        # halves and compare — the reflection should be ≈ as bright as the
        # direct view (roughness=0.02 keeps the mirror near-perfect).

        # Find bright pixels (the emissive box is the brightest object)
        bright_mask = rgb.mean(axis=2) > 0.15  # tune threshold

        # Split into upper half (direct) and lower half (reflection)
        mid = h // 2
        upper_mask = bright_mask[:mid, :]
        lower_mask = bright_mask[mid:, :]

        upper_bright = rgb[:mid][upper_mask]
        lower_bright = rgb[mid:][lower_mask]

        if len(upper_bright) > 10 and len(lower_bright) > 10:
            mean_direct = float(upper_bright.mean())
            mean_reflected = float(lower_bright.mean())
            ratio = mean_reflected / mean_direct if mean_direct > 1e-6 else 0

            print(f"\n  Direct view (upper)    box mean = {mean_direct:.6f}  ({len(upper_bright)} px)")
            print(f"  Reflected (lower)      box mean = {mean_reflected:.6f}  ({len(lower_bright)} px)")
            print(f"  Ratio (reflected/direct) = {ratio:.4f}")
            print(f"  Target: ratio ≈ 1.0 (slightly <1 due to roughness=0.02)")

            for ch, name in enumerate("RGB"):
                cd = float(upper_bright[:, ch].mean())
                cr = float(lower_bright[:, ch].mean())
                r = cr / cd if cd > 1e-6 else 0
                print(f"    {name}: direct={cd:.6f} reflected={cr:.6f} ratio={r:.4f}")
        else:
            print(f"\n  Could not find box in both halves (upper={len(upper_bright)}, lower={len(lower_bright)} px)")
            mean = float(rgb.mean())
            print(f"  Overall mean = {mean:.6f}")

    print("\nDone. PNGs in:", out)


if __name__ == "__main__":
    main()
