import zipfile
import shutil
import os
import requests
from tqdm import tqdm
import argparse


def copytree_common_to_binaries(folder, target="Debug", dst=None, dry_run=False):
    root_dir = os.getcwd()
    dst_path = os.path.join(root_dir, "Binaries", target, dst or "")
    if dry_run:
        print(f"[DRY RUN] Would copy {folder} to {dst_path}")
    else:
        # For RelWithDebInfo, use Release SDK folder if the folder path contains OpenUSD/<target>
        src_folder = folder
        if target == "RelWithDebInfo" and "/RelWithDebInfo" in folder.replace(
            "\\", "/"
        ):
            src_folder = folder.replace("RelWithDebInfo", "Release").replace(
                "/RelWithDebInfo/", "/Release/"
            )
            print(
                f"RelWithDebInfo: Using Release SDK folder, mapping {folder} -> {src_folder}"
            )

        src_path = os.path.join(os.path.dirname(__file__), "SDK", src_folder)
        for root, dirs, files in os.walk(src_path):
            relative_path = os.path.relpath(root, src_path)
            dst_dir = os.path.join(dst_path, relative_path)
            os.makedirs(dst_dir, exist_ok=True)
            for file in files:
                if file.endswith(".lib"):
                    print(f"Skipping {os.path.join(root, file)}")
                    continue
                src_file = os.path.join(root, file)
                dst_file = os.path.join(dst_dir, file)
                shutil.copy2(src_file, dst_file)
        print(f"Copied {src_folder} to {dst_path}")


def copy_imgui_ini_to_binaries(targets, dry_run=False):
    """Copy imgui.ini from tests/application/ to each target in Binaries/"""
    src_file = os.path.join(
        os.path.dirname(__file__), "tests", "application", "imgui.ini"
    )

    if not os.path.exists(src_file):
        print(f"  ⚠ imgui.ini not found at {src_file}, skipping")
        return

    for target in targets:
        target_dir = os.path.join(os.getcwd(), "Binaries", target)
        dst_file = os.path.join(target_dir, "imgui.ini")

        if dry_run:
            print(f"  [DRY RUN] Would copy imgui.ini to Binaries/{target}/")
        else:
            os.makedirs(target_dir, exist_ok=True)
            try:
                shutil.copy2(src_file, dst_file)
                print(f"  ✓ Copied imgui.ini to Binaries/{target}/")
            except Exception as e:
                print(f"  ✗ Failed to copy imgui.ini to Binaries/{target}/: {e}")


def copy_nvapi_header_to_slang(dry_run=False):
    """Copy nvapi headers (nvHLSLExtns.h, nvHLSLExtnsInternal.h, nvShaderExtnEnums.h) from external/nvapi/ to SDK/slang/include/"""
    nvapi_headers = ["nvHLSLExtns.h", "nvHLSLExtnsInternal.h", "nvShaderExtnEnums.h"]

    src_dir = os.path.join(os.path.dirname(__file__), "external", "nvapi")
    dst_dir = os.path.join(os.path.dirname(__file__), "SDK", "slang", "include")

    if dry_run:
        print(f"  [DRY RUN] Would copy nvapi headers to SDK/slang/include/")
        return

    os.makedirs(dst_dir, exist_ok=True)

    for header in nvapi_headers:
        src_file = os.path.join(src_dir, header)
        dst_file = os.path.join(dst_dir, header)

        if not os.path.exists(src_file):
            print(f"  ⚠ {header} not found at {src_file}, skipping")
            continue

        try:
            shutil.copy2(src_file, dst_file)
            print(f"  ✓ Copied {header} to SDK/slang/include/")
        except Exception as e:
            print(f"  ✗ Failed to copy {header} to SDK/slang/include/: {e}")


def copy_python_dlls_to_binaries(targets, dry_run=False):
    """Copy entire Python directory contents from SDK/python to Binaries/{target}/ for each target"""
    sdk_python_dir = os.path.join(os.path.dirname(__file__), "SDK", "python")
    if not os.path.exists(sdk_python_dir):
        return

    for target in targets:
        target_dir = os.path.join(os.getcwd(), "Binaries", target)

        if dry_run:
            print(f"  [DRY RUN] Would copy Python directory to Binaries/{target}/")
        else:
            os.makedirs(target_dir, exist_ok=True)

            # Use copytree with dirs_exist_ok to copy entire python directory efficiently
            shutil.copytree(sdk_python_dir, target_dir, dirs_exist_ok=True)

            print(f"  Copied Python directory to Binaries/{target}/")

    if not dry_run and targets:
        print(
            f"  Copied entire Python installation from SDK to Binaries for targets: {targets}"
        )


