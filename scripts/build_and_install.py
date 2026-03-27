#!/usr/bin/env python3
"""
Build and Install Script for Ruzino Project

This script provides a complete build, install, and test workflow:
1. Configure CMake with installation prefix
2. Build the project
3. Install CMake targets
4. Install runtime dependencies (DLLs, resources, Python, etc.)
5. Optionally run tests to verify the installation

Usage:
    python scripts/build_and_install.py --install-dir /path/to/install [options]

Examples:
    # Basic installation
    python scripts/build_and_install.py --install-dir ../RuzinoInstall

    # Installation with tests
    python scripts/build_and_install.py --install-dir ../RuzinoInstall --with-tests

    # Custom build type
    python scripts/build_and_install.py --install-dir ../RuzinoInstall --build-type Debug

    # Dry run (see what would happen)
    python scripts/build_and_install.py --install-dir ../RuzinoInstall --dry-run
"""

import argparse
import os
import sys
import shutil
import subprocess
import platform
from pathlib import Path
from typing import List, Optional

IS_WINDOWS = platform.system() == "Windows"
IS_LINUX = platform.system() == "Linux"
IS_MACOS = platform.system() == "Darwin"


class Colors:
    """ANSI color codes for terminal output."""

    HEADER = "\033[95m"
    OKBLUE = "\033[94m"
    OKCYAN = "\033[96m"
    OKGREEN = "\033[92m"
    WARNING = "\033[93m"
    FAIL = "\033[91m"
    ENDC = "\033[0m"
    BOLD = "\033[1m"
    UNDERLINE = "\033[4m"

    @staticmethod
    def disable():
        """Disable colors (for non-TTY output)."""
        Colors.HEADER = ""
        Colors.OKBLUE = ""
        Colors.OKCYAN = ""
        Colors.OKGREEN = ""
        Colors.WARNING = ""
        Colors.FAIL = ""
        Colors.ENDC = ""
        Colors.BOLD = ""
        Colors.UNDERLINE = ""


def print_header(message: str):
    """Print a formatted header message."""
    print(f"\n{Colors.BOLD}{Colors.HEADER}{'=' * 80}{Colors.ENDC}")
    print(f"{Colors.BOLD}{Colors.HEADER}{message.center(80)}{Colors.ENDC}")
    print(f"{Colors.BOLD}{Colors.HEADER}{'=' * 80}{Colors.ENDC}\n")


def print_step(step_num: int, total_steps: int, message: str):
    """Print a formatted step message."""
    print(
        f"\n{Colors.BOLD}{Colors.OKCYAN}[Step {step_num}/{total_steps}] {message}{Colors.ENDC}"
    )
    print(f"{Colors.OKCYAN}{'-' * 80}{Colors.ENDC}")


def print_success(message: str):
    """Print a success message."""
    print(f"{Colors.OKGREEN}✓ {message}{Colors.ENDC}")


def print_warning(message: str):
    """Print a warning message."""
    print(f"{Colors.WARNING}⚠ {message}{Colors.ENDC}")


def print_error(message: str):
    """Print an error message."""
    print(f"{Colors.FAIL}✗ {message}{Colors.ENDC}", file=sys.stderr)


def run_command(
    cmd: List[str],
    cwd: Optional[Path] = None,
    dry_run: bool = False,
    check: bool = True,
    capture_output: bool = False,
) -> subprocess.CompletedProcess:
    """
    Run a command with proper error handling.

    Args:
        cmd: Command and arguments to run
        cwd: Working directory
        dry_run: If True, print command without executing
        check: If True, raise exception on non-zero exit
        capture_output: If True, capture stdout/stderr

    Returns:
        CompletedProcess instance
    """
    cmd_str = " ".join(str(c) for c in cmd)

    if dry_run:
        print(f"{Colors.WARNING}[DRY RUN]{Colors.ENDC} Would run: {cmd_str}")
        return subprocess.CompletedProcess(cmd, 0, b"", b"")

    print(f"Running: {cmd_str}")

    try:
        result = subprocess.run(
            cmd,
            cwd=cwd,
            check=check,
            capture_output=capture_output,
            text=True if capture_output else False,
        )
        return result
    except subprocess.CalledProcessError as e:
        print_error(f"Command failed with exit code {e.returncode}")
        if capture_output:
            if e.stdout:
                print(f"STDOUT:\n{e.stdout}")
            if e.stderr:
                print(f"STDERR:\n{e.stderr}", file=sys.stderr)
        raise


