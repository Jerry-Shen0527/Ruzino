#!/usr/bin/env python3
"""Isolated OpenUSD SDK build into a parallel prefix (no wipe, no Binaries copy).

Builds a new OpenUSD version against the dependency set already installed in
SDK/OpenUSD/<BaseVariant>, into SDK/OpenUSD/<Variant>, without touching the
existing SDK prefixes or Binaries (configure.py's process_usd wipes the
install prefix and rebuilds every dependency from scratch, then overwrites
Binaries -- too destructive for trying out a new USD version).

The script:
  1. copies the base prefix (deps + old USD install) to the target prefix,
     then strips the USD-only parts so build_usd's dep_is_installed marker
     (include/pxr/pxr.h) is gone and USD itself rebuilds;
  2. downloads/extracts the requested OpenUSD source into SDK/OpenUSD/source;
  3. invokes configure.build_usd() with the same flags process_usd uses.

Usage:
  python scripts/build_usd_isolated.py                # v26.08, base Release
  python scripts/build_usd_isolated.py --version 26.11
  python scripts/build_usd_isolated.py --base-variant Debug
"""

import argparse
import os
import shutil
import sys
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO)

import configure  # noqa: E402  (repo-root module; guarded by __main__)

# USD-owned pieces of the install prefix; everything else belongs to the
# bundled dependencies (MaterialX, OpenEXR, TBB, OIIO, ...) and is reused.
# Note: the top-level CHANGELOG.md/LICENSE/README.md/THIRD_PARTY.md are
# MaterialX's install, and bin/ also holds dep tools (oiiotool) -- both kept.
# lib/python is the pre-26.08 pxr bindings layout; 26.08+ installs them to
# lib/site-packages (unless PXR_PYTHON_INSTALL_DIR redirects, which our
# configure.py does for the official prefixes -- isolated prefixes keep the
# upstream default). Both are stripped so the base prefix's bindings never
# leak into the new build.
_USD_PREFIX_FILES = [
    "pxrConfig.cmake",
]
_USD_PREFIX_DIRS = [
    "include/pxr",
    "lib/cmake/pxr",
    "lib/python",
    "lib/site-packages",
    "lib/usd",
    "plugin/usd",
]


def _strip_usd_install(prefix):
    pxr_h = os.path.join(prefix, "include", "pxr", "pxr.h")
    if not os.path.exists(pxr_h):
        print(f"{pxr_h} absent -- prefix already dep-only")
        return
    removed = []
    for rel in _USD_PREFIX_FILES:
        p = os.path.join(prefix, rel)
        if os.path.exists(p):
            os.remove(p)
            removed.append(rel)
    for rel in _USD_PREFIX_DIRS:
        p = os.path.join(prefix, rel.replace("/", os.sep))
        if os.path.exists(p):
            shutil.rmtree(p, ignore_errors=True)
            removed.append(rel + "/")
    # monolithic lib + USD tools in bin
    for name in os.listdir(os.path.join(prefix, "lib")):
        if name.startswith("usd_ms"):
            os.remove(os.path.join(prefix, "lib", name))
            removed.append("lib/" + name)
    bin_dir = os.path.join(prefix, "bin")
    for name in os.listdir(bin_dir):
        if name.startswith(("usd", "sdftoimage")) and name.endswith(".exe"):
            os.remove(os.path.join(bin_dir, name))
            removed.append("bin/" + name)
    print(f"Stripped {len(removed)} USD-owned entries (e.g. {removed[:6]}...)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="26.08")
    ap.add_argument("--base-variant", default="Release")
    ap.add_argument("--variant", default=None,
                    help="target prefix name (default <version>-<base>)")
    ap.add_argument("--skip-download", action="store_true")
    args = ap.parse_args()

    variant = args.variant or f"{args.version}-{args.base_variant}"
    base_prefix = os.path.join(REPO, "SDK", "OpenUSD", args.base_variant)
    prefix = os.path.join(REPO, "SDK", "OpenUSD", variant)
    source_dir = os.path.join(REPO, "SDK", "OpenUSD", "source")
    src = os.path.join(source_dir, f"OpenUSD-{args.version}")
    zip_path = os.path.join(REPO, "SDK", "cache", f"v{args.version}.zip")

    if not os.path.isdir(base_prefix):
        sys.exit(f"Base prefix {base_prefix} does not exist")

    # 1. copy base prefix if the target does not exist yet
    if not os.path.isdir(prefix):
        print(f"Copying {base_prefix} -> {prefix} ...")
        shutil.copytree(base_prefix, prefix)
    else:
        print(f"Target prefix {prefix} already exists")

    # 2. strip the USD-only parts so USD rebuilds while deps are reused
    _strip_usd_install(prefix)

    # 3. fetch + extract source
    if not os.path.isdir(src):
        if not os.path.exists(zip_path):
            if args.skip_download:
                sys.exit(f"{zip_path} missing and --skip-download given")
            url = ("https://github.com/PixarAnimationStudios/"
                   f"OpenUSD/archive/refs/tags/v{args.version}.zip")
            print(f"Downloading {url} ...")
            configure.download_with_progress(url, zip_path)
        print(f"Extracting {zip_path} -> {source_dir} ...")
        with zipfile.ZipFile(zip_path) as zf:
            zf.extractall(source_dir)

    # 4. build USD only, into the isolated prefix
    sdk_python = os.path.join(REPO, "SDK", "python", "python.exe")
    print(f"Building OpenUSD {args.version} into {prefix} ...")
    configure.build_usd(prefix, src, args.base_variant, sdk_python)
    print("Done.")


if __name__ == "__main__":
    main()
