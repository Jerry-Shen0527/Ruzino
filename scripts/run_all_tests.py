#!/usr/bin/env python3
"""
Test Runner Script for Ruzino Project

This script:
1. Recursively finds all 'tests/' folders under './source/'
2. Runs pytest on any 'test_*.py' files found
3. Runs corresponding '*_test' executables from './Binaries/Release/' for any '.cpp' files

Usage:
    python run_all_tests.py                    # Run all tests
    python run_all_tests.py <test_name>       # Run specific test (e.g., 'rhi_test', 'cpu_slang')
"""

import os
import sys
import subprocess
import platform
from pathlib import Path
from typing import List, Tuple, Optional

# Platform detection
IS_WINDOWS = platform.system() == "Windows"
EXE_SUFFIX = ".exe" if IS_WINDOWS else ""

# Tests to skip in headless environment (UI/rendering related)
HEADLESS_SKIP_PATTERNS = [
    "gui",
    "editor",
    "render",
    "window",
    "imgui",
    "nvrhi",
    "opengl",
    "vulkan",
    "dx12",
    "cuda",
    "rhi",
]


def find_test_directories(source_dir: Path) -> List[Path]:
    """Find all 'tests/' directories under source_dir recursively."""
    test_dirs = []
    for root, dirs, _ in os.walk(source_dir):
        # Skip spdlog tests directory
        if "spdlog" in root:
            continue
        if "tests" in dirs:
            test_dirs.append(Path(root) / "tests")
    return test_dirs


def find_python_test_files(test_dir: Path) -> List[Path]:
    """Find all test_*.py files in the given directory."""
    return list(test_dir.glob("test_*.py"))


def find_cpp_test_files(test_dir: Path) -> List[Path]:
    """Find all *.cpp files in the given directory."""
    return list(test_dir.glob("*.cpp"))


def cpp_to_exe_name(cpp_file: Path) -> str:
    """Convert cpp filename to expected exe name.

    For example:
    - some_file.cpp -> some_file_test (Linux) or some_file_test.exe (Windows)
    - renderer.cpp -> renderer_test (Linux) or renderer_test.exe (Windows)
    """
    base_name = cpp_file.stem
    if base_name.endswith("_test"):
        return f"{base_name}{EXE_SUFFIX}"
    else:
        return f"{base_name}_test{EXE_SUFFIX}"


def should_run_test(exe_name: str, test_filter: Optional[str]) -> bool:
    """Check if a test should be run based on filter."""
    if test_filter is None:
        return True

    # Match test name (with or without _test suffix and .exe extension)
    test_base = exe_name.replace("_test", "").replace(".exe", "")
    filter_base = test_filter.replace("_test", "").replace(".exe", "")

    return filter_base.lower() in test_base.lower()


def should_skip_headless_test(exe_name: str) -> bool:
    """Check if a test should be skipped in headless environment."""
    exe_lower = exe_name.lower()
    for pattern in HEADLESS_SKIP_PATTERNS:
        if pattern in exe_lower:
            return True
    return False


def run_pytest(
    test_dir: Path, test_filter: Optional[str] = None
) -> Tuple[int, int, List[Tuple[str, str]]]:
    """Run pytest in the given directory.

    Returns:
        Tuple of (passed, failed, failed_test_info)
        where failed_test_info is a list of (test_name, output) tuples
    """
    python_tests = find_python_test_files(test_dir)

    if not python_tests:
        return 0, 0, []

    # Apply filter if specified
    if test_filter:
        python_tests = [
            t for t in python_tests if test_filter.lower() in t.stem.lower()
        ]
        if not python_tests:
            return 0, 0, []

    print(f"\n{'=' * 80}")
    print(f"Running pytest in: {test_dir}")
    print(f"{'=' * 80}")

    passed = 0
    failed = 0
    failed_tests = []

    for test_file in python_tests:
        print(f"\n--- Running: {test_file.name} ---")

        try:
            result = subprocess.run(
                ["pytest", str(test_file), "-v", "--tb=short"],
                cwd=test_dir,  # Run from test directory so relative imports work
                timeout=300,  # 5 minute timeout per test file
                capture_output=True,
                text=True,
            )

            if result.returncode == 0:
                print(f"✓ PASSED: {test_file.name}")
                passed += 1
            else:
                print(f"✗ FAILED: {test_file.name} (exit code: {result.returncode})")
                failed += 1
                output = result.stdout + result.stderr
                failed_tests.append((f"{test_dir.name}/{test_file.name}", output))

        except subprocess.TimeoutExpired as e:
            print(f"✗ TIMEOUT: {test_file.name}")
            failed += 1
            output = (e.stdout or b"").decode("utf-8", errors="ignore") + (
                e.stderr or b""
            ).decode("utf-8", errors="ignore")
            failed_tests.append(
                (f"{test_dir.name}/{test_file.name}", f"TIMEOUT after 300s\n{output}")
            )
        except Exception as e:
            print(f"✗ ERROR: {test_file.name} - {str(e)}")
            failed += 1
            failed_tests.append(
                (f"{test_dir.name}/{test_file.name}", f"ERROR: {str(e)}")
            )

    return passed, failed, failed_tests