def copy_cuda_runtime_dlls_to_binaries(targets, dry_run=False):
    """Copy CUDA runtime DLLs to Binaries/{target}/ if available"""
    # Try to find CUDA installation from environment variable
    cuda_path = os.environ.get("CUDA_PATH")

    # If not found, silently skip (CUDA is optional)
    if not cuda_path:
        print("  CUDA_PATH not set, skipping CUDA runtime DLLs")
        return

    # Define the DLLs we need
    cuda_dlls = [
        "cudart64_12.dll",
        "nvrtc64_120_0.dll",
        "cudart64_13.dll",
        "nvrtc64_130_0.dll",
    ]

    # Possible bin directories to search
    bin_dirs = [
        os.path.join(cuda_path, "bin"),
        os.path.join(cuda_path, "bin", "x64"),
    ]

    # Copy each DLL to Binaries/{target}
    for target in targets:
        target_dir = os.path.join(os.getcwd(), "Binaries", target)

        for dll_name in cuda_dlls:
            # Try to find the DLL in any of the bin directories
            src_dll = None
            for bin_dir in bin_dirs:
                potential_path = os.path.join(bin_dir, dll_name)
                if os.path.exists(potential_path):
                    src_dll = potential_path
                    break

            # Skip if DLL doesn't exist in any location
            if not src_dll:
                print(
                    f"  ⚠ {dll_name} not found in {cuda_path}/bin or {cuda_path}/bin/x64, skipping"
                )
                continue

            dst_dll = os.path.join(target_dir, dll_name)

            if dry_run:
                print(f"  [DRY RUN] Would copy {dll_name} to Binaries/{target}/")
            else:
                os.makedirs(target_dir, exist_ok=True)
                try:
                    shutil.copy2(src_dll, dst_dll)
                    file_size_mb = os.path.getsize(dst_dll) / (1024 * 1024)
                    print(
                        f"  ✓ Copied {dll_name} ({file_size_mb:.2f} MB) to Binaries/{target}/"
                    )
                except Exception as e:
                    print(f"  ✗ Failed to copy {dll_name}: {e}")


def download_with_progress(url, zip_path, dry_run=False):
    if dry_run:
        print(f"[DRY RUN] Would download from {url} to {zip_path}")
        return

    # Ensure the directory exists
    os.makedirs(os.path.dirname(zip_path), exist_ok=True)

    response = requests.get(url, stream=True)
    file_size = int(response.headers.get("Content-Length", 0))
    with tqdm(total=file_size, unit="B", unit_scale=True, desc=zip_path) as pbar:
        with open(zip_path, "wb") as file_handle:
            for chunk in response.iter_content(chunk_size=8192):
                if chunk:
                    file_handle.write(chunk)
                    pbar.update(len(chunk))


def download_and_extract(url, extract_path, folder, targets, dry_run=False):
    zip_path = os.path.dirname(__file__) + "/SDK/cache/" + url.split("/")[-1]
    if os.path.exists(zip_path):
        print(f"Using cached file {zip_path}")
    else:
        if not dry_run:
            print(f"Downloading from {url}...")
        download_with_progress(url, zip_path, dry_run)

    if dry_run:
        print(f"[DRY RUN] Would extract {zip_path} to {extract_path}")
        return

    print(f"Extracting to {extract_path}...")
    try:
        with zipfile.ZipFile(zip_path, "r") as zip_ref:
            zip_ref.extractall(extract_path)
        print(f"Downloaded and extracted successfully.")
        for target in targets:
            copytree_common_to_binaries(folder, target=target, dry_run=dry_run)
    except Exception as e:
        print(f"Error extracting {zip_path}: {e}")


openusd_version = "25.05.01"