def check_prerequisites(project_root: Path):
    """Check that all prerequisites are installed."""
    print_header("Checking Prerequisites")

    errors = []

    # Check CMake
    try:
        result = run_command(["cmake", "--version"], capture_output=True, check=False)
        version = result.stdout.split("\n")[0] if result.stdout else "unknown"
        print_success(f"CMake: {version}")
    except FileNotFoundError:
        errors.append("CMake not found. Please install CMake >= 3.28")

    # Check Ninja (warning only, cmake --build can work without it in PATH)
    ninja_warning = False
    try:
        result = run_command(["ninja", "--version"], capture_output=True, check=False)
        print_success(f"Ninja: {result.stdout.strip()}")
    except FileNotFoundError:
        ninja_warning = True
        print_warning("Ninja not found in PATH. Will use 'cmake --build' instead.")
        print_warning(
            "If build fails, ensure Ninja is available or rebuild with different generator."
        )

    # Check Python
    try:
        result = run_command(
            [sys.executable, "--version"], capture_output=True, check=False
        )
        print_success(f"Python: {result.stdout.strip()}")
    except Exception as e:
        errors.append(f"Python check failed: {e}")

    # Check SDK directory
    sdk_dir = project_root / "SDK"
    if not sdk_dir.exists():
        errors.append(f"SDK directory not found: {sdk_dir}")
    else:
        print_success(f"SDK directory: {sdk_dir}")

    # Check source directory
    source_dir = project_root / "source"
    if not source_dir.exists():
        errors.append(f"Source directory not found: {source_dir}")
    else:
        print_success(f"Source directory: {source_dir}")

    if errors:
        print_error("Prerequisites check failed:")
        for error in errors:
            print_error(f"  - {error}")
        sys.exit(1)

    print_success("All prerequisites satisfied!")


def configure_cmake(
    build_dir: Path,
    install_dir: Path,
    build_type: str,
    with_cuda: bool,
    with_tests: bool,
    dry_run: bool,
) -> bool:
    """
    Configure CMake with proper installation settings.

    Returns:
        True if configuration succeeded, False otherwise
    """
    print_step(1, 5, "Configuring CMake")

    # Create build directory
    if not dry_run:
        build_dir.mkdir(parents=True, exist_ok=True)

    # Build CMake command
    cmd = [
        "cmake",
        "-G",
        "Ninja",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        f"-DCMAKE_INSTALL_PREFIX={install_dir}",
    ]

    # Add CUDA option
    if with_cuda:
        cmd.append("-DRUZINO_WITH_CUDA=ON")
    else:
        cmd.append("-DRUZINO_WITH_CUDA=OFF")

    # Disable homework plugins for SDK installation
    cmd.append("-DUSTC_HOMEWORK_PLUGINS=OFF")

    # Add test installation option
    if with_tests:
        cmd.append("-DRUZINO_INSTALL_TESTS=ON")
    else:
        cmd.append("-DRUZINO_INSTALL_TESTS=OFF")

    # Add source directory
    cmd.append("..")

    try:
        run_command(cmd, cwd=build_dir, dry_run=dry_run)
        print_success("CMake configuration completed!")
        return True
    except subprocess.CalledProcessError:
        print_error("CMake configuration failed!")
        return False


def build_project(build_dir: Path, build_type: str, dry_run: bool) -> bool:
    """
    Build the project using cmake --build.

    Returns:
        True if build succeeded, False otherwise
    """
    print_step(2, 5, f"Building Project ({build_type})")

    # Use cmake --build instead of ninja directly for better compatibility
    cmd = ["cmake", "--build", "."]

    # Add parallel build flag for faster builds
    if not IS_WINDOWS:
        cmd.extend(["--", "-j4"])

    try:
        run_command(cmd, cwd=build_dir, dry_run=dry_run)
        print_success("Build completed successfully!")
        return True
    except subprocess.CalledProcessError:
        print_error("Build failed!")
        return False


