import zipfile
import shutil
import glob as glob_mod
import os
import sys
import platform
import stat
import subprocess
import tarfile
import requests
from tqdm import tqdm
import argparse


def get_platform():
    """
    Returns the current platform as a string: 'windows', 'linux', or 'macos'
    """
    system = platform.system().lower()
    if system == 'windows':
        return 'windows'
    elif system == 'linux':
        return 'linux'
    elif system == 'darwin':
        return 'macos'
    else:
        return system


def get_binary_extension():
    """
    Returns the appropriate binary extension for the current platform.
    Windows: .dll, Linux: .so, macOS: .dylib
    """
    plat = get_platform()
    if plat == 'windows':
        return '.dll'
    elif plat == 'linux':
        return '.so'
    elif plat == 'macos':
        return '.dylib'
    return ''


def get_executable_extension():
    """
    Returns the appropriate executable extension for the current platform.
    Windows: .exe, Linux/macOS: '' (no extension)
    """
    return '.exe' if get_platform() == 'windows' else ''


def is_windows():
    return get_platform() == 'windows'


def is_linux():
    return get_platform() == 'linux'


def is_macos():
    return get_platform() == 'macos'


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
                # Skip static/import libraries (not needed at runtime)
                # Windows: .lib (import libraries), Linux/macOS: .a (static archives)
                if file.endswith(".lib") or file.endswith(".a"):
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


def _python_include_dir(python_dir):
    """Return the CPython headers directory, handling the Windows 'Include'
    (capital I) convention as well as the POSIX 'include' spelling."""
    for name in ("include", "Include"):
        candidate = os.path.join(python_dir, name)
        if os.path.isdir(candidate):
            return candidate
    return os.path.join(python_dir, "include")


def _python_install_is_complete(python_dir):
    """Check that a Python installation has the dev artifacts needed to build
    C/C++ extensions against it: the interpreter executable, the headers
    (including pyconfig.h) and the link library directory.

    A bare python.exe with no headers/libs is a common partial-install state
    (e.g. the embeddable distribution or a partially copied SDK/python) that
    silently breaks any target that #includes Python's C API.
    """
    exe_names = (
        ["python.exe", "pythonw.exe"] if is_windows()
        else ["python3", "python"]
    )
    has_exe = any(os.path.isfile(os.path.join(python_dir, n)) for n in exe_names)
    inc_dir = _python_include_dir(python_dir)
    has_headers = os.path.isfile(os.path.join(inc_dir, "pyconfig.h"))
    libs_dir = os.path.join(python_dir, "libs") if is_windows() else python_dir
    lib_ext = ".lib" if is_windows() else (".so" if is_linux() else ".dylib")
    has_lib = any(
        f.endswith(lib_ext) for f in os.listdir(libs_dir)
    ) if os.path.isdir(libs_dir) else False
    return has_exe and has_headers and has_lib


def _is_virtualenv(python_dir):
    """True if python_dir looks like a venv (has a pyvenv.cfg marker), which
    lacks its own Include/libs and must not be used as a copy source."""
    return os.path.isfile(os.path.join(python_dir, "pyvenv.cfg"))


def _find_system_python():
    """Locate a real, complete Python installation suitable for copying into
    SDK/python.

    Order of preference:
      1. Windows: the 'py' launcher (--list / -3.13), which resolves
         to base installs and is not fooled by an active virtualenv on PATH.
      2. 'python3' / 'python' on PATH, but only if the resolved dir is a real
         install (not a venv) AND passes the completeness check.
      3. Common well-known install locations as a last resort.

    Returns the directory of a complete Python installation, or None.
    """
    candidates = []

    # 1. Windows py launcher — most reliable, skips venvs.
    # 3.13 first: the project standardizes on 3.13 for both the app pyd
    # extensions and OpenUSD. Older versions are listed only as fallback.
    if is_windows():
        py_launcher = shutil.which("py")
        if py_launcher:
            for ver in ("3.13", "3.12", "3.11"):
                try:
                    exe = subprocess.check_output(
                        [py_launcher, f"-{ver}", "-c",
                         "import sys; print(sys.executable)"],
                        text=True, stderr=subprocess.DEVNULL,
                    ).strip()
                except (subprocess.CalledProcessError, FileNotFoundError):
                    continue
                if exe:
                    candidates.append(os.path.dirname(exe))

    # 1b. Scoop shimmed installs — preferred over a bare PATH lookup because
    # scoop apps/pythonXYZ/current resolves to a fixed base install (not a
    # venv) and the reverse-sorted listing naturally prefers the newest
    # Python (313 > 312 > 311). The PATH lookup below can otherwise latch
    # onto an older python that happens to sort earlier on PATH.
    if is_windows():
        scoop = os.environ.get("SCOOP", r"C:\Users\%USERNAME%\scoop")
        scoop_apps = os.path.join(os.path.expandvars(scoop), "apps")
        if os.path.isdir(scoop_apps):
            for name in sorted(os.listdir(scoop_apps), reverse=True):
                if name.lower().startswith("python"):
                    candidates.append(os.path.join(scoop_apps, name, "current"))

    # 2. PATH lookup — may resolve to a venv, filtered below.
    for cmd in ("python3", "python"):
        exe = shutil.which(cmd)
        if exe:
            candidates.append(os.path.dirname(os.path.abspath(exe)))

    # 3. Well-known base install roots (Windows).
    if is_windows():
        program_files = os.environ.get("ProgramFiles", r"C:\Program Files")
        local_appdata = os.environ.get("LocalAppData", "")
        roots = [
            os.path.join(local_appdata, "Programs", "Python"),
            os.path.join(program_files, "Python313"),
            os.path.join(program_files, "Python312"),
            os.path.join(program_files, "Python311"),
        ]
        for root in roots:
            if os.path.isdir(root):
                for name in sorted(os.listdir(root), reverse=True):
                    candidates.append(os.path.join(root, name))

    # Deduplicate while preserving order, then keep the first real + complete one.
    seen = set()
    for cand in candidates:
        cand_norm = os.path.normcase(os.path.normpath(cand))
        if cand_norm in seen:
            continue
        seen.add(cand_norm)
        if _is_virtualenv(cand):
            continue
        if _python_install_is_complete(cand):
            return cand
    return None