def run_cpp_tests(
    test_dir: Path,
    binaries_dir: Path,
    test_filter: Optional[str] = None,
    skip_headless: bool = False,
) -> Tuple[int, int, int, List[Tuple[str, str]]]:
    """Run C++ test executables corresponding to cpp files in test_dir.

    Args:
        test_dir: Directory containing cpp test files
        binaries_dir: Directory containing compiled test executables
        test_filter: Optional filter to run only matching tests
        skip_headless: If True, skip UI/rendering tests in headless environment

    Returns:
        Tuple of (passed, failed, skipped, failed_test_info)
        where failed_test_info is a list of (test_name, output) tuples
    """
    cpp_files = find_cpp_test_files(test_dir)

    if not cpp_files:
        return 0, 0, 0, []

    print(f"\n{'=' * 80}")
    print(f"Running C++ tests from: {test_dir}")
    print(f"{'=' * 80}")

    passed = 0
    failed = 0
    skipped = 0
    failed_tests = []

    # For test executables, check bin/tests/ first (install mode), then bin/ (build mode)
    test_binaries_dir = (
        binaries_dir / "tests" if (binaries_dir / "tests").exists() else binaries_dir
    )

    for cpp_file in cpp_files:
        exe_name = cpp_to_exe_name(cpp_file)

        # Apply filter if specified
        if test_filter and not should_run_test(exe_name, test_filter):
            continue

        # Skip UI/rendering tests in headless environment
        if skip_headless and should_skip_headless_test(exe_name):
            print(f"\n--- Skipping: {exe_name} (headless environment) ---")
            skipped += 1
            continue

        exe_path = test_binaries_dir / exe_name

        print(f"\n--- Running: {exe_name} (from {cpp_file.name}) ---")

        if not exe_path.exists():
            print(f"⚠ SKIPPED: {exe_name} not found in {test_binaries_dir}")
            skipped += 1
            continue

        try:
            result = subprocess.run(
                [str(exe_path)],
                cwd=binaries_dir,
                timeout=300,  # 5 minute timeout per test
                capture_output=True,
                text=True,
            )

            if result.returncode == 0:
                print(f"✓ PASSED: {exe_name}")
                passed += 1
            else:
                print(f"✗ FAILED: {exe_name} (exit code: {result.returncode})")
                failed += 1
                output = result.stdout + result.stderr
                failed_tests.append((exe_name, output))

        except subprocess.TimeoutExpired as e:
            print(f"✗ TIMEOUT: {exe_name}")
            failed += 1
            output = (e.stdout or "") + (e.stderr or "")
            failed_tests.append((exe_name, f"TIMEOUT after 300s\n{output}"))
        except Exception as e:
            print(f"✗ ERROR: {exe_name} - {str(e)}")
            failed += 1
            failed_tests.append((exe_name, f"ERROR: {str(e)}"))

    return passed, failed, skipped, failed_tests