def process_usd(targets, dry_run=False, keep_original_files=True, copy_only=False):
    if not copy_only:
        # First download and extract the source files
        url = "https://github.com/PixarAnimationStudios/OpenUSD/archive/refs/tags/v{}.zip".format(
            openusd_version
        )

        zip_path = os.path.join(
            os.path.dirname(__file__), "SDK", "cache", url.split("/")[-1]
        )
        if os.path.exists(zip_path):
            print(f"Using cached file {zip_path}")
        else:
            if not dry_run:
                print(f"Downloading from {url}...")
            download_with_progress(url, zip_path, dry_run)

        # Extract the downloaded zip file
        extract_path = os.path.join(
            os.path.dirname(__file__), "SDK", "OpenUSD", "source"
        )
        if keep_original_files and os.path.exists(extract_path):
            print(f"Keeping original files in {extract_path}")
        else:
            if dry_run:
                print(f"[DRY RUN] Would extract {zip_path} to {extract_path}")
            else:
                try:
                    with zipfile.ZipFile(zip_path, "r") as zip_ref:
                        zip_ref.extractall(extract_path)
                    print(f"Downloaded and extracted successfully.")
                except Exception as e:
                    print(f"Error extracting {zip_path}: {e}")
                    return

        # Call the build script with the specified options
        build_script = os.path.join(
            extract_path,
            "OpenUSD-{}".format(openusd_version),
            "build_scripts",
            "build_usd.py",
        )

        # Check if the user has a debug python installed
        import subprocess

        try:
            subprocess.check_output(["python_d", "--version"], stderr=subprocess.STDOUT)
            has_python_d = True
        except subprocess.CalledProcessError:
            has_python_d = False
        except FileNotFoundError:
            has_python_d = False

        if has_python_d:
            use_debug_python = "--debug-python "
        else:
            use_debug_python = ""

        for target in targets:
            build_variant_map = {
                "Debug": "debug",
                "Release": "release",
                "RelWithDebInfo": "relwithdebuginfo",
            }
            build_variant = build_variant_map.get(target, target.lower())
            generator_ninja = "--generator Ninja "

            # if build_variant == "relwithdebuginfo":
            #     openvdb_args = 'OpenVDB,"-DUSE_EXPLICIT_INSTANTIATION=OFF -DOPENVDB_BUILD_NANOVDB=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBUGINFO=Release" '
            # else:

            openvdb_args = (
                'OpenVDB,"-DUSE_EXPLICIT_INSTANTIATION=OFF -DOPENVDB_BUILD_NANOVDB=ON" '
            )
            no_tbb_linkage = "-DCMAKE_CXX_FLAGS=-D__TBB_NO_IMPLICIT_LINKAGE=1"
            openimageio_args = f"OpenImageIO,{no_tbb_linkage} "
            build_command = f'python {build_script} --build-args USD,"-DPXR_ENABLE_GL_SUPPORT=ON" {openvdb_args}{openimageio_args}--openvdb {use_debug_python}--ptex --openimageio --opencolorio --no-examples --no-tutorials {generator_ninja}--build-variant {build_variant} {os.path.dirname(__file__)}/SDK/OpenUSD/{target} -v'

            if dry_run:
                print(f"[DRY RUN] Would run: {build_command}")
            else:
                # Apply FindTBB.cmake patch after building
                if target == "RelWithDebInfo":  # Only patch once for RelWithDebInfo
                    patch_findtbb_cmake(dry_run)

                # Enable long path support for Windows before building
                import subprocess

                try:
                    # Enable Git long path support
                    subprocess.run(
                        ["git", "config", "--global", "core.longpaths", "true"],
                        check=False,
                    )
                    print("Enabled Git long path support")
                except Exception as e:
                    print(f"Warning: Could not enable Git long path support: {e}")

                os.system(build_command)

    # Copy the built binaries to the Binaries folder
    for target in targets:
        copytree_common_to_binaries(
            os.path.join("OpenUSD", target, "bin"), target=target, dry_run=dry_run
        )
        copytree_common_to_binaries(
            os.path.join("OpenUSD", target, "lib"), target=target, dry_run=dry_run
        )
        copytree_common_to_binaries(
            os.path.join("OpenUSD", target, "plugin"), target=target, dry_run=dry_run
        )

        # Copy libraries and resources wholly
        copytree_common_to_binaries(
            os.path.join("OpenUSD", target, "libraries"),
            target=target,
            dst="libraries",
            dry_run=dry_run,
        )
        copytree_common_to_binaries(
            os.path.join("OpenUSD", target, "resources"),
            target=target,
            dst="resources",
            dry_run=dry_run,
        )

        # Copy USD Python bindings (pxr module) directly to Binaries/{target}/
        # This allows Python to import pxr directly when running from Binaries/{target}/
        copytree_common_to_binaries(
            os.path.join("OpenUSD", target, "lib", "python"),
            target=target,
            dst="",  # Copy directly to Binaries/{target}/, not to python/ subdirectory
            dry_run=dry_run,
        )


import concurrent.futures
import subprocess