def _copy_python_installation(python_dir, dst_python_dir, dry_run=False):
    """Copy essential Python installation files from system Python to SDK/python."""
    if dry_run:
        print(
            f"[DRY RUN] Would copy Python installation from {python_dir} to {dst_python_dir}"
        )
        return

    print(f"Copying Python installation from {python_dir} to {dst_python_dir}")
    os.makedirs(dst_python_dir, exist_ok=True)

    # Copy Python executables (platform-specific names)
    if is_windows():
        exe_names = ["python.exe", "python_d.exe", "pythonw.exe"]
    else:
        exe_names = ["python3", "python", "python3.d"]

    for exe_name in exe_names:
        exe_path = os.path.join(python_dir, exe_name)
        if os.path.exists(exe_path):
            shutil.copy2(exe_path, dst_python_dir)

    # Copy shared libraries in python directory (platform-specific extensions)
    lib_extension = get_binary_extension()

    for file in os.listdir(python_dir):
        if file.endswith(lib_extension) or (is_linux() and ".so" in file):
            shutil.copy2(os.path.join(python_dir, file), dst_python_dir)

    # Copy DLLs/lib directory if exists (Windows: DLLs, Linux: lib-dynload)
    dynload_dirs = ["DLLs", "lib-dynload", "lib/python*/lib-dynload"]
    for dynload_name in dynload_dirs:
        dynload_dir = os.path.join(python_dir, dynload_name)
        if os.path.exists(dynload_dir):
            dst_dynload_dir = os.path.join(dst_python_dir, dynload_name)
            shutil.copytree(dynload_dir, dst_dynload_dir, dirs_exist_ok=True)

    # Copy libs directory (contains python3.lib/python3.a and other static libraries)
    libs_dir = os.path.join(python_dir, "libs")
    if os.path.exists(libs_dir):
        dst_libs_dir = os.path.join(dst_python_dir, "libs")
        shutil.copytree(libs_dir, dst_libs_dir, dirs_exist_ok=True)
        print(f"Copied libs directory")

    # Copy Scripts/bin directory (contains pip and other tools)
    scripts_names = ["Scripts", "bin"]
    for scripts_name in scripts_names:
        scripts_dir = os.path.join(python_dir, scripts_name)
        if os.path.exists(scripts_dir):
            dst_scripts_dir = os.path.join(dst_python_dir, scripts_name)
            shutil.copytree(scripts_dir, dst_scripts_dir, dirs_exist_ok=True)
            print(f"Copied {scripts_name} directory (including pip)")

    # Copy Lib directory but exclude site-packages and other third-party packages
    lib_dir = os.path.join(python_dir, "Lib")
    if os.path.exists(lib_dir):
        dst_lib_dir = os.path.join(dst_python_dir, "Lib")
        os.makedirs(dst_lib_dir, exist_ok=True)

        standard_lib_items = []
        exclude_dirs = {"site-packages", "dist-packages", "__pycache__"}

        for item in os.listdir(lib_dir):
            item_path = os.path.join(lib_dir, item)
            if os.path.isdir(item_path):
                if item not in exclude_dirs:
                    standard_lib_items.append(item)
            else:
                if item.endswith(".py"):
                    standard_lib_items.append(item)

        for item in standard_lib_items:
            src_item = os.path.join(lib_dir, item)
            dst_item = os.path.join(dst_lib_dir, item)
            if os.path.isdir(src_item):
                shutil.copytree(src_item, dst_item, dirs_exist_ok=True)
            else:
                shutil.copy2(src_item, dst_item)

    # Copy Include directory if exists (Windows CPython uses 'Include',
    # POSIX uses 'include' — _python_include_dir handles both)
    include_dir = _python_include_dir(python_dir)
    if os.path.exists(include_dir):
        dst_include_dir = os.path.join(dst_python_dir, "include")
        shutil.copytree(include_dir, dst_include_dir, dirs_exist_ok=True)
        print(f"Copied include directory")

    print(f"Python installation copied successfully")


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
    """
    Copy CUDA runtime libraries to Binaries/{target}/ if available.
    Supports both Windows (.dll) and Linux (.so) platforms.
    """
    cuda_path = os.environ.get("CUDA_PATH")
    if not cuda_path and is_linux():
        cuda_path = os.environ.get("CUDA_HOME")

    if not cuda_path:
        env_var = "CUDA_PATH" if is_windows() else "CUDA_PATH/CUDA_HOME"
        print(f"  {env_var} not set, skipping CUDA runtime libraries")
        return

    # Define platform-specific library names
    if is_windows():
        cuda_libs = [
            "cudart64_12.dll",
            "nvrtc64_120_0.dll",
            "cudart64_13.dll",
            "nvrtc64_130_0.dll",
            # nvrtc-builtins ships as _130 on CUDA 12.x/13.0 and _133 on 13.3;
            # list both so the JIT backend runtime resolves on either install.
            "nvrtc-builtins64_130.dll",
            "nvrtc-builtins64_133.dll",
            # Math/runtime libs transitively required by solver & GPU plugins
            # (e.g. RZSolver.dll imports cublas64_13 -> cublasLt64_13, and
            # cusparse64_12). Without these, LoadLibrary of node_set_value/
            # GPU_sph fails with a misleading "Failed to load library".
            "cublas64_13.dll",
            "cublasLt64_13.dll",
            "cusparse64_12.dll",
        ]
        lib_dirs = [
            os.path.join(cuda_path, "bin"),
            os.path.join(cuda_path, "bin", "x64"),
        ]
    elif is_linux():
        cuda_libs = [
            "libcudart.so.12",
            "libnvrtc.so.12",
            "libcudart.so.13",
            "libnvrtc.so.13",
        ]
        lib_dirs = [
            os.path.join(cuda_path, "lib64"),
            os.path.join(cuda_path, "lib"),
            "/usr/local/cuda/lib64",
            "/usr/lib/x86_64-linux-gnu",
        ]
    elif is_macos():
        cuda_libs = [
            "libcudart.12.dylib",
            "libnvrtc.12.dylib",
            "libcudart.13.dylib",
            "libnvrtc.13.dylib",
        ]
        lib_dirs = [
            os.path.join(cuda_path, "lib"),
            "/usr/local/cuda/lib",
        ]
    else:
        print(f"  Unsupported platform for CUDA, skipping")
        return

    for target in targets:
        target_dir = os.path.join(os.getcwd(), "Binaries", target)

        for lib_name in cuda_libs:
            src_lib = None
            for lib_dir in lib_dirs:
                potential_path = os.path.join(lib_dir, lib_name)
                if os.path.exists(potential_path):
                    src_lib = potential_path
                    break

            if not src_lib:
                search_paths = ", ".join(lib_dirs)
                print(f"  ⚠ {lib_name} not found in {search_paths}, skipping")
                continue

            dst_lib = os.path.join(target_dir, lib_name)


            if dry_run:
                print(f"  [DRY RUN] Would copy {lib_name} to Binaries/{target}/")
            else:
                os.makedirs(target_dir, exist_ok=True)
                try:
                    shutil.copy2(src_lib, dst_lib)
                    file_size_mb = os.path.getsize(dst_lib) / (1024 * 1024)
                    print(f"  ✓ Copied {lib_name} ({file_size_mb:.2f} MB) to Binaries/{target}/")
                except Exception as e:
                    print(f"  ✗ Failed to copy {lib_name}: {e}")


def _copy_runtime_bin_dlls_to_binaries(targets, dry_run=False):
    """Always copy prebuilt third-party runtime DLLs (slang/dxc/embree/d3d12)
    into Binaries/{target}/.

    These come from upstream prebuilt archives (no compile step), so the copy
    is fast and idempotent. Running it unconditionally — independent of which
    ``--library`` was selected — ensures a plain ``--library openusd`` run
    still yields a runnable Binaries dir instead of missing slang/dxc/d3d12/
    embree DLLs. Source SDK subdirs that don't exist yet are skipped.
    """
    # Mirror the `folders` mapping used in main()'s per-library branches.
    runtime_bins = ["slang/bin", "dxc/bin/x64", "embree/bin"]
    if is_windows():
        runtime_bins.append("d3d12/bin")
    for folder in runtime_bins:
        src_path = os.path.join(os.path.dirname(__file__), "SDK", folder)
        if not os.path.isdir(src_path):
            continue  # not extracted yet (run --library <lib> or --all first)
        for target in targets:
            copytree_common_to_binaries(folder, target=target, dry_run=dry_run)


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
    """
    Download and extract an archive file (supports .zip and .tar.gz).
    Cross-platform: handles both Windows .zip and Linux .tar.gz archives.
    """
    import tarfile
    
    archive_path = os.path.join(os.path.dirname(__file__), "SDK", "cache", url.split("/")[-1])
    if os.path.exists(archive_path):
        print(f"Using cached file {archive_path}")
    else:
        if not dry_run:
            print(f"Downloading from {url}...")
        download_with_progress(url, archive_path, dry_run)

    if dry_run:
        print(f"[DRY RUN] Would extract {archive_path} to {extract_path}")
        return

    print(f"Extracting to {extract_path}...")
    try:
        # Detect archive type and extract accordingly
        if archive_path.endswith('.tar.gz') or archive_path.endswith('.tgz'):
            with tarfile.open(archive_path, "r:gz") as tar_ref:
                tar_ref.extractall(extract_path)
        else:
            with zipfile.ZipFile(archive_path, "r") as zip_ref:
                zip_ref.extractall(extract_path)
        print(f"Downloaded and extracted successfully.")
        for target in targets:
            copytree_common_to_binaries(folder, target=target, dry_run=dry_run)
    except Exception as e:
        print(f"Error extracting {archive_path}: {e}")


openusd_version = "26.03"


def fix_slang_symlinks(dry_run=False):
    """
    Fix broken symlinks in SDK/slang/lib directory.

    Slang's official GitHub releases have a bug where Unix symlinks are
    stored as text files containing the target filename instead of proper
    symlinks. This function detects and fixes them.

    Only processes files smaller than 256 bytes (symlink stubs are tiny,
    real library binaries are MB). Also validates that symlink targets
    are real files, not other symlinks or missing files.
    """
    slang_lib_dir = os.path.join(os.path.dirname(__file__), "SDK", "slang", "lib")

    if not os.path.exists(slang_lib_dir):
        print(f"  ⚠ Slang lib directory not found: {slang_lib_dir}")
        return

    fixed_count = 0
    for filename in os.listdir(slang_lib_dir):
        filepath = os.path.join(slang_lib_dir, filename)

        # Skip if it's already a proper symlink
        if os.path.islink(filepath):
            continue

        # Check if it's a small file that might be a broken symlink stub
        # Real library binaries are large (MB), symlink stubs are tiny (< 256 bytes)
        if not os.path.isfile(filepath):
            continue
        if os.path.getsize(filepath) > 256:
            continue

        # Read the file content
        try:
            with open(filepath, 'r') as f:
                content = f.read().strip()

            # Check if content looks like a library filename
            if content.endswith('.so') or '.so.' in content:
                target_path = os.path.join(slang_lib_dir, content)
                # Only create symlink if target is a real file (not a symlink or directory)
                if os.path.isfile(target_path) and not os.path.islink(target_path):
                    if dry_run:
                        print(f"  [DRY RUN] Would fix symlink: {filename} -> {content}")
                    else:
                        os.remove(filepath)
                        os.symlink(content, filepath)
                        print(f"  ✓ Fixed symlink: {filename} -> {content}")
                        fixed_count += 1
                else:
                    print(f"  ⚠ Skipping {filename}: target '{content}' is not a real file")
        except Exception as e:
            # Not a text file or other error, skip
            pass

    if fixed_count > 0:
        print(f"  Fixed {fixed_count} broken symlinks in SDK/slang/lib/")


