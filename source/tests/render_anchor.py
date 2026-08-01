"""Generate (or regenerate) the Cornell Box reference anchor image.

This is the *only* entry point permitted to write a render product into the
source tree, and only into ``source/tests/data/reference/``. The anchor is a
high-SPP render that the convergence test in ``test_render_materials.py``
converges toward. Generate once, commit, reuse.

Run from Binaries/Release so node-plugin DLLs and render_nodes.json resolve::

    cd Binaries/Release
    python ../../source/tests/render_anchor.py            # 1024x1024, 512 SPP
    python ../../source/tests/render_anchor.py --size 512 --spp 256  # lighter

The render graph construction mirrors ``test_render_materials._build_render_graph``
(imported from there so the two stay in lockstep).
"""

import argparse
import os
import sys
from pathlib import Path

# Allow running as a script from Binaries/Release: add source/tests to sys.path.
HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

# conftest.py sets up sys.path/DLL dirs/chdir for the binary_dir. Importing it as
# a module reproduces that setup when running this file as a plain script
# (pytest would do it automatically). conftest must be importable as a module:
# put source/tests first (done above) and import under its real module name.
import conftest  # noqa: F401,E402  (side effect: sys.path/chdir/dll setup)
import _render_compare  # noqa: E402
from test_render_materials import _build_render_graph, _render  # noqa: E402

PROJECT_ROOT = HERE.parent.parent
DATA_DIR = HERE / "data"
REFERENCE_DIR = DATA_DIR / "reference"
SCENE = DATA_DIR / "scenes" / "cornell_box.usda"
DEFAULT_ANCHOR = REFERENCE_DIR / "cornell_box_anchor"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--size", type=int, default=1024,
                    help="image resolution (square), default 1024")
    ap.add_argument("--spp", type=int, default=512,
                    help="anchor sample count, default 512")
    ap.add_argument("--name", default="cornell_box_anchor",
                    help="anchor stem under data/reference/, "
                         "default cornell_box_anchor")
    ap.add_argument("--force", action="store_true",
                    help="regenerate even if an anchor already exists")
    args = ap.parse_args()

    if not SCENE.exists():
        sys.exit(f"scene not found: {SCENE}")

    anchor_path = REFERENCE_DIR / args.name
    anchor_npy = anchor_path.with_suffix(".npy")
    anchor_png = anchor_path.with_suffix(".png")

    # Reuse a committed anchor if one exists and --force is not set; otherwise
    # render fresh. This script runs in its own process so a fresh HydraRenderer
    # is fine (unlike the pytest convergence test, which must reuse one).
    existing = _render_compare.load_anchor(anchor_path)
    if existing is not None and not args.force:
        h, w = existing.shape[:2]
        mean = float(existing[..., :3].mean())
        print(f"Anchor already exists: {anchor_png.name} ({w}x{h}) mean RGB={mean:.4f}")
        print("  use --force to regenerate.")
        return

    print(f"Generating anchor: {args.size}x{args.size} @ {args.spp} SPP")
    print(f"  scene:   {SCENE}")
    print(f"  output:  {anchor_png.name} / {anchor_npy.name}")
    img = _render(SCENE, args.size, args.size, args.spp)

    import numpy as np
    REFERENCE_DIR.mkdir(parents=True, exist_ok=True)
    np.save(anchor_npy, img)
    _render_compare._save_png(img, anchor_png)

    mean = float(img[..., :3].mean())
    finite = bool(np.isfinite(img).all())
    print(f"  mean RGB={mean:.4f}  finite={finite}")
    print("Done. Review the PNG; if it looks right, commit it as the reference.")


if __name__ == "__main__":
    main()