def extract_and_setup_sdk(sdk_zip_path, targets=None, dry_run=False):
    """
    Extract SDK.zip and copy its contents to Binaries folder for each build type.
    Uses the same copy logic as --copy-only --all.

    Args:
        sdk_zip_path: Path to SDK.zip file (relative to project root)
        targets: List of build targets (Debug, Release). Defaults to both.
        dry_run: If True, print actions without executing them

    Returns:
        True if successful, False otherwise
    """
    if targets is None:
        targets = ["Debug", "Release"]

    project_root = os.path.dirname(__file__)
    sdk_zip = os.path.join(project_root, sdk_zip_path)

    if not os.path.exists(sdk_zip):
        print(f"ERROR: SDK.zip not found at {sdk_zip}")
        return False

    try:
        # Extract SDK.zip to SDK/ folder
        sdk_dir = os.path.join(project_root, "SDK")
        print(f"Extracting {sdk_zip} to {sdk_dir}...")

        if not dry_run:
            os.makedirs(sdk_dir, exist_ok=True)
            with zipfile.ZipFile(sdk_zip, "r") as zip_ref:
                zip_ref.extractall(sdk_dir)
            print("✓ SDK extracted successfully")
        else:
            print(f"[DRY RUN] Would extract {sdk_zip} to {sdk_dir}")

        # Copy SDK content to Binaries using the same logic as --copy-only --all
        print(
            "\nSetting up SDK structure for builds (using --copy-only --all logic)..."
        )

        # Copy OpenUSD components
        for target in targets:
            copytree_common_to_binaries(
                os.path.join("OpenUSD", target, "bin"), target=target, dry_run=dry_run
            )
            copytree_common_to_binaries(
                os.path.join("OpenUSD", target, "lib"), target=target, dry_run=dry_run
            )
            copytree_common_to_binaries(
                os.path.join("OpenUSD", target, "plugin"),
                target=target,
                dry_run=dry_run,
            )
            copytree_common_to_binaries(
                os.path.join("OpenUSD", target, "libraries"),
                target=target,
                dst="libraries",
                dry_run=dry_run,
            )
            copytree_common_to_binaries(
                os.path.join("OpenUSD", target, "resources"),
                target=target,
                dst="resources",
                dry_run=dry_run,
            )

            # Copy USD Python bindings (pxr module) directly to Binaries/{target}/
            copytree_common_to_binaries(
                os.path.join("OpenUSD", target, "lib", "python"),
                target=target,
                dst="",  # Copy directly to Binaries/{target}/, not to python/ subdirectory
                dry_run=dry_run,
            )

        # Copy Slang
        folders = {
            "slang": "slang/bin",
            "d3d12": "d3d12/bin",
            "dxc": "dxc/bin/x64",
            "embree": "embree/bin",
        }
        for target in targets:
            copytree_common_to_binaries(
                folders["slang"], target=target, dry_run=dry_run
            )

        # Copy D3D12 (Windows only)
        if os.name == "nt":
            for target in targets:
                copytree_common_to_binaries(
                    folders["d3d12"], target=target, dry_run=dry_run
                )

        # Copy DXC
        for target in targets:
            copytree_common_to_binaries(folders["dxc"], target=target, dry_run=dry_run)

        # Copy Embree
        for target in targets:
            copytree_common_to_binaries(
                folders["embree"], target=target, dry_run=dry_run
            )

        # Copy Python DLLs
        copy_python_dlls_to_binaries(targets, dry_run=dry_run)

        # Copy CUDA runtime DLLs if available
        copy_cuda_runtime_dlls_to_binaries(targets, dry_run=dry_run)

        # Copy imgui.ini to Binaries
        copy_imgui_ini_to_binaries(targets, dry_run=dry_run)

        # Copy nvHLSLExtns.h to SDK/slang/include/
        copy_nvapi_header_to_slang(dry_run=dry_run)

        print("✓ SDK structure setup complete")
        return True

    except Exception as e:
        print(f"ERROR: Failed to extract/setup SDK: {e}")
        return False