def validate_slang_sdk():
    """
    Validate that the Slang SDK is complete and usable.
    Returns True if valid, False otherwise.
    """
    slang_lib_dir = os.path.join(os.path.dirname(__file__), "SDK", "slang", "lib")
    slang_include_dir = os.path.join(os.path.dirname(__file__), "SDK", "slang", "include")

    # Check required files exist and resolve to real content
    required_libs = ["libslang.so", "libslang-rt.so"]
    if is_linux():
        required_libs.append("libgfx.so")

    valid = True
    for lib in required_libs:
        lib_path = os.path.join(slang_lib_dir, lib)
        if not os.path.exists(lib_path):
            print(f"  ✗ Missing: {lib}")
            valid = False
        else:
            # Resolve the real file (follow symlinks)
            real_path = os.path.realpath(lib_path)
            if not os.path.isfile(real_path):
                print(f"  ✗ Broken symlink: {lib} -> {real_path}")
                valid = False
            elif os.path.getsize(real_path) < 1024:
                print(f"  ✗ Suspiciously small: {lib} ({os.path.getsize(real_path)} bytes)")
                valid = False
            else:
                size_mb = os.path.getsize(real_path) / (1024 * 1024)
                print(f"  ✓ {lib} -> {os.path.basename(real_path)} ({size_mb:.1f} MB)")

    # Check include directory
    slang_h = os.path.join(slang_include_dir, "slang.h")
    if not os.path.exists(slang_h):
        print(f"  ✗ Missing: slang.h in include directory")
        valid = False

    return valid


def setup_slang_libs_for_binaries(targets, dry_run=False):
    """
    Copy Slang libraries to Binaries/{target}/ for each build target.
    On Linux, resolves symlinks and copies the actual library files
    to ensure Binaries has self-contained real files (no broken symlinks).
    """
    slang_lib_dir = os.path.join(os.path.dirname(__file__), "SDK", "slang", "lib")

    if not os.path.exists(slang_lib_dir):
        print(f"  ⚠ Slang lib directory not found: {slang_lib_dir}")
        return

    for target in targets:
        target_dir = os.path.join(os.getcwd(), "Binaries", target)
        if dry_run:
            print(f"  [DRY RUN] Would copy Slang libraries to Binaries/{target}/")
            continue

        os.makedirs(target_dir, exist_ok=True)

        for filename in os.listdir(slang_lib_dir):
            # Skip subdirectories (cmake/, slang-standard-module/)
            src = os.path.join(slang_lib_dir, filename)
            if not os.path.isfile(src) and not os.path.islink(src):
                continue

            dst = os.path.join(target_dir, filename)

            # Remove existing file/symlink first so we always get a real file
            if os.path.exists(dst) or os.path.islink(dst):
                os.remove(dst)

            if os.path.islink(src):
                # Resolve symlink and copy the real file content
                real_src = os.path.realpath(src)
                if os.path.isfile(real_src):
                    shutil.copy2(real_src, dst)
                    print(f"  ✓ Copied {filename} (resolved from {os.path.basename(real_src)})")
                else:
                    print(f"  ⚠ Skipping broken symlink: {filename}")
            else:
                shutil.copy2(src, dst)

        print(f"  ✓ Slang libraries copied to Binaries/{target}/")



# ============================================================
# Dependency build infrastructure
# ============================================================


def _rmtree_readonly(top):
    """shutil.rmtree that handles read-only files on Windows."""
    def on_ro(func, path, _exc):
        os.chmod(path, stat.S_IWRITE)
        func(path)
    shutil.rmtree(top, onerror=on_ro)