def install_cmake_targets(build_dir: Path, dry_run: bool) -> bool:
    """
    Install CMake targets to installation directory.

    Returns:
        True if installation succeeded, False otherwise
    """
    print_step(3, 5, "Installing CMake Targets")

    cmd = ["cmake", "--install", "."]

    try:
        run_command(cmd, cwd=build_dir, dry_run=dry_run)
        print_success("CMake targets installed successfully!")
        return True
    except subprocess.CalledProcessError:
        print_error("CMake installation failed!")
        return False


def install_dependencies(
    project_root: Path,
    install_dir: Path,
    build_type: str,
    dry_run: bool,
) -> bool:
    """
    Install runtime dependencies using install_deps.py script.

    Returns:
        True if dependency installation succeeded, False otherwise
    """
    print_step(4, 5, "Installing Runtime Dependencies")

    install_deps_script = project_root / "scripts" / "install_deps.py"

    if not install_deps_script.exists():
        print_error(f"install_deps.py not found: {install_deps_script}")
        return False

    cmd = [
        sys.executable,
        str(install_deps_script),
        "--install-dir",
        str(install_dir),
        "--build-type",
        build_type,
    ]

    if dry_run:
        cmd.append("--dry-run")

    try:
        run_command(
            cmd, cwd=project_root, dry_run=False
        )  # install_deps.py handles dry_run itself
        print_success("Runtime dependencies installed successfully!")
        return True
    except subprocess.CalledProcessError:
        print_error("Dependency installation failed!")
        return False


def run_tests(
    project_root: Path,
    install_dir: Path,
    dry_run: bool,
) -> bool:
    """
    Run tests to verify installation.

    Returns:
        True if all tests passed, False otherwise
    """
    print_step(5, 5, "Running Tests")

    run_tests_script = project_root / "scripts" / "run_all_tests.py"

    if not run_tests_script.exists():
        print_error(f"run_all_tests.py not found: {run_tests_script}")
        return False

    cmd = [
        sys.executable,
        str(run_tests_script),
        "--install-dir",
        str(install_dir),
    ]

    if dry_run:
        print(
            f"{Colors.WARNING}[DRY RUN]{Colors.ENDC} Would run tests from: {install_dir}"
        )
        return True

    try:
        result = run_command(cmd, cwd=project_root, check=False)
        if result.returncode == 0:
            print_success("All tests passed!")
            return True
        else:
            print_error("Some tests failed. Please check the test output above.")
            return False
    except Exception as e:
        print_error(f"Test execution failed: {e}")
        return False


def print_installation_summary(
    install_dir: Path,
    build_type: str,
    with_tests: bool,
    tests_passed: Optional[bool],
):
    """Print a summary of the installation."""
    print_header("Installation Summary")

    print(f"{Colors.BOLD}Installation Directory:{Colors.ENDC} {install_dir}")
    print(f"{Colors.BOLD}Build Type:{Colors.ENDC} {build_type}")
    print(f"{Colors.BOLD}Tests Installed:{Colors.ENDC} {'Yes' if with_tests else 'No'}")

    if tests_passed is not None:
        status = (
            f"{Colors.OKGREEN}Passed{Colors.ENDC}"
            if tests_passed
            else f"{Colors.FAIL}Failed{Colors.ENDC}"
        )
        print(f"{Colors.BOLD}Test Results:{Colors.ENDC} {status}")

    print(f"\n{Colors.BOLD}Installation Structure:{Colors.ENDC}")
    print(f"  {install_dir}/")
    print(f"  ├── bin/           # Applications and DLLs")
    if with_tests:
        print(f"  │   └── tests/     # Test executables")
    print(f"  ├── lib/           # Static libraries and CMake configs")
    print(f"  └── include/       # Header files")

    print(f"\n{Colors.BOLD}Next Steps:{Colors.ENDC}")
    print(f"  1. Add to PATH: {install_dir}/bin")
    print(f"  2. Use in CMake: -DCMAKE_PREFIX_PATH={install_dir}")
    print(f"  3. Run applications from: {install_dir}/bin")

    print(f"\n{Colors.BOLD}Example CMake Usage:{Colors.ENDC}")
    print(f"  find_package(Ruzino REQUIRED)")
    print(f"  target_link_libraries(your_target PRIVATE Ruzino::geometry)")