def pack_sdk(dry_run=False):
    src_dir = os.path.join(os.path.dirname(__file__), "SDK")
    dst_dir = os.path.join(os.path.dirname(__file__), "SDK\\SDK_pack_temp")

    # Path that need to be replaced
    import sys
    where_python = (
        subprocess.check_output(["where", "python"]).decode("utf-8").split("\n")[0]
    )
    where_python_dir = sys.base_prefix  # This should give us the root directory of the Python installation
    
    python_dir_backward_slash = where_python_dir.replace("/", "\\")
    python_dir_forward_slash = python_dir_backward_slash.replace("\\", "/")
    framework3d_dir_backward_slash = os.getcwd().replace("/", "\\")
    framework3d_dir_forward_slash = framework3d_dir_backward_slash.replace("\\", "/")

    # Define replacements for GridBuilder.h
    gridbuilder_replacements = {"std::result_of": "std::invoke_result_t"}

    def copy_file(src_file, dst_file):
        if dry_run:
            print(f"[DRY RUN] Would copy {src_file} to {dst_file}")
        else:
            shutil.copy2(src_file, dst_file)
            try:
                with open(dst_file, "r", encoding="utf-8") as file:
                    filedata = file.read()
            except (UnicodeDecodeError, IOError) as e:
                return
            filedata_0 = filedata
            filedata = filedata.replace(
                python_dir_backward_slash, "${Python3_ROOT_DIR}"
            )
            filedata = filedata.replace(python_dir_forward_slash, "${Python3_ROOT_DIR}")
            filedata = filedata.replace(
                framework3d_dir_backward_slash, "${FRAMEWORK3D_DIR}"
            )
            filedata = filedata.replace(
                framework3d_dir_forward_slash, "${FRAMEWORK3D_DIR}"
            )

            # Remove brackets around paths containing placeholders
            import re

            # Pattern to match [[${FRAMEWORK3D_DIR}/...]] or [[${Python3_ROOT_DIR}/...]]
            bracket_pattern = r"\[\[(.*?)\]\]"
            matches = re.findall(bracket_pattern, filedata)
            for match in matches:
                if "${FRAMEWORK3D_DIR}" in match or "${Python3_ROOT_DIR}" in match:
                    # Normalize path separators to forward slashes
                    normalized_match = match.replace("\\", "/")
                    filedata = filedata.replace(f"[[{match}]]", normalized_match)

            # Also normalize any remaining paths with placeholders that have backslashes
            filedata = re.sub(
                r"(\$\{(?:FRAMEWORK3D_DIR|Python3_ROOT_DIR)\}[^;\s\]]*)",
                lambda m: m.group(1).replace("\\", "/"),
                filedata,
            )

            # Handle GridBuilder.h replacements
            # Only replace in the specific path: SDK\OpenUSD\<variant>\include\nanovdb\util\GridBuilder.h
            if (
                "GridBuilder.h" in dst_file
                and "include" in dst_file
                and "nanovdb" in dst_file
                and "util" in dst_file
                and "OpenUSD" in dst_file
            ):
                for old_text, new_text in gridbuilder_replacements.items():
                    if old_text in filedata:
                        filedata = filedata.replace(old_text, new_text)
                        print(f"Replaced '{old_text}' with '{new_text}' in {dst_file}")

            if filedata != filedata_0:
                with open(dst_file, "w", encoding="utf-8") as file:
                    file.write(filedata)
                    print(f"Found and replaced path in {dst_file}")

    def copy_python_installation(python_dir, dst_python_dir):
        """Copy essential Python installation files"""
        if dry_run:
            print(
                f"[DRY RUN] Would copy Python installation from {python_dir} to {dst_python_dir}"
            )
            return

        print(f"Copying Python installation from {python_dir} to {dst_python_dir}")
        os.makedirs(dst_python_dir, exist_ok=True)

        # Copy python.exe and python_d.exe if exists
        for exe_name in ["python.exe", "python_d.exe", "pythonw.exe"]:
            exe_path = os.path.join(python_dir, exe_name)
            if os.path.exists(exe_path):
                shutil.copy2(exe_path, dst_python_dir)

        # Copy all DLLs in python directory
        for file in os.listdir(python_dir):
            if file.endswith(".dll"):
                shutil.copy2(os.path.join(python_dir, file), dst_python_dir)

        # Copy DLLs directory if exists
        dlls_dir = os.path.join(python_dir, "DLLs")
        if os.path.exists(dlls_dir):
            dst_dlls_dir = os.path.join(dst_python_dir, "DLLs")
            shutil.copytree(dlls_dir, dst_dlls_dir, dirs_exist_ok=True)

        # Copy libs directory (contains python3.lib and other static libraries)
        libs_dir = os.path.join(python_dir, "libs")
        if os.path.exists(libs_dir):
            dst_libs_dir = os.path.join(dst_python_dir, "libs")
            shutil.copytree(libs_dir, dst_libs_dir, dirs_exist_ok=True)
            print(f"Copied libs directory (including python3.lib)")

        # Copy Scripts directory (contains pip and other tools)
        scripts_dir = os.path.join(python_dir, "Scripts")
        if os.path.exists(scripts_dir):
            dst_scripts_dir = os.path.join(dst_python_dir, "Scripts")
            shutil.copytree(scripts_dir, dst_scripts_dir, dirs_exist_ok=True)
            print(f"Copied Scripts directory (including pip)")

        # Copy Lib directory but exclude site-packages and other third-party packages
        lib_dir = os.path.join(python_dir, "Lib")
        if os.path.exists(lib_dir):
            dst_lib_dir = os.path.join(dst_python_dir, "Lib")
            os.makedirs(dst_lib_dir, exist_ok=True)

            # Standard library directories/files to include
            standard_lib_items = []
            exclude_dirs = {"site-packages", "dist-packages", "__pycache__"}

            for item in os.listdir(lib_dir):
                item_path = os.path.join(lib_dir, item)
                if os.path.isdir(item_path):
                    if item not in exclude_dirs:
                        standard_lib_items.append(item)
                else:
                    # Include .py files in root Lib directory
                    if item.endswith(".py"):
                        standard_lib_items.append(item)

            # Copy standard library items
            for item in standard_lib_items:
                src_item = os.path.join(lib_dir, item)
                dst_item = os.path.join(dst_lib_dir, item)
                if os.path.isdir(src_item):
                    shutil.copytree(src_item, dst_item, dirs_exist_ok=True)
                else:
                    shutil.copy2(src_item, dst_item)

        # Copy Include directory if exists
        include_dir = os.path.join(python_dir, "include")
        if os.path.exists(include_dir):
            dst_include_dir = os.path.join(dst_python_dir, "include")
            shutil.copytree(include_dir, dst_include_dir, dirs_exist_ok=True)

        print(f"Python installation copied successfully")

    with concurrent.futures.ThreadPoolExecutor() as executor:
        futures = []
        for root, dirs, files in os.walk(src_dir):
            # Skip build, cache directories and anything under */src/
            if any(
                skip_dir in root
                for skip_dir in ["\\build", "\\cache", "\\src", "\\source"]
            ):
                continue

            # Create corresponding directory in destination
            relative_path = os.path.relpath(root, src_dir)
            dst_path = os.path.join(dst_dir, relative_path)
            if not dry_run:
                os.makedirs(dst_path, exist_ok=True)

            for file in files:
                if file.endswith(".pdb") or file == "libopenvdb.lib":
                    print(f"Skipping {os.path.join(root, file)}")
                    continue

                src_file = os.path.join(root, file)
                dst_file = os.path.join(dst_path, file)
                futures.append(executor.submit(copy_file, src_file, dst_file))

        # Wait for all threads to complete
        concurrent.futures.wait(futures)

        # Copy Python installation
        python_dst_dir = os.path.join(dst_dir, "python")
        copy_python_installation(python_dir_backward_slash, python_dst_dir)

        # Pack the SDK_temp directory into SDK.zip
        if dry_run:
            print(f"[DRY RUN] Would pack {dst_dir} into SDK.zip")
        else:
            shutil.make_archive("SDK\\SDK", "zip", dst_dir)
            print(f"Packed {dst_dir} into SDK.zip")

        # Delete the SDK_temp directory with retry logic
        if dry_run:
            print(f"[DRY RUN] Would delete {dst_dir}")
        else:
            import time

            max_retries = 5
            retry_count = 0
            while retry_count < max_retries:
                try:

                    def remove_readonly(func, path, exc):
                        """Error handler for shutil.rmtree to handle read-only files"""
                        import stat

                        if not os.access(path, os.W_OK):
                            os.chmod(path, stat.S_IWUSR | stat.S_IREAD)
                            func(path)
                        else:
                            raise

                    shutil.rmtree(dst_dir, onerror=remove_readonly)
                    print(f"Deleted {dst_dir}")
                    break
                except Exception as e:
                    retry_count += 1
                    if retry_count < max_retries:
                        print(
                            f"Warning: Failed to delete {dst_dir}, retrying ({retry_count}/{max_retries})..."
                        )
                        time.sleep(1)
                    else:
                        print(
                            f"Error: Failed to delete {dst_dir} after {max_retries} retries: {e}"
                        )
                        print(
                            f"SDK.zip has been created successfully, but temporary directory could not be cleaned up."
                        )


