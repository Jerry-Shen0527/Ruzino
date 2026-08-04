"""Render cornell_box.usda at 1024 SPP for reference-image evaluation.

Standalone script (not a pytest). Reuses the test_render_materials helpers to
build the standard path-tracing render graph, then renders 1024 frames into the
accumulator. Output: Binaries/Release/test_output/render_materials/cornell_1024.{npy,png}
at 256x256 to match the anchor format.
"""
import sys
from pathlib import Path

# Match conftest path setup so hd_RUZINO_py resolves.
import os
import platform

PROJECT_ROOT = Path(__file__).resolve().parents[2]
binary_dir = PROJECT_ROOT / "Binaries" / "Release"
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(binary_dir)
sys.path.insert(0, str(binary_dir))
os.environ["PATH"] = str(binary_dir) + os.pathsep + os.environ.get("PATH", "")
if platform.system() == "Windows":
    os.add_dll_directory(str(binary_dir))
    for sub in ("SDK/python", "SDK/OpenUSD/Release/lib"):
        d = PROJECT_ROOT / sub
        if d.is_dir():
            os.add_dll_directory(str(d))

sys.path.insert(0, str(PROJECT_ROOT / "source/Core/rznode/python"))
sys.path.insert(0, str(PROJECT_ROOT / "source/Runtime/renderer/python"))
sys.path.insert(0, str(PROJECT_ROOT / "source/tests"))

import numpy as np
from PIL import Image
import test_render_materials as trm

SCENE = trm.DATA_DIR / "scenes" / "cornell_box.usda"
OUT_DIR = PROJECT_ROOT / "Binaries/Release/test_output/render_materials"
OUT_DIR.mkdir(parents=True, exist_ok=True)

SPP = 1024
SIZE = 256  # match anchor

print(f"Rendering {SCENE.name} at {SPP} SPP, {SIZE}x{SIZE}...")
img = trm._render(SCENE, width=SIZE, height=SIZE, samples=SPP, save_name="cornell_1024")

npy_path = OUT_DIR / "cornell_1024.npy"
np.save(npy_path, img.astype(np.float32))
print(f"Saved: {npy_path}")
print(f"mean={img[..., :3].mean():.4f}  shape={img.shape}")
