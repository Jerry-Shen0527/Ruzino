#!/usr/bin/env python3
"""
UNBIASED procedural cloud render test (delta tracking / ratio tracking).

Same scene as render_clouds.py, but the cloud volume carries
volumeType="cloud_unbiased": the renderer routes it to the unbiased hit
groups (SBT slots 8/9 — delta tracking for free-flight sampling, ratio
tracking for transmittance, phase-sampled path continuation for multiple
scattering). No quadrature steps, no heuristic multi-scatter/powder terms —
the estimate is unbiased; noise is bought down by frame accumulation.

Run from Binaries/Release:

    python ../../source/tests/render_cloud_unbiased.py

Outputs land in Binaries/Release/test_output/clouds/cloud_unbiased_*.png,
next to the biased pipeline's cloud_sunny.png / cloud_overcast.png for A/B.
"""
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BIN = ROOT / "Binaries" / "Release"

sys.path.insert(0, str(BIN))
sys.path.insert(0, str(ROOT / "source" / "Core" / "rznode" / "python"))
sys.path.insert(0, str(ROOT / "source" / "Runtime" / "renderer" / "python"))
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(BIN)
os.environ["PATH"] = str(BIN) + os.pathsep + os.environ.get("PATH", "")
os.add_dll_directory(str(BIN))

sys.path.insert(0, str(HERE))
from pxr import Usd, Sdf


def main():
    import numpy as np
    import hd_RUZINO_py as renderer
    import render_clouds as rc  # reuse the biased pipeline's scene + graph

    out_dir = BIN / "test_output" / "clouds"
    out_dir.mkdir(parents=True, exist_ok=True)
    scene_dir = BIN / "cloud_scenes"
    scene_dir.mkdir(parents=True, exist_ok=True)

    WIDTH, HEIGHT, SPP = 640, 480, 4096

    cases = [
        # Same shaping params as the biased pipeline so the two are directly
        # comparable — only the integration pipeline differs.
        ("cloud_unbiased_sunny", 0.20, 0.06, 55.0),
        ("cloud_unbiased_overcast", 0.08, 0.05, 40.0),
    ]
    # Optional filter: python render_cloud_unbiased.py sunny|overcast
    if len(sys.argv) > 1:
        cases = [c for c in cases if sys.argv[1] in c[0]]

    for name, cov, dens, elev in cases:
        scene = scene_dir / f"{name}.usda"
        rc._build_scene(scene, coverage=cov, density=dens, sun_elev_deg=elev)
        # Flip the volume type token to route through the unbiased pipeline.
        st = Usd.Stage.Open(str(scene))
        vol = st.GetPrimAtPath("/CloudLayer")
        pv = vol.GetAttribute("primvars:volumeType")
        pv.Set("cloud_unbiased")
        st.GetRootLayer().Save()
        print(f"\n[{name}] coverage={cov} density={dens} elev={elev}deg")

        hydra = renderer.HydraRenderer(str(scene), WIDTH, HEIGHT)
        rc._build_render_graph(hydra, SPP)
        for _ in range(SPP):
            hydra.render()
        tex = hydra.get_output_texture()
        img = np.array(tex, dtype=np.float32).reshape(HEIGHT, WIDTH, 4)
        img = np.flipud(img)
        rgb = np.clip(img[:, :, :3], 0, 1)

        h = HEIGHT
        sky = rgb[: h // 3].mean(axis=(0, 1))
        mid = rgb[h // 3: 2 * h // 3].mean(axis=(0, 1))
        gnd = rgb[2 * h // 3:].mean(axis=(0, 1))
        finite = np.isfinite(img).all()

        from PIL import Image
        Image.fromarray((rgb * 255).astype(np.uint8)).save(out_dir / f"{name}.png")
        print(f"  saved {out_dir / (name + '.png')}")
        print(f"  finite={finite}")
        print(f"  sky mean RGB   = ({sky[0]:.3f},{sky[1]:.3f},{sky[2]:.3f})")
        print(f"  cloud mean RGB = ({mid[0]:.3f},{mid[1]:.3f},{mid[2]:.3f})")
        print(f"  ground mean RGB= ({gnd[0]:.3f},{gnd[1]:.3f},{gnd[2]:.3f})")
        if not finite:
            print(f"  !! WARNING: {name} contains NaN/Inf")

    print(f"\nDone. PNGs in {out_dir}")


if __name__ == "__main__":
    main()