def find_and_replace(file_path, replacements):
    """处理单个文件的替换操作"""
    try:
        with open(file_path, "r", encoding="utf-8") as file:
            filedata = file.read()

        filedata_0 = filedata
        for old_text, new_text in replacements.items():
            filedata = filedata.replace(old_text, new_text)

        if filedata != filedata_0:
            with open(file_path, "w", encoding="utf-8") as file:
                file.write(filedata)
                print(f"Found and replaced path in {file_path}")
    except (UnicodeDecodeError, IOError) as e:
        return


def patch_findtbb_cmake(dry_run=False):
    """Patch FindTBB.cmake to work better with single target configuration generators"""
    findtbb_path = os.path.join(
        os.path.dirname(__file__),
        "SDK",
        "OpenUSD",
        "RelWithDebInfo",
        "src",
        "openvdb-9.1.0",
        "cmake",
        "FindTBB.cmake",
    )

    if not os.path.exists(findtbb_path):
        print(f"FindTBB.cmake not found at {findtbb_path}, skipping patch")
        return

    # The original problematic code block
    old_code = """  if(Tbb_${COMPONENT}_LIBRARY_DEBUG AND Tbb_${COMPONENT}_LIBRARY_RELEASE)
    # if the generator is multi-config or if CMAKE_BUILD_TYPE is set for
    # single-config generators, set optimized and debug libraries
    get_property(_isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_isMultiConfig OR CMAKE_BUILD_TYPE)
      set(Tbb_${COMPONENT}_LIBRARY optimized ${Tbb_${COMPONENT}_LIBRARY_RELEASE} debug ${Tbb_${COMPONENT}_LIBRARY_DEBUG})
    else()
      # For single-config generators where CMAKE_BUILD_TYPE has no value,
      # just use the release libraries
      set(Tbb_${COMPONENT}_LIBRARY ${Tbb_${COMPONENT}_LIBRARY_RELEASE})
    endif()
    # FIXME: This probably should be set for both cases
    set(Tbb_${COMPONENT}_LIBRARIES optimized ${Tbb_${COMPONENT}_LIBRARY_RELEASE} debug ${Tbb_${COMPONENT}_LIBRARY_DEBUG})
  endif()"""

    # New code that works better with single target generators
    new_code = """  if(Tbb_${COMPONENT}_LIBRARY_DEBUG AND Tbb_${COMPONENT}_LIBRARY_RELEASE)
    # Check if we're using a multi-config generator or if CMAKE_BUILD_TYPE is set
    get_property(_isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_isMultiConfig)
      # Multi-config generator: use optimized/debug keywords
      set(Tbb_${COMPONENT}_LIBRARY optimized ${Tbb_${COMPONENT}_LIBRARY_RELEASE} debug ${Tbb_${COMPONENT}_LIBRARY_DEBUG})
      set(Tbb_${COMPONENT}_LIBRARIES optimized ${Tbb_${COMPONENT}_LIBRARY_RELEASE} debug ${Tbb_${COMPONENT}_LIBRARY_DEBUG})
    elseif(CMAKE_BUILD_TYPE)
      # Single-config generator with CMAKE_BUILD_TYPE set
      if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(Tbb_${COMPONENT}_LIBRARY ${Tbb_${COMPONENT}_LIBRARY_DEBUG})
        set(Tbb_${COMPONENT}_LIBRARIES ${Tbb_${COMPONENT}_LIBRARY_DEBUG})
      else()
        set(Tbb_${COMPONENT}_LIBRARY ${Tbb_${COMPONENT}_LIBRARY_RELEASE})
        set(Tbb_${COMPONENT}_LIBRARIES ${Tbb_${COMPONENT}_LIBRARY_RELEASE})
      endif()
    else()
      # Single-config generator without CMAKE_BUILD_TYPE: default to release but provide both options
      set(Tbb_${COMPONENT}_LIBRARY ${Tbb_${COMPONENT}_LIBRARY_RELEASE})
      set(Tbb_${COMPONENT}_LIBRARIES optimized ${Tbb_${COMPONENT}_LIBRARY_RELEASE} debug ${Tbb_${COMPONENT}_LIBRARY_DEBUG})
    endif()
  endif()"""

    if dry_run:
        print(f"[DRY RUN] Would patch FindTBB.cmake at {findtbb_path}")
        return

    try:
        with open(findtbb_path, "r", encoding="utf-8") as file:
            content = file.read()

        if old_code in content:
            content = content.replace(old_code, new_code)

            with open(findtbb_path, "w", encoding="utf-8") as file:
                file.write(content)

            print(
                f"Successfully patched FindTBB.cmake for single target configuration generators"
            )
        else:
            print(
                f"FindTBB.cmake patch target not found - file may already be patched or have different content"
            )

    except Exception as e:
        print(f"Error patching FindTBB.cmake: {e}")