def main():
    """Main entry point for the build and install script."""
    parser = argparse.ArgumentParser(
        description="Build and install Ruzino Framework3D",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --install-dir ../RuzinoInstall
  %(prog)s --install-dir /opt/ruzino --with-tests --build-type Release
  %(prog)s --install-dir ./install --dry-run
        """,
    )

    parser.add_argument(
        "--install-dir",
        type=str,
        required=True,
        help="Installation directory (e.g., ../RuzinoInstall, /opt/ruzino)",
    )

    parser.add_argument(
        "--build-type",
        type=str,
        default="Release",
        choices=["Release", "Debug", "RelWithDebInfo"],
        help="Build type (default: Release)",
    )

    parser.add_argument(
        "--with-tests",
        action="store_true",
        help="Install tests and run them after installation",
    )

    parser.add_argument(
        "--with-cuda",
        action="store_true",
        help="Enable CUDA support during build",
    )

    parser.add_argument(
        "--dry-run",
        "-n",
        action="store_true",
        help="Print actions without executing them",
    )

    parser.add_argument(
        "--skip-prerequisites",
        action="store_true",
        help="Skip prerequisites check",
    )

    parser.add_argument(
        "--build-dir",
        type=str,
        default="build",
        help="Build directory (default: build)",
    )

    parser.add_argument(
        "--no-color",
        action="store_true",
        help="Disable colored output",
    )

    args = parser.parse_args()

    # Disable colors if requested or if not in a TTY
    if args.no_color or not sys.stdout.isatty():
        Colors.disable()

    # Resolve paths
    project_root = Path(__file__).parent.parent
    install_dir = Path(args.install_dir)
    build_dir = Path(args.build_dir)

    # Make paths absolute if they're relative
    if not install_dir.is_absolute():
        install_dir = project_root / install_dir
    if not build_dir.is_absolute():
        build_dir = project_root / build_dir

    # Print banner
    print_header("Ruzino Framework3D - Build and Install")

    print(f"{Colors.BOLD}Configuration:{Colors.ENDC}")
    print(f"  Project Root:    {project_root}")
    print(f"  Build Directory: {build_dir}")
    print(f"  Install Dir:     {install_dir}")
    print(f"  Build Type:      {args.build_type}")
    print(f"  With CUDA:       {'Yes' if args.with_cuda else 'No'}")
    print(f"  With Tests:      {'Yes' if args.with_tests else 'No'}")
    print(f"  Dry Run:         {'Yes' if args.dry_run else 'No'}")

    # Check prerequisites
    if not args.skip_prerequisites:
        check_prerequisites(project_root)

    # Execute build and install workflow
    success = True

    # Step 1: Configure CMake
    if not configure_cmake(
        build_dir,
        install_dir,
        args.build_type,
        args.with_cuda,
        args.with_tests,
        args.dry_run,
    ):
        success = False

    # Step 2: Build project
    if success:
        if not build_project(build_dir, args.build_type, args.dry_run):
            success = False

    # Step 3: Install CMake targets
    if success:
        if not install_cmake_targets(build_dir, args.dry_run):
            success = False

    # Step 4: Install dependencies
    if success:
        if not install_dependencies(
            project_root,
            install_dir,
            args.build_type,
            args.dry_run,
        ):
            success = False

    # Step 5: Run tests (if requested)
    tests_passed = None
    if success and args.with_tests:
        tests_passed = run_tests(project_root, install_dir, args.dry_run)
        if not tests_passed:
            print_warning("Some tests failed, but installation completed.")

    # Print summary
    print_installation_summary(
        install_dir,
        args.build_type,
        args.with_tests,
        tests_passed if args.with_tests else None,
    )

    # Exit with appropriate code
    if success:
        print_success("Build and installation completed successfully!")
        sys.exit(0)
    else:
        print_error("Build and installation failed!")
        sys.exit(1)


if __name__ == "__main__":
    main()
