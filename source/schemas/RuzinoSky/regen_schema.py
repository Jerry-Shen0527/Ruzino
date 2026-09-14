#!/usr/bin/env python3
"""Regenerate the HosekWilkieSky codeless-schema artifacts in resources/.

Only needed when schema.usda.in changes — the generated artifacts are
checked in and the regular build has NO dependency on this script, on
usdGenSchema, or on jinja2.

Requirements:
  * pip install jinja2            (usdGenSchema's template engine)
  * the OpenUSD SDK source tree  (base schema layers MUST come from
    pxr/usd/{usd,usdLux}/schema.usda — the shipped generatedSchema.usda
    files have their /GLOBAL codegen metadata stripped, and usdGenSchema
    refuses them as base layers)
  * the runtime pxr package in Binaries/Release (usd_ms.dll import)

Usage:
    python source/schemas/RuzinoSky/regen_schema.py

Env overrides: RUZINO_SDK_DIR (default <repo>/SDK), RUZINO_BIN_DIR
(default <repo>/Binaries/Release).
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCHEMA_DIR = Path(__file__).resolve().parent
SDK_DIR = Path(os.environ.get("RUZINO_SDK_DIR", REPO / "SDK"))
BIN_DIR = Path(os.environ.get("RUZINO_BIN_DIR", REPO / "Binaries" / "Release"))


def find_sdk_source() -> Path:
    candidates = sorted((SDK_DIR / "OpenUSD" / "source").glob("OpenUSD-*"))
    # Build sub-dirs (e.g. OpenUSD-26.08-build-release) may shadow the
    # pristine source tree; prefer the shortest path that actually carries
    # the base schema layers usdGenSchema sublayers.
    candidates = [
        c for c in candidates if (c / "pxr" / "usd" / "usd" / "schema.usda").exists()
    ]
    if not candidates:
        sys.exit(
            f"no OpenUSD source tree with pxr/usd/usd/schema.usda under "
            f"{SDK_DIR}/OpenUSD/source"
        )
    return min(candidates, key=lambda p: len(str(p)))


def main() -> None:
    usd_src = find_sdk_source()
    genschema = (
        BIN_DIR / "SDK" / "OpenUSD" / "Release" / "lib" / "python"
        / "pxr" / "Usd" / "usdGenSchema.py"
    )
    if not genschema.exists():
        # SDK layouts differ; fall back to a direct search.
        hits = list((SDK_DIR).glob("OpenUSD/Release/lib/python/pxr/Usd/usdGenSchema.py"))
        if not hits:
            sys.exit(f"usdGenSchema.py not found under {SDK_DIR}")
        genschema = hits[0]

    usd_schema = usd_src / "pxr" / "usd" / "usd" / "schema.usda"
    usdlux_schema = usd_src / "pxr" / "usd" / "usdLux" / "schema.usda"
    for p in (usd_schema, usdlux_schema):
        if not p.exists():
            sys.exit(f"base schema layer missing: {p}")

    template = (SCHEMA_DIR / "schema.usda.in").read_text()
    # The '@...@' around each placeholder is usda asset-path syntax and must
    # survive substitution — replace the bare placeholder names only.
    filled = template.replace("RUZINO_USD_SOURCE_SCHEMA", usd_schema.as_posix())
    filled = filled.replace("RUZINO_USDLUX_SOURCE_SCHEMA", usdlux_schema.as_posix())

    env = dict(
        os.environ,
        PYTHONPATH=str(BIN_DIR),
        PXR_USD_WINDOWS_DLL_PATH=str(BIN_DIR),
    )
    env["PATH"] = str(BIN_DIR) + os.pathsep + env.get("PATH", "")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / "schema.usda").write_text(filled)
        subprocess.run(
            [sys.executable, str(genschema), "schema.usda", "."],
            cwd=tmp,
            env=env,
            check=True,
        )

        # plugInfo.json: strip the generator's leading '#' comments, then
        # resolve the @PLUG_INFO_*@ placeholders for a library-less
        # ("Type": "resource") plugin.
        raw = (tmp / "plugInfo.json").read_text().splitlines()
        info = json.loads("\n".join(l for l in raw if not l.strip().startswith("#")))
        plugin = info["Plugins"][0]
        plugin["LibraryPath"] = ""
        plugin["Root"] = ".."
        plugin["ResourcePath"] = "resources"
        pluginfo_out = json.dumps(info, indent=4) + "\n"

        schematics = (tmp / "generatedSchema.usda").read_text()

    # The schema registry opens <ResourcePath>/generatedSchema.usda — the
    # schematics must land DIRECTLY in resources/, not in a subdir.
    resources = SCHEMA_DIR / "resources"
    (resources / "RuzinoSky").exists() and shutil.rmtree(resources / "RuzinoSky")
    resources.mkdir(exist_ok=True)
    (resources / "plugInfo.json").write_text(pluginfo_out)
    (resources / "generatedSchema.usda").write_text(schematics)
    print(f"regenerated: {resources / 'plugInfo.json'}")
    print(f"regenerated: {resources / 'generatedSchema.usda'}")


if __name__ == "__main__":
    main()