def main():
    parser = argparse.ArgumentParser(description="Download and configure libraries.")
    parser.add_argument(
        "--build_variant", nargs="*", default=["Debug"], help="Specify build variants."
    )
    parser.add_argument(
        "--library",
        choices=["slang", "openusd", "d3d12", "dxc", "embree"],
        help="Specify the library to configure.",
    )
    parser.add_argument("--all", action="store_true", help="Configure all libraries.")
    parser.add_argument(
        "--dry-run",
        "-n",
        action="store_true",
        help="Print actions without executing them.",
    )
    parser.add_argument(
        "--keep-original-files",
        type=bool,
        default=True,
        help="Keep original files if the extract path exists.",
    )
    parser.add_argument(
        "--copy-only",
        action="store_true",
        help="Only copy files, skip downloading and building.",
    )
    parser.add_argument(
        "--pack",
        action="store_true",
        help="Pack SDK files to SDK_temp, skipping pdb files and build/cache directories.",
    )
    parser.add_argument(
        "--extract-sdk",
        type=str,
        metavar="SDK_ZIP_PATH",
        help="Extract SDK.zip and setup structure for builds (e.g., --extract-sdk SDK/SDK.zip)",
    )
    args = parser.parse_args()

    targets = args.build_variant
    dry_run = args.dry_run
    keep_original_files = args.keep_original_files
    copy_only = args.copy_only

    if args.pack:
        pack_sdk(dry_run)
        return

    if args.extract_sdk:
        # Extract and setup SDK from zip file
        success = extract_and_setup_sdk(
            args.extract_sdk, targets=targets, dry_run=dry_run
        )
        if success:
            print("\n✓ SDK ready for building")
            return
        else:
            print("\n✗ Failed to extract SDK")
            exit(1)

    if args.all:
        args.library = ["openusd", "slang", "d3d12", "dxc", "embree"]
    elif not args.library:
        print(
            "No library specified and --all not set. No libraries will be configured."
        )
        return
    else:
        args.library = [args.library]

    if dry_run:
        print(f"[DRY RUN] Selected build variants: {targets}")

    if os.name == "nt":
        urls = {
            "slang": "https://github.com/shader-slang/slang/releases/download/v2025.22.1/slang-2025.22.1-windows-x86_64.zip",
            "d3d12": "https://globalcdn.nuget.org/packages/microsoft.direct3d.d3d12.1.616.1.nupkg",
            "dxc": "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2505.1/dxc_2025_07_14.zip",
            "embree": "https://github.com/RenderKit/embree/releases/download/v4.4.0/embree-4.4.0.x64.windows.zip",
        }
    elif os.name == "posix":
        urls = {
            "slang": "https://github.com/shader-slang/slang/releases/download/v2025.22.1/slang-2025.22.1-macos-x86_64.zip",
            "dxc": "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2505.1/dxc_2025_07_14.zip",
            "embree": "https://github.com/RenderKit/embree/releases/download/v4.4.0/embree-4.4.0.x64.windows.zip",
        }
    else:
        urls = {
            "slang": "https://github.com/shader-slang/slang/releases/download/v2025.22.1/slang-2025.22.1-linux-x86_64.zip",
            "dxc": "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2505.1/dxc_2025_07_14.zip",
            "embree": "https://github.com/RenderKit/embree/releases/download/v4.4.0/embree-4.4.0.x64.windows.zip",
        }
    folders = {
        "slang": "slang/bin",
        "d3d12": "d3d12/bin",
        "dxc": "dxc/bin/x64",
        "embree": "embree/bin",
    }

    for lib in args.library:
        if lib == "openusd":
            process_usd(targets, dry_run, keep_original_files, copy_only)
        elif lib == "d3d12" and os.name == "nt":
            if not copy_only:
                # Download the nupkg file
                nupkg_path = os.path.dirname(__file__) + "/SDK/cache/d3d12.nupkg"
                download_with_progress(urls[lib], nupkg_path, dry_run)

                # Rename to zip and extract
                zip_path = nupkg_path.replace(".nupkg", ".zip")

                if dry_run:
                    print(f"[DRY RUN] Would rename {nupkg_path} to {zip_path}")
                else:
                    if os.path.exists(nupkg_path):
                        shutil.copy2(nupkg_path, zip_path)
                        print(f"Renamed {nupkg_path} to {zip_path}")

                # Extract the zip file
                extract_path = os.path.dirname(__file__) + "/SDK/d3d12"
                if dry_run:
                    print(f"[DRY RUN] Would extract {zip_path} to {extract_path}")
                else:
                    try:
                        with zipfile.ZipFile(zip_path, "r") as zip_ref:
                            zip_ref.extractall(extract_path)
                        print(f"Downloaded and extracted successfully.")

                        # Create bin directory and move necessary files
                        bin_dir = os.path.join(extract_path, "bin")
                        os.makedirs(bin_dir, exist_ok=True)

                        # Move relevant DLLs from extracted structure to bin folder
                        agility_path = os.path.join(
                            extract_path, "build", "native", "bin", "x64"
                        )
                        if os.path.exists(agility_path):
                            for file in os.listdir(agility_path):
                                if file.endswith(".dll") or file.endswith(".pdb"):
                                    shutil.copy2(
                                        os.path.join(agility_path, file), bin_dir
                                    )

                        print(f"D3D12 Agility SDK files prepared in {bin_dir}")
                    except Exception as e:
                        print(f"Error extracting {zip_path}: {e}")

            # Copy the D3D12 files to the binaries folder
            for target in targets:
                copytree_common_to_binaries(
                    folders[lib], target=target, dry_run=dry_run
                )
        elif lib == "dxc":
            if not copy_only:
                # Download and extract DXC
                extract_path = os.path.dirname(__file__) + "/SDK/dxc"
                zip_path = os.path.dirname(__file__) + "/SDK/cache/dxc.zip"
                download_with_progress(urls[lib], zip_path, dry_run)

                if dry_run:
                    print(f"[DRY RUN] Would extract {zip_path} to {extract_path}")
                else:
                    try:
                        # Ensure bin directory exists
                        bin_dir = os.path.join(extract_path, "bin")
                        os.makedirs(bin_dir, exist_ok=True)

                        # Extract DXC files
                        with zipfile.ZipFile(zip_path, "r") as zip_ref:
                            zip_ref.extractall(extract_path)
                        print(f"Downloaded and extracted DXC successfully.")

                        # Find and move binaries to bin directory
                        for root, _, files in os.walk(extract_path):
                            for file in files:
                                if (
                                    file.endswith(".exe")
                                    or file.endswith(".dll")
                                    or file.endswith(".lib")
                                ):
                                    src_file = os.path.join(root, file)
                                    dst_file = os.path.join(bin_dir, file)
                                    if src_file != dst_file:
                                        shutil.copy2(src_file, bin_dir)

                        print(f"DXC files prepared in {bin_dir}")
                    except Exception as e:
                        print(
                            f"Error extracting DXC: {e}"
                        )  # Copy the DXC files to the binaries folder
            for target in targets:
                copytree_common_to_binaries(
                    folders[lib], target=target, dry_run=dry_run
                )
        elif lib == "embree":
            if not copy_only:
                download_and_extract(
                    urls[lib],
                    os.path.dirname(__file__) + "/SDK/embree",
                    folders[lib],
                    targets,
                    dry_run,
                )
            # Copy the embree bin files to the binaries folder
            for target in targets:
                copytree_common_to_binaries(
                    folders[lib], target=target, dry_run=dry_run
                )
        else:
            if not copy_only:
                download_and_extract(
                    urls[lib],
                    os.path.dirname(__file__) + f"/SDK/{lib}",
                    folders[lib],
                    targets,
                    dry_run,
                )
            else:
                for target in targets:
                    copytree_common_to_binaries(
                        folders[lib], target=target, dry_run=dry_run
                    )

    # Copy Python DLLs from SDK to Binaries for each target in copy-only mode
    if copy_only:
        copy_python_dlls_to_binaries(targets, dry_run=dry_run)
        # Also copy CUDA runtime DLLs if available
        copy_cuda_runtime_dlls_to_binaries(targets, dry_run=dry_run)

    # Always copy imgui.ini to Binaries
    copy_imgui_ini_to_binaries(targets, dry_run=dry_run)

    # Always copy nvHLSLExtns.h to SDK/slang/include/
    copy_nvapi_header_to_slang(dry_run=dry_run)


if __name__ == "__main__":
    main()
