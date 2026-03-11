#!/usr/bin/env python3
"""
Post-install script to copy dependencies and resources to the install directory.

This script is run after 'cmake --install' to copy all runtime dependencies:
- DLLs from SDK
- Python runtime
- USD resources
- imgui.ini
- CUDA runtime libraries (if available)

Usage:
    python scripts/install_deps.py --install-dir <path> [--build-type <type>] [--with-tests] [--dry-run]
"""

import argparse
import os
import sys
import shutil
import platform
from pathlib import Path

# Import functions from configure.py
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from configure import (
    get_platform,
    get_binary_extension,
    is_windows,
    is_linux,
    is_macos,
    copytree_common_to_binaries,
    copy_python_dlls_to_binaries,
    copy_cuda_runtime_dlls_to_binaries,
    copy_imgui_ini_to_binaries,
)


def install_dependencies(
    install_dir: Path,
    build_type: str = "Release",
    with_tests: bool = False,
    dry_run: bool = False,
):
    """
    Install all runtime dependencies to the install directory.

    Args:
        install_dir: Target installation directory
        build_type: Build type (Release, Debug, RelWithDebInfo)
        with_tests: Whether to install test dependencies
        dry_run: If True, print actions without executing them
    """
    print(f"Installing dependencies to: {install_dir}")
    print(f"Build type: {build_type}")
    print(f"Install tests: {with_tests}")
    print(f"Dry run: {dry_run}")

    # Map RelWithDebInfo to Release for SDK folder selection
    sdk_folder = build_type
    if build_type == "RelWithDebInfo":
        sdk_folder = "Release"
        print(f"RelWithDebInfo build: Using Release SDK folder")

    # Create install directories
    bin_dir = install_dir / "bin"
    lib_dir = install_dir / "lib"

    if not dry_run:
        bin_dir.mkdir(parents=True, exist_ok=True)
        lib_dir.mkdir(parents=True, exist_ok=True)

    # Helper function to copy SDK files to install directory
    def copy_sdk_to_install(folder: str, dst: str = ""):
        """Copy files from SDK folder to install directory."""
        project_root = Path(__file__).parent.parent
        src_path = project_root / "SDK" / folder

        if not src_path.exists():
            print(f"  ⚠ SDK folder not found: {src_path}")
            return

        # Determine destination
        if dst:
            dst_path = bin_dir / dst
        else:
            dst_path = bin_dir

        if dry_run:
            print(f"  [DRY RUN] Would copy {folder} to {dst_path}")
            return

            # Copy files,        for root, dirs, files in os.walk(src_path):
            relative_path = os.path.relpath(root, src_path)
            dst_dir = dst_path / relative_path
            os.makedirs(dst_dir, exist_ok=True)

            for file in files:
                # Skip static/import libraries
                if file.endswith(".lib") or file.endswith(".a"):
                    print(f"  Skipping {os.path.join(root, file)}")
                    continue

                src_file = Path(root) / file
                dst_file = dst_dir / file
                shutil.copy2(src_file, dst_file)

        print(f"  ✓ Copied {folder} to {dst_path}")

    # Copy OpenUSD dependencies
    print("\n1. Installing OpenUSD dependencies...")
    copy_sdk_to_install(f"OpenUSD/{sdk_folder}/bin")
    copy_sdk_to_install(f"OpenUSD/{sdk_folder}/lib")
    copy_sdk_to_install(f"OpenUSD/{sdk_folder}/plugin")
    copy_sdk_to_install(f"OpenUSD/{sdk_folder}/libraries", dst="libraries")
    copy_sdk_to_install(f"OpenUSD/{sdk_folder}/resources", dst="resources")
    copy_sdk_to_install(f"OpenUSD/{sdk_folder}/lib/python", dst="")  # Python bindings

    # Copy Slang
    print("\n2. Installing Slang...")
    copy_sdk_to_install("slang/bin")

    # Copy D3D12 (Windows only)
    if is_windows():
        print("\n3. Installing D3D12...")
        copy_sdk_to_install("d3d12/bin")

    # Copy DXC
    print("\n4. Installing DXC...")
    copy_sdk_to_install("dxc/bin/x64")

    # Copy Embree
    print("\n5. Installing Embree...")
    copy_sdk_to_install("embree/bin")

    # Copy Python runtime
    print("\n6. Installing Python runtime...")
    project_root = Path(__file__).parent.parent
    sdk_python_dir = project_root / "SDK" / "python"

    if sdk_python_dir.exists():
        if dry_run:
            print(f"  [DRY RUN] Would copy Python to {bin_dir}")
        else:
            print(f"  Copying Python to {bin_dir}")
            # Copy entire Python installation
            for item in sdk_python_dir.iterdir():
                src_item = sdk_python_dir / item.name
                dst_item = bin_dir / item.name

                if src_item.is_dir():
                    if dst_item.exists():
                        shutil.rmtree(dst_item)
                    shutil.copytree(src_item, dst_item)
                else:
                    shutil.copy2(src_item, dst_item)
            print(f"  ✓ Python runtime installed")
    else:
        print(f"  ⚠ Python not found in SDK")

    # Copy CUDA runtime libraries if available
    print("\n7. Installing CUDA runtime libraries...")
    cuda_path = os.environ.get("CUDA_PATH")
    if not cuda_path and is_linux():
        cuda_path = os.environ.get("CUDA_HOME")

    if cuda_path:
        if is_windows():
            cuda_libs = [
                "cudart64_12.dll",
                "nvrtc64_120_0.dll",
                "cudart64_13.dll",
                "nvrtc64_130_0.dll",
            ]
            lib_dirs = [Path(cuda_path) / "bin", Path(cuda_path) / "bin" / "x64"]
        elif is_linux():
            cuda_libs = [
                "libcudart.so.12",
                "libnvrtc.so.12",
                "libcudart.so.13",
                "libnvrtc.so.13",
            ]
            lib_dirs = [
                Path(cuda_path) / "lib64",
                Path(cuda_path) / "lib",
                Path("/usr/local/cuda/lib64"),
                Path("/usr/lib/x86_64-linux-gnu"),
            ]
        elif is_macos():
            cuda_libs = [
                "libcudart.12.dylib",
                "libnvrtc.12.dylib",
                "libcudart.13.dylib",
                "libnvrtc.13.dylib",
            ]
            lib_dirs = [Path(cuda_path) / "lib", Path("/usr/local/cuda/lib")]
        else:
            cuda_libs = []
            lib_dirs = []

        for lib_name in cuda_libs:
            src_lib = None
            for lib_dir in lib_dirs:
                potential_path = lib_dir / lib_name
                if potential_path.exists():
                    src_lib = potential_path
                    break

            if src_lib:
                dst_lib = bin_dir / lib_name

                if dry_run:
                    print(f"  [DRY RUN] Would copy {lib_name}")
                else:
                    shutil.copy2(src_lib, dst_lib)
                    print(f"  ✓ Copied {lib_name}")
            else:
                print(f"  ⚠ {lib_name} not found, skipping")
    else:
        env_var = "CUDA_PATH" if is_windows() else "CUDA_PATH/CUDA_HOME"
        print(f"  ⚠ {env_var} not set, skipping CUDA runtime libraries")

    # Copy imgui.ini
    print("\n8. Installing imgui.ini...")
    project_root = Path(__file__).parent.parent
    src_file = project_root / "tests" / "application" / "imgui.ini"

    if src_file.exists():
        dst_file = bin_dir / "imgui.ini"
        if dry_run:
            print(f"  [DRY RUN] Would copy imgui.ini")
        else:
            shutil.copy2(src_file, dst_file)
            print(f"  ✓ Copied imgui.ini")
    else:
        print(f"  ⚠ imgui.ini not found at {src_file}")

    # Summary
    print("\n" + "=" * 80)
    print("DEPENDENCY INSTALLATION SUMMARY")
    print("=" * 80)
    print(f"Install directory: {install_dir}")
    print(f"Build type: {build_type}")
    print(f"Dependencies installed successfully!")
    print("=" * 80)


def main():
    parser = argparse.ArgumentParser(
        description="Install runtime dependencies to install directory"
    )
    parser.add_argument(
        "--install-dir",
        type=str,
        required=True,
        help="Installation directory (e.g., ../RuzinoInstall)",
    )
    parser.add_argument(
        "--build-type",
        type=str,
        default="Release",
        choices=["Release", "Debug", "RelWithDebInfo"],
        help="Build type (default: Release)",
    )
    parser.add_argument(
        "--with-tests", action="store_true", help="Also install test dependencies"
    )
    parser.add_argument(
        "--dry-run",
        "-n",
        action="store_true",
        help="Print actions without executing them",
    )

    args = parser.parse_args()

    install_dir = Path(args.install_dir)
    build_type = args.build_type
    with_tests = args.with_tests
    dry_run = args.dry_run

    # Make install_dir absolute if it's relative
    if not install_dir.is_absolute():
        install_dir = Path(__file__).parent.parent / install_dir

    install_dependencies(install_dir, build_type, with_tests, dry_run)


if __name__ == "__main__":
    main()
