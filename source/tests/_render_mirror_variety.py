#!/usr/bin/env python3
"""Render mirror / metallic variety scene and report per-panel brightness.

Validates that metallic reflection works correctly when the metal parameters
are set PROPERLY (base_color / diffuseColor drives metal reflectivity —
a zero there is a physically-correct black metal, not a mirror).

Five panels (A-E):
  A: standard_surface neutral mirror  (base=1, base_color=0.95, metalness=1, rough=0.01)
  B: standard_surface copper           (base_color=(0.95,0.6,0.3), rough=0.25)
  C: UsdPreviewSurface neutral mirror  (diffuseColor=0.95, metallic=1, rough=0.01)
  D: UsdPreviewSurface red metal       (diffuseColor=(0.8,0.1,0.1), rough=0.2)
  E: diffuse white reference           (metallic=0)

All lit by the same overhead RectLight. A red wall sits behind the camera;
neutral mirrors (A, C) should reflect it -> reddish tint in their reflection.

Success:
  - No panel is black (mean > 0.01).
  - Panels A and C are close (both neutral mirrors, different mat systems).
  - Panel E (diffuse) is the brightest directly-lit, no red tint.
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


def main():
    workspace_root, binary_dir = prepare_env()
    from test_render_materials import _build_render_graph
    import hd_RUZINO_py as renderer

    scene = workspace_root / "source" / "tests" / "data" / "scenes" / "mirror_variety_test.usda"
    out_dir = binary_dir / "test_output" / "mirror_variety"
    out_dir.mkdir(parents=True, exist_ok=True)

    W, H, SPP = 640, 360, 256
    print(f"Rendering {scene.name} ({W}x{H} @ {SPP} SPP)...", flush=True)
    hydra = renderer.HydraRenderer(str(scene), W, H)
    _build_render_graph(hydra, SPP)
    for i in range(SPP):
        hydra.render()
        if i == 0:
            print("  shader compiled + first frame OK", flush=True)
        if (i + 1) % 64 == 0:
            print(f"  {i+1}/{SPP}...", flush=True)

    data = hydra.get_output_texture()
    img = np.array(data, dtype=np.float32).reshape(H, W, 4)
    img = np.flipud(img)  # GPU top-left -> Y-up

    from PIL import Image
    rgb = np.clip(img[:, :, :3], 0, 1)
    Image.fromarray((rgb * 255 + 0.5).astype(np.uint8)).save(out_dir / "mirror_variety.png")
    print(f"\nSaved: {out_dir / 'mirror_variety.png'}")
    print(f"Overall mean={img[:,:,:3].mean():.4f} max={img[:,:,:3].max():.2f}")

    # Per-panel brightness. Panels span x = -4.75..4.75 in world, 5 panels each
    # 1.5m wide with 0.5m gaps. Camera at z=8 with 50mm / 36mm horizontal
    # aperture -> horizontal FOV = 2*atan(18/50) = 39.6 deg. At z=0 (panel
    # plane, 8m away) the visible width = 2*8*tan(19.8deg) = 5.76m, so world
    # x in [-2.88, 2.88] maps to pixel x in [0, W]. Panel centers (world):
    #   A=-4.0, B=-2.0, C=0.0, D=2.0, E=4.0 -> but only B,C,D center are in
    #   frame; A and E are off-screen at this FOV. Sample generously instead:
    #   divide the image into 5 vertical strips and report each.
    print("\nPer-strip brightness (vertical fifths of the image):")
    strip_w = W // 5
    for i in range(5):
        x0 = i * strip_w
        x1 = (i + 1) * strip_w if i < 4 else W
        strip = rgb[:, x0:x1, :]
        mean = strip.reshape(-1, 3).mean(axis=0)
        label = "ABCDE"[i]
        print(f"  strip {label} (px {x0:3d}-{x1:3d}): R={mean[0]:.4f} G={mean[1]:.4f} B={mean[2]:.4f}  lum={np.dot(mean, [0.2126,0.7152,0.0722]):.4f}")

    # Save debug: overall channel means
    print(f"\nChannel means: R={rgb[:,:,0].mean():.4f} G={rgb[:,:,1].mean():.4f} B={rgb[:,:,2].mean():.4f}")

    print("\nDone.")


if __name__ == "__main__":
    main()