def _patch_oiiio_fmt_msvc_debug(src_dir, dry_run=False):
    """Patch OpenImageIO's vendored fmt 10.0 to build under MSVC Debug.

    fmt 10.0's ``#if defined(_SECURE_SCL) && _SECURE_SCL`` branch aliases
    ``checked_ptr`` to the now-deprecated ``stdext::checked_array_iterator``.
    Under MSVC 19.x Debug (``_ITERATOR_DEBUG_LEVEL != 0``) this alias fails
    to resolve and the trailing return types below it error out (fmt issue
    #3540). The STL no longer emits the "copying to a raw pointer" warnings
    this code existed to silence, so we force the plain-pointer branch —
    the same fix fmt itself shipped in later versions.

    Idempotent: matched by a marker comment so re-runs are a no-op.
    """
    fmt_h = os.path.join(
        src_dir, "ext", "fmt", "include", "fmt", "format.h"
    )
    if not os.path.isfile(fmt_h) or dry_run:
        return
    with open(fmt_h, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    marker = "fmt 10.0: force the T* branch"
    if marker in text:
        return  # already patched
    old = (
        "#if defined(_SECURE_SCL) && _SECURE_SCL\n"
        "// Make a checked iterator to avoid MSVC warnings.\n"
        "template <typename T> using checked_ptr = stdext::checked_array_iterator<T*>;\n"
        "template <typename T>\n"
        "constexpr auto make_checked(T* p, size_t size) -> checked_ptr<T> {\n"
        "  return {p, size};\n"
        "}\n"
        "#else\n"
        "template <typename T> using checked_ptr = T*;\n"
        "template <typename T> constexpr auto make_checked(T* p, size_t) -> T* {\n"
        "  return p;\n"
        "}\n"
        "#endif"
    )
    new = (
        "// " + marker + ". The _SECURE_SCL / checked_array_iterator\n"
        "// path breaks under MSVC 19.x Debug (fmt issue #3540).\n"
        "template <typename T> using checked_ptr = T*;\n"
        "template <typename T> constexpr auto make_checked(T* p, size_t) -> T* {\n"
        "  return p;\n"
        "}"
    )
    if old not in text:
        print(
            "  ⚠ OpenImageIO fmt patch anchor not found (fmt version changed?); "
            "leaving format.h unchanged."
        )
        return
    text = text.replace(old, new, 1)
    with open(fmt_h, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print("  ✓ Patched OpenImageIO vendored fmt for MSVC Debug (issue #3540)")


def dep_is_installed(install_prefix, marker_file):
    """Check if a dependency is already installed by checking marker file."""
    marker_path = os.path.join(install_prefix, marker_file)
    if os.path.exists(marker_path):
        print(f"  SKIP {marker_file} (already installed)")
        return True
    return False


def download_dep(url, src_base_dir, folder_name=None, dry_run=False):
    """Download and extract a dependency source. Skips if source already extracted."""
    if folder_name is None:
        folder_name = (
            url.split("/")[-1]
            .replace(".zip", "")
            .replace(".tar.gz", "")
            .replace(".tgz", "")
        )

    src_dir = os.path.join(src_base_dir, folder_name)
    if os.path.exists(src_dir):
        print(f"  Source already extracted: {src_dir}")
        return src_dir

    archive_name = url.split("/")[-1]
    cache_path = os.path.join(
        os.path.dirname(__file__), "SDK", "cache", archive_name
    )
    if not os.path.exists(cache_path):
        if dry_run:
            print(f"[DRY RUN] Would download {url}")
            return src_dir
        print(f"  Downloading {url}...")
        download_with_progress(url, cache_path, dry_run)

    if dry_run:
        print(f"[DRY RUN] Would extract {cache_path} to {src_base_dir}")
        return src_dir

    print(f"  Extracting {cache_path} to {src_base_dir}...")
    if cache_path.endswith(".tar.gz") or cache_path.endswith(".tgz"):
        with tarfile.open(cache_path, "r:gz") as tar_ref:
            tar_ref.extractall(src_base_dir)
    else:
        with zipfile.ZipFile(cache_path, "r") as zip_ref:
            zip_ref.extractall(src_base_dir)

    print(f"  Extracted to {src_dir}")
    return src_dir


_MSOFT_VS_ENV_APPLIED = False


def find_visual_studio():
    """Locate the latest Visual Studio installation via vswhere.

    Returns the installation path (e.g. C:\\Program Files\\Microsoft Visual
    Studio\\18\\Community) or None if not found / not on Windows.
    """
    if not is_windows():
        return None
    candidates = [
        os.path.join(
            os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
            "Microsoft Visual Studio", "Installer", "vswhere.exe",
        ),
        os.path.join(
            os.environ.get("ProgramFiles", r"C:\Program Files"),
            "Microsoft Visual Studio", "Installer", "vswhere.exe",
        ),
    ]
    vswhere = next((p for p in candidates if os.path.isfile(p)), None)
    if not vswhere:
        return None
    # The VC-tools component requirement is intentionally omitted: some
    # installations report it unreliably (e.g. VS 2026 previews), which would
    # cause a false negative. vcvarsall.bat below validates the toolset anyway.
    out = subprocess.run(
        [vswhere, "-latest", "-products", "*", "-property", "installationPath"],
        capture_output=True, text=True,
    )
    path = out.stdout.strip()
    return path or None


def _ensure_msvc_env(force=False):
    """Source vcvarsall.bat (x64) into os.environ on Windows, idempotently.

    Ninja needs cl.exe / INCLUDE / LIB to be visible. When configure.py is run
    from a plain shell (not a Developer PowerShell), none of these are set, so
    every CMake configure with the Ninja generator fails with "No
    CMAKE_C_COMPILER could be found". Mirrors build_devshell.ps1's
    Enter-VsDevShell approach but in pure Python so the script works from any
    shell.
    """
    global _MSOFT_VS_ENV_APPLIED
    if not is_windows() or (_MSOFT_VS_ENV_APPLIED and not force):
        return
    # Already in a Developer shell: cl.exe is reachable, nothing to do.
    if shutil.which("cl") or os.environ.get("VCINSTALLDIR"):
        _MSOFT_VS_ENV_APPLIED = True
        return

    vs_path = find_visual_studio()
    if not vs_path:
        print(
            "Warning: could not locate a Visual Studio installation via "
            "vswhere; MSVC environment will not be set up. Run this script "
            "from a Developer PowerShell if CMake configure fails."
        )
        return

    vcvarsall = os.path.join(vs_path, "VC", "Auxiliary", "Build", "vcvarsall.bat")
    if not os.path.isfile(vcvarsall):
        print(f"Warning: {vcvarsall} not found; skipping MSVC env setup.")
        return

    # cmd parses our string directly, so the quoted path with spaces is handled
    # correctly. list-form subprocess would double-escape the quotes.
    cmd = f'"{vcvarsall}" x64 >nul 2>&1 && set'
    out = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if out.returncode != 0:
        print(
            f"Warning: vcvarsall.bat failed (rc={out.returncode}). "
            "MSVC environment may be incomplete.\n"
            f"{out.stderr.strip()}"
        )
        return

    for line in out.stdout.splitlines():
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        # Preserve case sensitivity of existing vars where possible, but
        # Windows env lookups are case-insensitive so this is cosmetic.
        os.environ[key] = value

    _MSOFT_VS_ENV_APPLIED = True
    print(f"MSVC environment loaded from {vs_path}")


def run_cmake_build(src_dir, install_prefix, build_type, extra_args=None, dry_run=False):
    """Generic CMake configure + build + install using Ninja."""
    _ensure_msvc_env()
    parent = os.path.dirname(src_dir)
    dep_name = os.path.basename(src_dir)
    build_dir = os.path.join(parent, f"{dep_name}-build-{build_type.lower()}")
    os.makedirs(build_dir, exist_ok=True)

    cmake_args = [
        "cmake",
        "-G",
        "Ninja",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        f"-DCMAKE_INSTALL_PREFIX={install_prefix}",
        f"-DCMAKE_PREFIX_PATH={install_prefix}",
    ]

    if is_linux():
        cmake_args.append("-DCMAKE_INSTALL_RPATH=$ORIGIN")

    if extra_args:
        cmake_args.extend(extra_args)

    cmake_args.append(src_dir)

    if dry_run:
        print(f"[DRY RUN] CMake configure: {' '.join(cmake_args)}")
        print(f"[DRY RUN] CMake build & install in {build_dir}")
        return

    print(f"  Configuring: {dep_name} ({build_type})...")
    subprocess.run(cmake_args, cwd=build_dir, check=True)

    print(f"  Building & installing: {dep_name}...")
    subprocess.run(
        ["cmake", "--build", ".", "--target", "install"],
        cwd=build_dir,
        check=True,
    )
    print(f"  Installed: {dep_name}")


# ============================================================
# Individual dependency build functions
# ============================================================


def build_zlib(install_prefix, src_base, build_type, dry_run=False):
    """Build zlib 1.2.13"""
    if dep_is_installed(install_prefix, "include/zlib.h"):
        return
    url = "https://github.com/madler/zlib/archive/refs/tags/v1.2.13.zip"
    src_dir = download_dep(url, src_base, folder_name="zlib-1.2.13", dry_run=dry_run)
    if dry_run:
        return
    run_cmake_build(src_dir, install_prefix, build_type, dry_run=dry_run)


def build_tbb(install_prefix, src_base, build_type, dry_run=False):
    """Build TBB 2021.12.0"""
    if dep_is_installed(install_prefix, "include/oneapi/tbb.h"):
        return
    url = "https://github.com/oneapi-src/oneTBB/archive/refs/tags/v2021.12.0.zip"
    src_dir = download_dep(url, src_base, folder_name="oneTBB-2021.12.0", dry_run=dry_run)
    if dry_run:
        return
    run_cmake_build(
        src_dir, install_prefix, build_type,
        extra_args=["-DTBB_TEST=OFF"],
        dry_run=dry_run,
    )


def build_blosc(install_prefix, src_base, build_type, dry_run=False):
    """Build c-blosc 1.20.1"""
    if dep_is_installed(install_prefix, "include/blosc.h"):
        return
    url = "https://github.com/Blosc/c-blosc/archive/refs/tags/v1.20.1.zip"
    src_dir = download_dep(url, src_base, folder_name="c-blosc-1.20.1", dry_run=dry_run)
    if dry_run:
        return
    run_cmake_build(
        src_dir, install_prefix, build_type,
        extra_args=["-DDEACTIVATE_SNAPPY=ON"],
        dry_run=dry_run,
    )


def _patch_boost_config_toolset(engine_dir):
    """Patch config_toolset.bat to route vcunk (VS 2026 / v145) to Config_VCUNK."""
    config_toolset = os.path.join(engine_dir, "config_toolset.bat")
    if not os.path.isfile(config_toolset):
        return
    with open(config_toolset, "r") as f:
        content = f.read()
    vcunk_route = 'if "_%B2_TOOLSET%_" == "_vcunk_" call :Config_VCUNK'
    if vcunk_route in content:
        return
    insert_after = 'if "_%B2_TOOLSET%_" == "_vc143_" call :Config_VC143'
    if insert_after in content:
        content = content.replace(insert_after, insert_after + "\n" + vcunk_route)
        with open(config_toolset, "w") as f:
            f.write(content)
        print("  Patched config_toolset.bat: added vcunk routing for VS 2026")


def _patch_boost_msvc_jam(tools_dir):
    """Patch msvc.jam to add MSVC 14.5 (VS 2026 / v145) support.

    Without this b2 falls back to msvc-6.0 and ARM architecture, causing
    broken compile commands that open .cpp files in the system editor.
    """
    msvc_jam = os.path.join(tools_dir, "msvc.jam")
    if not os.path.isfile(msvc_jam):
        return
    with open(msvc_jam, "r", encoding="utf-8") as f:
        content = f.read()
    if ".version-14.5-env" in content:
        return  # already patched

    # 1. Add 14.5 to known-versions (first = highest priority)
    content = content.replace(
        ".known-versions = 14.3 ",
        ".known-versions = 14.5 14.3 ",
        1,
    )
    # 2. Match 14.5 in generate-setup-cmd parent-path resolution
    content = content.replace(
        '[ MATCH "(14.[34])" : $(version) ]',
        '[ MATCH "(14.[345])" : $(version) ]',
        1,
    )
    # 3. Add 14.5 to vswhere version check
    content = content.replace(
        "if $(version) in 14.1 14.2 14.3 default && $(root)",
        "if $(version) in 14.1 14.2 14.3 14.5 default && $(root)",
        1,
    )
    # 4. Add vswhere version range for 14.5 (VS 18.x)
    content = content.replace(
        'if $(version) = 14.3\n'
        '            {\n'
        '                limit = "-version \\"[17.0,18.0)\\" -prerelease" ;',
        'if $(version) = 14.5\n'
        '            {\n'
        '                limit = "-version \\"[18.0,19.0)\\" -prerelease" ;\n'
        '            }\n'
        '            else if $(version) = 14.3\n'
        '            {\n'
        '                limit = "-version \\"[17.0,18.0)\\" -prerelease" ;',
        1,
    )
    # 5. Add version-14.5 path and env variables
    content = content.replace(
        ".version-14.3-env = VS170COMNTOOLS ProgramFiles ProgramFiles(x86) ;",
        ".version-14.3-env = VS170COMNTOOLS ProgramFiles ProgramFiles(x86) ;\n"
        ".version-14.5-path =\n"
        '    "../../VC/Tools/MSVC/*/bin/Host*/*"\n'
        '    "Microsoft Visual Studio/18/*/VC/Tools/MSVC/*/bin/Host*/*"\n'
        "    ;\n"
        ".version-14.5-env = VS180COMNTOOLS ProgramFiles ProgramFiles(x86) ;",
        1,
    )

    with open(msvc_jam, "w", encoding="utf-8") as f:
        f.write(content)
    print("  Patched msvc.jam: added MSVC 14.5 (VS 2026) support")


def build_boost(install_prefix, src_base, build_type, dry_run=False):
    """Build Boost 1.90.0 via b2.

    On Windows, calls bootstrap.bat and b2.exe directly (no wrapper bat).
    Passes 'msvc' as the first bootstrap argument so build.bat uses it
    directly instead of trying to auto-detect (which fails for VS 2026 / v145).
    Also patches config_toolset.bat to handle the vcunk toolset.
    """
    marker = os.path.join("include", "boost-1_90", "boost", "version.hpp")
    if dep_is_installed(install_prefix, marker):
        return
    url = "https://archives.boost.io/release/1.90.0/source/boost_1_90_0.zip"
    src_dir = download_dep(url, src_base, folder_name="boost_1_90_0", dry_run=dry_run)
    if dry_run:
        return

    variant = "release" if build_type.lower() == "release" else "debug"

    if is_windows():
        src_dir_win = os.path.normpath(os.path.abspath(src_dir))
        install_win = os.path.normpath(os.path.abspath(install_prefix))
        build_dir = os.path.join(
            os.path.dirname(src_dir_win), f"boost-build-{build_type.lower()}"
        )
        engine_dir = os.path.join(src_dir_win, "tools", "build", "src", "engine")
        tools_dir = os.path.join(src_dir_win, "tools", "build", "src", "tools")

        # Patch config_toolset.bat so vcunk is routed to Config_VCUNK.
        _patch_boost_config_toolset(engine_dir)
        # Patch msvc.jam so b2 detects MSVC 14.5 (VS 2026).
        _patch_boost_msvc_jam(tools_dir)

        # Put engine dir in PATH so nested batch calls (guess_toolset.bat,
        # vswhere_usability_wrapper.cmd) can be found.
        env = os.environ.copy()
        env["PATH"] = os.path.normpath(engine_dir) + ";" + env.get("PATH", "")

        # Step 1: bootstrap (compiles b2.exe from source).
        # "msvc" as FIRST arg → build.bat sets B2_TOOLSET=msvc directly,
        # bypassing Guess_Toolset which would fail for VS 2026.
        bootstrap_path = os.path.join(src_dir_win, "bootstrap.bat")
        print("  Bootstrapping Boost (toolset=msvc)...")
        subprocess.run(
            ["cmd.exe", "/d", "/c", bootstrap_path, "msvc"],
            cwd=src_dir_win, env=env, check=True,
        )

        # Step 2: b2 install
        b2_path = os.path.join(src_dir_win, "b2.exe")
        b2_args = [
            b2_path, "install",
            f"--prefix={install_win}",
            f"--build-dir={build_dir}",
            "toolset=msvc",
            "address-model=64",
            "link=shared",
            "runtime-link=shared",
            "threading=multi",
            f"variant={variant}",
            "--with-atomic", "--with-regex",
            "--with-date_time", "--with-chrono",
            "--with-system", "--with-thread",
            "--with-iostreams", "--with-filesystem",
            "-sNO_BZIP2=1",
            f"-j{os.cpu_count() or 4}",
        ]
        print(f"  Building Boost (variant={variant}, toolset=msvc)...")
        subprocess.run(b2_args, cwd=src_dir_win, env=env, check=True)
    else:
        subprocess.run(
            ["./bootstrap.sh", f"--prefix={install_prefix}"],
            cwd=src_dir, check=True,
        )
        b2_args = [
            "./b2", "install",
            f"--prefix={install_prefix}",
            f"--build-dir={os.path.join(src_base, 'boost-build')}",
            "address-model=64", "link=shared",
            "runtime-link=shared", "threading=multi",
            f"variant={variant}",
            "--with-atomic", "--with-regex",
            "--with-date_time", "--with-chrono",
            "--with-system", "--with-thread",
            "--with-iostreams", "--with-filesystem",
        ]
        if is_macos():
            b2_args.append("toolset=clang")
        subprocess.run(b2_args, cwd=src_dir, check=True)

    print("  Installed: Boost")


def build_openexr(install_prefix, src_base, build_type, dry_run=False):
    """Build OpenEXR 3.1.13"""
    if dep_is_installed(install_prefix, "include/OpenEXR/ImfVersion.h"):
        return
    url = (
        "https://github.com/AcademySoftwareFoundation/"
        "openexr/archive/refs/tags/v3.1.13.zip"
    )
    src_dir = download_dep(url, src_base, folder_name="openexr-3.1.13", dry_run=dry_run)
    if dry_run:
        return
    run_cmake_build(
        src_dir, install_prefix, build_type,
        extra_args=["-DBUILD_TESTING=OFF", "-DOPENEXR_INSTALL_TOOLS=OFF"],
        dry_run=dry_run,
    )


def build_opensubdiv(install_prefix, src_base, build_type, dry_run=False):
    """Build OpenSubdiv 3.6.1"""
    if dep_is_installed(install_prefix, "include/opensubdiv/version.h"):
        return
    url = (
        "https://github.com/PixarAnimationStudios/"
        "OpenSubdiv/archive/refs/tags/v3_6_1.zip"
    )
    src_dir = download_dep(url, src_base, folder_name="OpenSubdiv-3_6_1", dry_run=dry_run)
    if dry_run:
        return
    run_cmake_build(
        src_dir, install_prefix, build_type,
        extra_args=[
            "-DBUILD_SHARED_LIBS=ON",
            "-DNO_CUDA=ON", "-DNO_OPENCL=ON",
            "-DNO_DX=ON", "-DNO_METAL=ON",
            "-DNO_TESTS=ON", "-DNO_REGRESSION=ON",
            "-DNO_EXAMPLES=ON", "-DNO_TUTORIALS=ON",
            "-DNO_GLFW=ON", "-DNO_GLUT=ON",
        ],
        dry_run=dry_run,
    )


def build_ptex(install_prefix, src_base, build_type, dry_run=False):
    """Build Ptex 2.4.2"""
    if dep_is_installed(install_prefix, "include/PtexVersion.h"):
        return
    url = "https://github.com/wdas/ptex/archive/refs/tags/v2.4.2.zip"
    src_dir = download_dep(url, src_base, folder_name="ptex-2.4.2", dry_run=dry_run)
    if dry_run:
        return
    run_cmake_build(
        src_dir, install_prefix, build_type,
        extra_args=["-DPTEX_BUILD_STATIC_LIBS=OFF"],
        dry_run=dry_run,
    )


def build_materialx(install_prefix, src_base, build_type, dry_run=False):
    """Build MaterialX 1.39.3"""
    if dep_is_installed(install_prefix, "include/MaterialXCore/Library.h"):
        return
    url = (
        "https://github.com/AcademySoftwareFoundation/"
        "MaterialX/archive/refs/tags/v1.39.3.zip"
    )
    src_dir = download_dep(url, src_base, folder_name="MaterialX-1.39.3", dry_run=dry_run)
    if dry_run:
        return
    run_cmake_build(
        src_dir, install_prefix, build_type,
        extra_args=[
            "-DMATERIALX_BUILD_SHARED_LIBS=ON",
            "-DMATERIALX_BUILD_TESTS=OFF",
        ],
        dry_run=dry_run,
    )


def build_opencolorio(install_prefix, src_base, build_type, dry_run=False):
    """Build OpenColorIO 2.2.1"""
    if dep_is_installed(install_prefix, "include/OpenColorIO/OpenColorABI.h"):
        return
    url = (
        "https://github.com/AcademySoftwareFoundation/"
        "OpenColorIO/archive/refs/tags/v2.2.1.zip"
    )
    src_dir = download_dep(url, src_base, folder_name="OpenColorIO-2.2.1", dry_run=dry_run)
    if dry_run:
        return

    # Patch missing #include <cstring> for GCC 13+ compatibility
    filerules_path = os.path.join(src_dir, "src", "OpenColorIO", "FileRules.cpp")
    if os.path.exists(filerules_path):
        with open(filerules_path, "r", encoding="utf-8") as f:
            content = f.read()
        find_str = '#include <algorithm>\n#include <cctype>'
        replace_str = '#include <algorithm>\n#include <cctype>\n#include <cstring>'
        if find_str in content:
            content = content.replace(find_str, replace_str)
            with open(filerules_path, "w", encoding="utf-8") as f:
                f.write(content)
            print("  Patched FileRules.cpp: added #include <cstring>")

    run_cmake_build(
        src_dir, install_prefix, build_type,
        extra_args=[
            "-DOCIO_BUILD_APPS=OFF",
            "-DOCIO_BUILD_GPU_TESTS=OFF",
            "-DOCIO_BUILD_TESTS=OFF",
            "-DOCIO_BUILD_DOCS=OFF",
            "-DOCIO_BUILD_PYTHON=OFF",
        ],
        dry_run=dry_run,
    )


def build_openvdb(install_prefix, src_base, build_type, cuda=False, dry_run=False):
    """Build OpenVDB 12.0.1 with optional NanoVDB/CUDA"""
    if dep_is_installed(install_prefix, "include/openvdb/openvdb.h"):
        return
    url = (
        "https://github.com/AcademySoftwareFoundation/"
        "openvdb/archive/refs/tags/v12.0.1.zip"
    )
    src_dir = download_dep(url, src_base, folder_name="openvdb-12.0.1", dry_run=dry_run)
    if dry_run:
        return

    extra_args = [
        "-DUSE_EXPLICIT_INSTANTIATION=OFF",
        "-DNANOVDB_USE_OPENVDB=ON",
        "-DOPENVDB_BUILD_PYTHON_MODULE=OFF",
        "-DOPENVDB_BUILD_BINARIES=OFF",
        "-DOPENVDB_BUILD_UNITTESTS=OFF",
    ]
    # TBB 2021.x headers have #pragma comment(lib, "tbb12_debug.lib") on MSVC.
    # Disable it since we link via CMake targets, not MSVC auto-linking.
    if is_windows():
        # TBB headers have #pragma comment(lib, "tbb12_debug.lib") on MSVC.
        # Disable it via __TBB_NO_IMPLICIT_LINKAGE.
        # Must preserve /EHsc (C++ exceptions) since overriding CMAKE_CXX_FLAGS
        # replaces all defaults including /EHsc. Without /EHsc, Boost detects
        # BOOST_NO_EXCEPTIONS which changes throw_exception to a non-inline decl.
        extra_args.append('-DCMAKE_CXX_FLAGS=/EHsc /D__TBB_NO_IMPLICIT_LINKAGE')
    if cuda:
        extra_args.extend([
            "-DUSE_NANOVDB=ON",
            "-DNANOVDB_USE_CUDA=ON",
            "-DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler",
        ])

    run_cmake_build(
        src_dir, install_prefix, build_type,
        extra_args=extra_args,
        dry_run=dry_run,
    )


def build_libjpeg(install_prefix, src_base, build_type, dry_run=False):
    """Build libjpeg-turbo 2.0.1"""
    if dep_is_installed(install_prefix, "include/turbojpeg.h"):
        return
    url = (
        "https://github.com/libjpeg-turbo/libjpeg-turbo/"
        "archive/refs/tags/2.0.1.zip"
    )
    src_dir = download_dep(
        url, src_base, folder_name="libjpeg-turbo-2.0.1", dry_run=dry_run
    )
    if dry_run:
        return
    run_cmake_build(
        src_dir, install_prefix, build_type,
        extra_args=["-DWITH_SIMD=OFF"],
        dry_run=dry_run,
    )


def build_libpng(install_prefix, src_base, build_type, dry_run=False):
    """Build libpng 1.6.47"""
    if dep_is_installed(install_prefix, "include/png.h"):
        return
    url = (
        "https://github.com/pnggroup/libpng/"
        "archive/refs/tags/v1.6.47.zip"
    )
    src_dir = download_dep(
        url, src_base, folder_name="libpng-1.6.47", dry_run=dry_run
    )
    if dry_run:
        return
    run_cmake_build(
        src_dir, install_prefix, build_type,
        extra_args=["-DPNG_FRAMEWORK=OFF", "-DPNG_TESTS=OFF"],
        dry_run=dry_run,
    )


def build_libtiff(install_prefix, src_base, build_type, dry_run=False):
    """Build libtiff 4.0.7"""
    if dep_is_installed(install_prefix, "include/tiff.h"):
        return
    url = (
        "https://gitlab.com/libtiff/libtiff/-/archive/v4.0.7/"
        "libtiff-v4.0.7.zip"
    )
    src_dir = download_dep(
        url, src_base, folder_name="libtiff-v4.0.7", dry_run=dry_run
    )
    if dry_run:
        return
    # Patch: skip building tools and tests to avoid extra dependencies
    cmake_file = os.path.join(src_dir, "CMakeLists.txt")
    if os.path.isfile(cmake_file):
        with open(cmake_file, "r") as f:
            content = f.read()
        content = content.replace(
            "add_subdirectory(tools)", "# add_subdirectory(tools)"
        )
        content = content.replace(
            "add_subdirectory(test)", "# add_subdirectory(test)"
        )
        with open(cmake_file, "w") as f:
            f.write(content)
    run_cmake_build(
        src_dir, install_prefix, build_type,
        extra_args=["-Dld-version-script=OFF"],
        dry_run=dry_run,
    )


def build_openimageio(install_prefix, src_base, build_type, dry_run=False):
    """Build OpenImageIO 2.5.16.0"""
    if dep_is_installed(install_prefix, "include/OpenImageIO/oiioversion.h"):
        return
    url = (
        "https://github.com/AcademySoftwareFoundation/"
        "OpenImageIO/archive/refs/tags/v2.5.16.0.zip"
    )
    src_dir = download_dep(url, src_base, folder_name="OpenImageIO-2.5.16.0", dry_run=dry_run)
    if dry_run:
        return
    # Patch vendored fmt 10.0 so the MSVC Debug build compiles (fmt #3540).
    _patch_oiiio_fmt_msvc_debug(src_dir, dry_run=dry_run)
    extra_args = [
        "-DOIIO_BUILD_TESTS=OFF",
        "-DUSE_PYTHON=OFF",
        "-DBUILD_DOCS=OFF",
        "-DSTOP_ON_WARNING=OFF",
        "-DBoost_NO_SYSTEM_PATHS=ON",
    ]
    # Disable debug postfix so libraries are named OpenImageIO.lib (not _d)
    # USD's FindOpenImageIO.cmake only expects the _d suffix on macOS
    if build_type.lower() == "debug":
        extra_args.append("-DCMAKE_DEBUG_POSTFIX=")

    run_cmake_build(
        src_dir, install_prefix, build_type,
        extra_args=extra_args,
        dry_run=dry_run,
    )


def build_usd(install_prefix, usd_src_dir, build_type, python_executable, dry_run=False):
    """Build OpenUSD"""
    if dep_is_installed(install_prefix, "include/pxr/pxr.h"):
        return

    extra_args = [
        "-DPXR_BUILD_MONOLITHIC=ON",
        "-DPXR_ENABLE_GL_SUPPORT=ON",
        "-DPXR_ENABLE_PYTHON_SUPPORT=ON",
        "-DPXR_ENABLE_OPENVDB_SUPPORT=ON",
        "-DPXR_BUILD_OPENIMAGEIO_PLUGIN=ON",
        "-DPXR_BUILD_OPENCOLORIO_PLUGIN=ON",
        "-DPXR_ENABLE_PTEX_SUPPORT=ON",
        "-DPXR_ENABLE_MATERIALX_SUPPORT=ON",
        "-DPXR_BUILD_IMAGING=ON",
        "-DPXR_BUILD_USD_IMAGING=ON",
        "-DPXR_BUILD_USDVIEW=OFF",
        "-DPXR_BUILD_EXAMPLES=OFF",
        "-DPXR_BUILD_TUTORIALS=OFF",
        "-DPXR_BUILD_TESTS=OFF",
        "-DPXR_BUILD_DOCUMENTATION=OFF",
        "-DBoost_NO_SYSTEM_PATHS=ON",
        f"-DPython3_EXECUTABLE={python_executable}",
        # Pin FindPython3 to the SDK python directory so the Development
        # components resolve against THIS python (e.g. SDK/python 3.13), not
        # whatever python happens to sort first on PATH (e.g. a scoop
        # python312). Without this, FindPython3's Development validation can
        # query a different interpreter via subprocess and report the
        # components as missing even though the SDK python is complete.
        f"-DPython3_ROOT_DIR={os.path.dirname(python_executable)}",
        "-DPython3_FIND_STRATEGY=LOCATION",
        "-DPython3_FIND_REGISTRY=NEVER",
    ]

    if is_windows():
        extra_args.append("-DCMAKE_CXX_FLAGS=/Zm150")

    run_cmake_build(
        usd_src_dir, install_prefix, build_type,
        extra_args=extra_args,
        dry_run=dry_run,
    )


def process_usd(targets, dry_run=False, keep_original_files=True, copy_only=False):
    if not copy_only:
        # Download and extract OpenUSD source
        url = (
            "https://github.com/PixarAnimationStudios/"
            f"OpenUSD/archive/refs/tags/v{openusd_version}.zip"
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
                    print("Downloaded and extracted successfully.")
                except Exception as e:
                    print(f"Error extracting {zip_path}: {e}")
                    return

        usd_src_dir = os.path.join(extract_path, f"OpenUSD-{openusd_version}")

        # Detect Python
        try:
            subprocess.check_output(
                ["python_d", "--version"], stderr=subprocess.STDOUT
            )
            has_python_d = True
        except (subprocess.CalledProcessError, FileNotFoundError):
            has_python_d = False

        # Setup SDK/python if not present or incomplete (needs headers/libs to
        # build C/C++ extensions against it, not just the bare interpreter).
        sdk_python_dir = os.path.join(os.path.dirname(__file__), "SDK", "python")
        sdk_python = os.path.join(
            sdk_python_dir,
            "python.exe" if is_windows() else "bin/python3",
        )
        if not _python_install_is_complete(sdk_python_dir):
            if os.path.isdir(sdk_python_dir) and not _python_install_is_complete(sdk_python_dir):
                print(
                    "SDK/python is incomplete (missing headers/libs); "
                    "re-copying from system Python..."
                )
            # Find a real, complete system Python (skips venvs) and copy it
            # into SDK/python.
            python_dir = _find_system_python()
            if python_dir is None:
                print(
                    "ERROR: No complete Python installation found. Install "
                    "Python 3.13 (with dev headers/libs) and ensure "
                    "it is not only a virtualenv."
                )
                return
            print(f"Setting up SDK/python from {python_dir}...")
            _copy_python_installation(python_dir, sdk_python_dir, dry_run)
            if not dry_run and not _python_install_is_complete(sdk_python_dir):
                print(
                    "ERROR: SDK/python is still incomplete after copy "
                    "(source Python lacks dev headers/libs). Install the "
                    "Python development files and re-run."
                )

        # Find Python executable for USD Python bindings.
        # IMPORTANT: prefer SDK/python so USD links against the SAME Python
        # (3.13) as the Ruzino app itself. The app's pyd extensions
        # (stage_py, geometry_py, ...) are built against SDK/python; if USD
        # were linked against a different system Python (e.g. 3.12 from
        # scoop), usd_ms.dll would depend on a different python3x.dll than
        # Binaries ships, and LoadLibrary(usd_ms) fails at runtime (e.g.
        # during nanobind stub generation). jinja2 must be installed into
        # SDK/python if Python binding codegen is needed.
        python_cmd = "python" if is_windows() else "python3"
        sdk_python_abs = sdk_python if os.path.exists(sdk_python) else None
        python_executable = sdk_python_abs or shutil.which(python_cmd) or python_cmd

        # Shared source directory for all dependency sources
        src_base = os.path.join(
            os.path.dirname(__file__), "SDK", "OpenUSD", "src"
        )
        if not dry_run:
            os.makedirs(src_base, exist_ok=True)

        # Enable long path support for Windows before building
        if is_windows():
            try:
                subprocess.run(
                    ["git", "config", "--global", "core.longpaths", "true"],
                    check=False,
                )
                print("Enabled Git long path support")
            except Exception as e:
                print(f"Warning: Could not enable Git long path support: {e}")

        # Build each target
        for target in targets:
            print(f"\n{'=' * 60}")
            print(f"Building OpenUSD dependencies for {target}")
            print(f"{'=' * 60}")

            install_prefix = os.path.join(
                os.path.dirname(__file__), "SDK", "OpenUSD", target
            )

            # Clean old install artifacts before monolithic build
            if os.path.exists(install_prefix):
                print(f"Cleaning old install artifacts in {install_prefix}...")
                for item in os.listdir(install_prefix):
                    item_path = os.path.join(install_prefix, item)
                    if os.path.isdir(item_path) and item not in ("src", "build"):
                        _rmtree_readonly(item_path)
                    elif os.path.isfile(item_path):
                        os.chmod(item_path, stat.S_IWRITE)
                        os.remove(item_path)
            else:
                os.makedirs(install_prefix, exist_ok=True)

            # Select Python executable for this target
            if has_python_d and target == "Debug":
                py_exec = "python_d"
            else:
                py_exec = python_executable

            # Build dependencies in order
            print("\n--- Building zlib ---")
            build_zlib(install_prefix, src_base, target, dry_run)

            print("\n--- Building TBB ---")
            build_tbb(install_prefix, src_base, target, dry_run)

            print("\n--- Building Blosc ---")
            build_blosc(install_prefix, src_base, target, dry_run)

            print("\n--- Building Boost ---")
            build_boost(install_prefix, src_base, target, dry_run)

            print("\n--- Building OpenEXR ---")
            build_openexr(install_prefix, src_base, target, dry_run)

            print("\n--- Building OpenSubdiv ---")
            build_opensubdiv(install_prefix, src_base, target, dry_run)

            print("\n--- Building Ptex ---")
            build_ptex(install_prefix, src_base, target, dry_run)

            print("\n--- Building MaterialX ---")
            build_materialx(install_prefix, src_base, target, dry_run)

            print("\n--- Building OpenColorIO ---")
            build_opencolorio(install_prefix, src_base, target, dry_run)

            print("\n--- Building OpenVDB ---")
            build_openvdb(
                install_prefix, src_base, target, cuda=True, dry_run=dry_run
            )

            print("\n--- Building libjpeg ---")
            build_libjpeg(install_prefix, src_base, target, dry_run)
            print("\n--- Building libpng ---")
            build_libpng(install_prefix, src_base, target, dry_run)
            print("\n--- Building libtiff ---")
            build_libtiff(install_prefix, src_base, target, dry_run)

            print("\n--- Building OpenImageIO ---")
            build_openimageio(install_prefix, src_base, target, dry_run)

            print("\n--- Building USD ---")
            build_usd(install_prefix, usd_src_dir, target, py_exec, dry_run)

            print(f"\n{'=' * 60}")
            print(f"OpenUSD {target} build complete")
            print(f"{'=' * 60}")

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
        if is_windows():
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
    dst_dir = os.path.join(os.path.dirname(__file__), "SDK", "SDK_pack_temp")

    # Find Python installation path using platform-appropriate command
    if is_windows():
        where_python = (
            subprocess.check_output(["where", "python"]).decode("utf-8").split("\n")[0].strip()
        )
    else:
        where_python = (
            subprocess.check_output(["which", "python3"]).decode("utf-8").strip()
        )

    python_dir = os.path.dirname(where_python)
    framework3d_dir = os.getcwd()


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

            # Replace Python path with placeholder (handle both forward and backslash variants)
            filedata = filedata.replace(python_dir, "${Python3_ROOT_DIR}")
            filedata = filedata.replace(python_dir.replace("\\", "/"), "${Python3_ROOT_DIR}")
            filedata = filedata.replace(python_dir.replace("/", "\\"), "${Python3_ROOT_DIR}")

            # Replace Framework3D path with placeholder (handle both forward and backslash variants)
            filedata = filedata.replace(framework3d_dir, "${FRAMEWORK3D_DIR}")
            filedata = filedata.replace(framework3d_dir.replace("\\", "/"), "${FRAMEWORK3D_DIR}")
            filedata = filedata.replace(framework3d_dir.replace("/", "\\"), "${FRAMEWORK3D_DIR}")


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
            # Only replace in the specific path: SDK/OpenUSD/<variant>/include/nanovdb/util/GridBuilder.h
            if ("GridBuilder.h" in dst_file and
                "include" in dst_file and
                "nanovdb" in dst_file and
                "util" in dst_file and
                "OpenUSD" in dst_file):
                for old_text, new_text in gridbuilder_replacements.items():
                    if old_text in filedata:
                        filedata = filedata.replace(old_text, new_text)
                        print(f"Replaced '{old_text}' with '{new_text}' in {dst_file}")

            if filedata != filedata_0:
                with open(dst_file, "w", encoding="utf-8") as file:
                    file.write(filedata)
                    print(f"Found and replaced path in {dst_file}")

    with concurrent.futures.ThreadPoolExecutor() as executor:
        futures = []
        # Platform-specific directory separators for skip detection
        sep = os.sep
        skip_patterns = [f"{sep}build", f"{sep}cache", f"{sep}src", f"{sep}source"]
        
        for root, dirs, files in os.walk(src_dir):
            # Skip build, cache directories and anything under */src/
            if any(skip_pattern in root for skip_pattern in skip_patterns):
                continue

            # Create corresponding directory in destination
            relative_path = os.path.relpath(root, src_dir)
            dst_path = os.path.join(dst_dir, relative_path)
            if not dry_run:
                os.makedirs(dst_path, exist_ok=True)

            for file in files:
                # Skip PDB files (Windows-only debug symbols)
                if is_windows() and file.endswith(".pdb"):
                    print(f"Skipping {os.path.join(root, file)}")
                    continue
                if file == "libopenvdb.lib":
                    print(f"Skipping {os.path.join(root, file)}")
                    continue

                src_file = os.path.join(root, file)
                dst_file = os.path.join(dst_path, file)
                futures.append(executor.submit(copy_file, src_file, dst_file))

        # Wait for all threads to complete
        concurrent.futures.wait(futures)

        # Copy Python installation
        python_dst_dir = os.path.join(dst_dir, "python")
        _copy_python_installation(python_dir, python_dst_dir, dry_run)

        # Pack the SDK_temp directory into SDK.zip
        sdk_archive_path = os.path.join(os.path.dirname(__file__), "SDK", "SDK")
        if dry_run:
            print(f"[DRY RUN] Would pack {dst_dir} into SDK.zip")
        else:
            shutil.make_archive(sdk_archive_path, "zip", dst_dir)
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
                    def on_rm_error(func, path, exc):
                        """Error handler for shutil.rmtree to handle read-only files (cross-platform)"""
                        if not os.access(path, os.W_OK):
                            os.chmod(path, stat.S_IWUSR | stat.S_IREAD | stat.S_IRGRP | stat.S_IROTH)
                            func(path)
                        else:
                            raise

                    shutil.rmtree(dst_dir, onerror=on_rm_error)
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
        args.library = ["openusd", "slang", "dxc", "embree"]
        if is_windows():
            args.library.append("d3d12")
    elif not args.library:
        print(
            "No library specified and --all not set. No libraries will be configured."
        )
        return
    else:
        args.library = [args.library]

    if dry_run:
        print(f"[DRY RUN] Selected build variants: {targets}")

    # Platform-specific SDK download URLs
    # D3D12 is Windows-only, other libraries have platform-specific downloads
    plat = get_platform()
    if plat == 'windows':
        urls = {
            "slang": "https://github.com/shader-slang/slang/releases/download/v2025.22.1/slang-2025.22.1-windows-x86_64.zip",
            "d3d12": "https://globalcdn.nuget.org/packages/microsoft.direct3d.d3d12.1.616.1.nupkg",
            "dxc": "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2505.1/dxc_2025_07_14.zip",
            "embree": "https://github.com/RenderKit/embree/releases/download/v4.4.0/embree-4.4.0.x64.windows.zip",
        }
    elif plat == 'linux':
        urls = {
            "slang": "https://github.com/shader-slang/slang/releases/download/v2025.22.1/slang-2025.22.1-linux-x86_64.zip",
            "dxc": "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2505.1/dxc_2025_07_14.zip",
            "embree": "https://github.com/RenderKit/embree/releases/download/v4.4.0/embree-4.4.0.x86_64.linux.tar.gz",
        }
    elif plat == 'macos':
        urls = {
            "slang": "https://github.com/shader-slang/slang/releases/download/v2025.22.1/slang-2025.22.1-macos-x86_64.zip",
            "dxc": "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2505.1/dxc_2025_07_14.zip",
            "embree": "https://github.com/RenderKit/embree/releases/download/v4.4.0/embree-4.4.0.x64.macos.zip",
        }
    else:
        print(f"Warning: Unsupported platform '{plat}', using Linux URLs as fallback")
        urls = {
            "slang": "https://github.com/shader-slang/slang/releases/download/v2025.22.1/slang-2025.22.1-linux-x86_64.zip",
            "dxc": "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2505.1/dxc_2025_07_14.zip",
            "embree": "https://github.com/RenderKit/embree/releases/download/v4.4.0/embree-4.4.0.x64.linux.tar.gz",
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
        elif lib == "d3d12" and is_windows():
            if not copy_only:
                # Download the nupkg file
                nupkg_path = os.path.join(os.path.dirname(__file__), "SDK", "cache", "d3d12.nupkg")
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
                extract_path = os.path.join(os.path.dirname(__file__), "SDK", "d3d12")
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
                extract_path = os.path.join(os.path.dirname(__file__), "SDK", "dxc")
                zip_path = os.path.join(os.path.dirname(__file__), "SDK", "cache", "dxc.zip")
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

                        # Find and move binaries to bin directory (cross-platform)
                        for root, _, files in os.walk(extract_path):
                            for file in files:
                                # Check for platform-specific binary extensions
                                bin_exts = (".exe", ".dll", ".lib") if is_windows() else (".so", ".a", "")
                                if any(file.endswith(ext) for ext in bin_exts if ext) or (not is_windows() and not os.path.splitext(file)[1]):
                                    # On Linux, also consider extensionless files as potential binaries
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
                    os.path.join(os.path.dirname(__file__), "SDK", "embree"),
                    folders[lib],
                    targets,
                    dry_run,
                )
            # Copy the embree bin files to the binaries folder
            for target in targets:
                copytree_common_to_binaries(
                    folders[lib], target=target, dry_run=dry_run
                )
        elif lib == "slang":
            # Slang needs special handling on Linux for symlink issues
            extract_path = os.path.join(os.path.dirname(__file__), "SDK", "slang")

            if not copy_only:
                # Download
                archive_path = os.path.join(os.path.dirname(__file__), "SDK", "cache", urls[lib].split("/")[-1])
                if os.path.exists(archive_path):
                    print(f"Using cached file {archive_path}")
                else:
                    if not dry_run:
                        print(f"Downloading from {urls[lib]}...")
                    download_with_progress(urls[lib], archive_path, dry_run)

                if dry_run:
                    print(f"[DRY RUN] Would extract {archive_path} to {extract_path}")
                else:
                    # On Linux, clean lib/ directory before extraction to avoid
                    # stale broken symlinks from previous runs
                    if is_linux() or is_macos():
                        slang_lib_dir = os.path.join(extract_path, "lib")
                        if os.path.exists(slang_lib_dir):
                            print(f"  Cleaning previous {slang_lib_dir}...")
                            shutil.rmtree(slang_lib_dir, ignore_errors=True)

                    # Extract
                    print(f"Extracting Slang to {extract_path}...")
                    try:
                        with zipfile.ZipFile(archive_path, "r") as zip_ref:
                            zip_ref.extractall(extract_path)
                        print(f"✓ Slang extracted successfully")
                    except Exception as e:
                        print(f"Error extracting Slang: {e}")
                        return

                    # On Linux/macOS: fix text-file symlinks and validate
                    if is_linux() or is_macos():
                        print("Fixing Slang symlinks...")
                        fix_slang_symlinks(dry_run=dry_run)

                        print("Validating Slang SDK...")
                        if not validate_slang_sdk():
                            print("  ✗ Slang SDK validation failed!")
                            print("  Try deleting SDK/slang/ and re-running configure.py")
                        else:
                            print("  ✓ Slang SDK is valid")

            # Copy slang runtime binaries to Binaries
            for target in targets:
                copytree_common_to_binaries(
                    folders[lib], target=target, dry_run=dry_run
                )

            # On Linux/macOS: copy resolved library files to Binaries
            if is_linux() or is_macos():
                setup_slang_libs_for_binaries(targets, dry_run=dry_run)
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

    # Always copy Python DLLs from SDK to Binaries for each target. Python is
    # a pure runtime dependency (no Debug/Release ABI split), so copying it
    # unconditionally keeps Binaries/{target} runnable regardless of which
    # --library was built.
    copy_python_dlls_to_binaries(targets, dry_run=dry_run)

    # Always copy third-party runtime DLLs (slang/dxc/embree/d3d12 bin) to
    # Binaries/{target}. These are prebuilt upstream archives (no compile),
    # so copying is fast and side-effect-free; doing it unconditionally means
    # a plain `--library openusd` run still produces a runnable Binaries dir
    # instead of leaving slang/dxc/d3d12/embree DLLs missing.
    _copy_runtime_bin_dlls_to_binaries(targets, dry_run=dry_run)

    # Always copy CUDA runtime DLLs if available
    copy_cuda_runtime_dlls_to_binaries(targets, dry_run=dry_run)

    # Always copy imgui.ini to Binaries
    copy_imgui_ini_to_binaries(targets, dry_run=dry_run)

    # Always copy nvHLSLExtns.h to SDK/slang/include/
    copy_nvapi_header_to_slang(dry_run=dry_run)


if __name__ == "__main__":
    main()
