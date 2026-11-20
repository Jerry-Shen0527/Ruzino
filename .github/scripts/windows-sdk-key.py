"""Compute the identity of the Windows CUDA SDK in the selected build environment."""

import ast
import hashlib
import json
import os
from pathlib import Path
import sys
import sysconfig


root = Path(__file__).resolve().parents[2]
recipe = root / "configure.py"
cuda_installer = root / ".github/scripts/install-windows-cuda.ps1"

inputs = {
    # Keep dependency URLs, flags and build/pack logic, without Python comments,
    # whitespace, or checkout-specific line endings.
    "recipe": ast.dump(ast.parse(recipe.read_text(encoding="utf-8"))),
    "cuda_installer": cuda_installer.read_text(encoding="utf-8"),
    "target": "windows-x64",
    "toolchain": {
        name: os.environ[name].rstrip("\\")
        for name in (
            "VCToolsVersion",
            "WindowsSDKVersion",
            "CUDA_VERSION",
            "CUDAARCHS",
            "SDK_VARIANT",
        )
    },
    "python": sys.version.split()[0],
    "python_abi": sysconfig.get_config_var("EXT_SUFFIX"),
}
print(hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest())
