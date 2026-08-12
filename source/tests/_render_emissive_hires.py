#!/usr/bin/env python3
"""High-resolution, high-SPP renders of emissive mesh scenes for visual review.

Renders both scenes at 768×512 / 512 SPP with the full tone-mapping pipeline
(LPM + gamma), matching the production render graph. Saves PNGs to
Binaries/Release/test_output/emissive_hires/.
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
    return workspace_root, binary_dir


def main():
    workspace_root, binary_dir = prepare_env()
    # Reuse the canonical render graph builder (rng→raygen→pt→accumulate→
    # lpm→gamma→present) from the materials test.
    sys.path.insert(0, str(workspace_root / "source" / "tests"))
    from test_render_materials import _build_render_graph

    import hd_RUZINO_py as renderer

    scenes_dir = workspace_root / "source" / "tests" / "data" / "scenes"
    out_dir = binary_dir / "test_output" / "emissive_hires"
    out_dir.mkdir(parents=True, exist_ok=True)

    jobs = [
        ("emissive_single", scenes_dir / "emissive_mesh.usda", 512, 512, 512),
        ("emissive_multi", scenes_dir / "emissive_many_lights.usda", 768, 512, 512),
    ]

    for name, scene_path, w, h, spp in jobs:
        if not scene_path.exists():
            print(f"SKIP {name}: {scene_path} not found")
            continue
        print(f"\n=== {name}: {w}x{h} @ {spp} SPP ===", flush=True)
        hydra = renderer.HydraRenderer(str(scene_path), w, h)
        _build_render_graph(hydra, spp)
        for i in range(spp):
            hydra.render()
            if i == 0:
                print(f"  shader compiled + first frame OK", flush=True)
            if (i + 1) % 128 == 0:
                print(f"  {i+1}/{spp}...", flush=True)

        data = hydra.get_output_texture()
        img = np.array(data, dtype=np.float32).reshape(h, w, 4)
        img = np.flipud(img)  # GPU top-left origin → Y-up

        rgb = np.clip(img[:, :, :3], 0, 1)
        from PIL import Image
        Image.fromarray((rgb * 255 + 0.5).astype(np.uint8)).save(out_dir / f"{name}.png")
        print(f"  Saved: {out_dir / f'{name}.png'}")
        print(f"  mean={img[:,:,:3].mean():.4f} max={img[:,:,:3].max():.2f}")


if __name__ == "__main__":
    main()