def main():
    """Main test runner."""
    import argparse

    # Parse command line arguments
    parser = argparse.ArgumentParser(description="Run Ruzino tests")
    parser.add_argument("test_filter", nargs="?", help="Filter to run specific tests")
    parser.add_argument(
        "--no-skip-headless",
        action="store_true",
        help="Do not skip UI/rendering tests in headless environment",
    )
    parser.add_argument(
        "--install-dir",
        type=str,
        help="Run tests from installation directory instead of build directory",
    )
    args = parser.parse_args()

    test_filter = args.test_filter
    skip_headless = not args.no_skip_headless

    if test_filter:
        print(f"Running tests matching: {test_filter}")

    if skip_headless:
        print("Skipping UI/rendering tests (headless environment)")

    # Setup paths
    if args.install_dir:
        # Run from installation directory
        install_dir = Path(args.install_dir)
        if not install_dir.is_absolute():
            install_dir = Path(__file__).parent.parent / install_dir
        binaries_dir = install_dir / "bin"
        project_root = Path(__file__).parent.parent
        source_dir = project_root / "source"
        print(f"Running tests from installation directory: {install_dir}")
    else:
        # Run from build directory (default)
        project_root = Path(
            __file__
        ).parent.parent  # Go up from scripts/ to project root
        source_dir = project_root / "source"
        binaries_dir = project_root / "Binaries" / "Release"
        print(f"Running tests from build directory")

    print(f"Starting test run...")
    print(
        f"Platform: {platform.system()} ({'Windows' if IS_WINDOWS else 'Linux/macOS'})"
    )
    print(f"Binaries directory: {binaries_dir}")
    print(f"Source directory: {source_dir}")
    print("-" * 80)

    # Check if binaries directory exists
    if not binaries_dir.exists():
        print(f"ERROR: Binaries directory not found: {binaries_dir}")
        sys.exit(1)

    # Find all test directories
    test_dirs = find_test_directories(source_dir)
    print(f"Found {len(test_dirs)} test directories")

    # Run all tests
    total_pytest_passed = 0
    total_pytest_failed = 0
    total_cpp_passed = 0
    total_cpp_failed = 0
    total_cpp_skipped = 0
    all_failed_tests = []

    for test_dir in test_dirs:
        print(f"\nProcessing: {test_dir.relative_to(project_root)}")

        # Run Python tests
        pytest_passed, pytest_failed, pytest_failed_tests = run_pytest(
            test_dir, test_filter
        )
        total_pytest_passed += pytest_passed
        total_pytest_failed += pytest_failed
        all_failed_tests.extend(pytest_failed_tests)

        if pytest_passed + pytest_failed > 0:
            print(f"  Python tests: {pytest_passed} passed, {pytest_failed} failed")

        # Run C++ tests
        cpp_passed, cpp_failed, cpp_skipped, cpp_failed_tests = run_cpp_tests(
            test_dir, binaries_dir, test_filter, skip_headless
        )
        total_cpp_passed += cpp_passed
        total_cpp_failed += cpp_failed
        total_cpp_skipped += cpp_skipped
        all_failed_tests.extend(cpp_failed_tests)

        if cpp_passed + cpp_failed + cpp_skipped > 0:
            print(
                f"  C++ tests: {cpp_passed} passed, {cpp_failed} failed, {cpp_skipped} skipped"
            )

    # Print summary to console
    print("\n" + "=" * 80)
    print("TEST SUMMARY")
    print("=" * 80)
    print(f"Python Tests: {total_pytest_passed} passed, {total_pytest_failed} failed")
    print(
        f"C++ Tests:    {total_cpp_passed} passed, {total_cpp_failed} failed, {total_cpp_skipped} skipped"
    )
    print(
        f"Overall:      {total_pytest_passed + total_cpp_passed} passed, "
        f"{total_pytest_failed + total_cpp_failed} failed, {total_cpp_skipped} skipped"
    )

    # Print failed tests if any
    if all_failed_tests:
        print("\n" + "=" * 80)
        print("FAILED TESTS:")
        print("=" * 80)
        for test_name, output in all_failed_tests:
            print(f"\n  ✗ {test_name}")
            print("-" * 80)
            # Print last 50 lines of output to avoid overwhelming the summary
            lines = output.strip().split("\n")
            if len(lines) > 50:
                print("  ... (output truncated, showing last 50 lines) ...\n")
                lines = lines[-50:]
            for line in lines:
                print(f"    {line}")
            print("-" * 80)

    print("=" * 80)

    # Exit with error code if any tests failed
    if total_pytest_failed + total_cpp_failed > 0:
        sys.exit(1)
    else:
        sys.exit(0)


if __name__ == "__main__":
    main()
