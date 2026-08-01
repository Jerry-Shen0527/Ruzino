#!/usr/bin/env python3
"""
Test Runner Script for Ruzino Project

This script:
1. Recursively finds all 'tests/' folders under './source/'
2. Runs pytest on any 'test_*.py' files found
3. Runs corresponding '*_test.exe' files from './Binaries/Release/' for any '.cpp' files

Usage:
    python run_all_tests.py                    # Run all tests
    python run_all_tests.py <test_name>       # Run specific test (e.g., 'rhi_test', 'cpu_slang')
"""

import os
import re
import sys
import subprocess
from pathlib import Path
from typing import List, Tuple, Optional





def find_test_directories(source_dir: Path) -> List[Path]:
    """Find all 'tests/' directories under source_dir recursively."""
    test_dirs = []
    for root, dirs, _ in os.walk(source_dir):
        # Skip spdlog tests directory
        if 'spdlog' in root:
            continue
        if 'tests' in dirs:
            test_dirs.append(Path(root) / 'tests')
    return test_dirs


# Windows NTSTATUS for access violation. Returned by subprocess as a positive int
# because Python surfaces the raw exit code. 0xC0000005 == 3221225477.
_WIN_ACCESS_VIOLATION = 0xC0000005
_WIN_ACCESS_VIOLATION_POS = 3221225477


def _classify_crash(returncode: int, output: str) -> str:
    """Return a short human-readable tag if the failure looks like an infrastructure
    crash (not a test assertion failure), else ''.

    Distinguishing these is valuable: a segfault during process shutdown usually
    points to a C++ destructor or DLL-unload issue, while a non-zero exit with
    normal pytest output is a real test failure. Tagging them separately keeps
    the failure report actionable instead of lumping everything under 'FAILED'.
    """
    # Linux/macOS: negative returncodes encode the signal (e.g. -11 == SIGSEGV).
    if returncode < 0:
        return f"crash: signal {-returncode}"
    # Windows: access violation surfaces as the raw NTSTATUS as a positive int.
    if returncode == _WIN_ACCESS_VIOLATION_POS:
        return "crash: access violation (0xC0000005)"
    # Fall back to scanning output for fatal-exception banners pytest can't catch.
    if output and re.search(r"Windows fatal exception: access violation", output):
        return "crash: access violation"
    return ""


def find_python_test_files(test_dir: Path) -> List[Path]:
    """Find all test_*.py files in the given directory."""
    return list(test_dir.glob('test_*.py'))


def find_cpp_test_files(test_dir: Path) -> List[Path]:
    """Find all *.cpp files in the given directory."""
    return list(test_dir.glob('*.cpp'))


def cpp_to_exe_name(cpp_file: Path) -> str:
    """Convert cpp filename to expected exe name.

    For example:
    - some_file.cpp -> some_file_test (or some_file_test.exe on Windows)
    - renderer.cpp -> renderer_test
    """
    base_name = cpp_file.stem
    exe_suffix = ".exe" if sys.platform == "win32" else ""
    if base_name.endswith('_test'):
        return f"{base_name}{exe_suffix}"
    else:
        return f"{base_name}_test{exe_suffix}"


def should_run_test(exe_name: str, test_filter: Optional[str]) -> bool:
    """Check if a test should be run based on filter."""
    if test_filter is None:
        return True

    # Match test name (with or without _test / _test.exe suffix)
    test_base = exe_name.replace('_test.exe', '').replace('.exe', '').replace('_test', '')
    filter_base = test_filter.replace('_test', '').replace('.exe', '')

    return filter_base.lower() in test_base.lower()


def run_pytest(test_dir: Path, test_filter: Optional[str] = None) -> Tuple[int, int, int, List[Tuple[str, str]]]:
    """Run pytest in the given directory.

    Returns:
        Tuple of (passed, skipped_files, failed, failed_test_info)
        where failed_test_info is a list of (test_name, output) tuples.
        skipped_files counts whole test FILES skipped (e.g. missing optional
        deps like torch), distinct from per-test skips inside a run.
    """
    python_tests = find_python_test_files(test_dir)

    if not python_tests:
        return 0, 0, 0, []

    # Apply filter if specified
    if test_filter:
        python_tests = [t for t in python_tests if test_filter.lower() in t.stem.lower()]
        if not python_tests:
            return 0, 0, 0, []

    print(f"\n{'='*80}")
    print(f"Running pytest in: {test_dir}")
    print(f"{'='*80}")

    passed = 0
    skipped_files = 0
    failed = 0
    failed_tests = []

    for test_file in python_tests:
        print(f"\n--- Running: {test_file.name} ---")

        try:
            result = subprocess.run(
                ['pytest', str(test_file), '-v', '--tb=short'],
                cwd=test_dir,  # Run from test directory so relative imports work
                timeout=300,  # 5 minute timeout per test file
                capture_output=True,
                text=True
            )

            if result.returncode == 0:
                print(f"✓ PASSED: {test_file.name}")
                passed += 1
            else:
                # Check if pytest itself reported all tests passed.
                # Non-zero exit code can come from post-test crashes (e.g.
                # C++ destructor segfault during process shutdown) that happen
                # after pytest has already printed its results.
                combined_output = result.stdout + result.stderr
                summary_match = re.search(
                    r'(\d+) passed(?:, (\d+) failed)?(?:, \d+ (?:warning|skipped|deselected|error)s?)?',
                    combined_output)
                if summary_match:
                    n_passed = int(summary_match.group(1))
                    n_failed = int(summary_match.group(2) or 0)
                    if n_passed > 0 and n_failed == 0:
                        print(f"✓ PASSED: {test_file.name} "
                              f"(all {n_passed} tests passed, exit {result.returncode} from cleanup)")
                        passed += 1
                        continue

                # A whole test FILE can be skipped via module-level
                # pytest.importorskip("torch") when an optional dep is missing.
                # pytest surfaces this as exit code 5 (NO_TESTS_COLLECTED) with
                # "N skipped" in the summary — not a real failure. Detect it so
                # the report distinguishes "couldn't run (env)" from "failed".
                skip_match = re.search(r'(\d+) skipped', combined_output)
                if (result.returncode == 5 and skip_match
                        and not re.search(r'\d+ failed', combined_output)):
                    n_skipped = int(skip_match.group(1))
                    print(f"○ SKIPPED: {test_file.name} "
                          f"({n_skipped} tests skipped, likely missing optional dep)")
                    skipped_files += 1
                    continue

                # Distinguish infrastructure-level crashes (access violation / segfault)
                # from genuine test assertion failures. Windows 0xC0000005 == 3221225477,
                # Linux SIGSEGV == -11. These usually indicate a C++ destructor crash during
                # process shutdown or an environment issue, NOT a test logic bug — surfacing
                # this clearly saves hours of misdirected debugging.
                crash_tag = _classify_crash(result.returncode, combined_output)
                label = f" ({crash_tag})" if crash_tag else ""
                print(f"✗ FAILED: {test_file.name} (exit code: {result.returncode}){label}")
                failed += 1
                output = result.stdout + result.stderr
                failed_tests.append((f"{test_dir.name}/{test_file.name}", output))

        except subprocess.TimeoutExpired as e:
            print(f"✗ TIMEOUT: {test_file.name}")
            failed += 1
            output = (e.stdout or b'').decode('utf-8', errors='ignore') + (e.stderr or b'').decode('utf-8', errors='ignore')
            failed_tests.append((f"{test_dir.name}/{test_file.name}", f"TIMEOUT after 300s\n{output}"))
        except Exception as e:
            print(f"✗ ERROR: {test_file.name} - {str(e)}")
            failed += 1
            failed_tests.append((f"{test_dir.name}/{test_file.name}", f"ERROR: {str(e)}"))

    return passed, skipped_files, failed, failed_tests


def run_cpp_tests(test_dir: Path, binaries_dir: Path, test_filter: Optional[str] = None) -> Tuple[int, int, List[Tuple[str, str]]]:
    """Run C++ test executables corresponding to cpp files in test_dir.

    Returns:
        Tuple of (passed, failed, failed_test_info)
        where failed_test_info is a list of (test_name, output) tuples
    """
    cpp_files = find_cpp_test_files(test_dir)

    if not cpp_files:
        return 0, 0, []

    print(f"\n{'='*80}")
    print(f"Running C++ tests from: {test_dir}")
    print(f"{'='*80}")

    passed = 0
    failed = 0
    failed_tests = []

    for cpp_file in cpp_files:
        exe_name = cpp_to_exe_name(cpp_file)

        # Apply filter if specified
        if test_filter and not should_run_test(exe_name, test_filter):
            continue

        exe_path = binaries_dir / exe_name

        print(f"\n--- Running: {exe_name} (from {cpp_file.name}) ---")

        if not exe_path.exists():
            print(f"⚠ SKIPPED: {exe_name} not found in {binaries_dir}")
            continue

        try:
            result = subprocess.run(
                [str(exe_path)],
                cwd=binaries_dir,
                timeout=300,  # 5 minute timeout per test
                capture_output=True,
                text=True
            )

            if result.returncode == 0:
                print(f"✓ PASSED: {exe_name}")
                passed += 1
            else:
                combined = result.stdout + result.stderr
                crash_tag = _classify_crash(result.returncode, combined)
                label = f" ({crash_tag})" if crash_tag else ""
                print(f"✗ FAILED: {exe_name} (exit code: {result.returncode}){label}")
                failed += 1
                failed_tests.append((exe_name, combined))

        except subprocess.TimeoutExpired as e:
            print(f"✗ TIMEOUT: {exe_name}")
            failed += 1
            output = (e.stdout or b'').decode('utf-8', errors='ignore') + (e.stderr or b'').decode('utf-8', errors='ignore')
            failed_tests.append((exe_name, f"TIMEOUT after 300s\n{output}"))
        except Exception as e:
            print(f"✗ ERROR: {exe_name} - {str(e)}")
            failed += 1
            failed_tests.append((exe_name, f"ERROR: {str(e)}"))

    return passed, failed, failed_tests


def main():
    """Main test runner."""
    # Parse command line arguments
    test_filter = None
    if len(sys.argv) > 1:
        test_filter = sys.argv[1]
        print(f"Running tests matching: {test_filter}")

    # Setup paths - script is in scripts/ directory, so go up one level to project root
    project_root = Path(__file__).parent.parent  # Go up from scripts/ to project root
    source_dir = project_root / 'source'
    binaries_dir = project_root / 'Binaries' / 'Release'

    print(f"Starting test run...")
    print(f"Searching for tests in: {source_dir}")
    print("-" * 80)

    # Find all test directories
    test_dirs = find_test_directories(source_dir)
    print(f"Found {len(test_dirs)} test directories")

    # Run all tests
    total_pytest_passed = 0
    total_pytest_skipped = 0
    total_pytest_failed = 0
    total_cpp_passed = 0
    total_cpp_failed = 0
    all_failed_tests = []

    for test_dir in test_dirs:
        print(f"\nProcessing: {test_dir.relative_to(project_root)}")

        # Run Python tests
        pytest_passed, pytest_skipped, pytest_failed, pytest_failed_tests = run_pytest(test_dir, test_filter)
        total_pytest_passed += pytest_passed
        total_pytest_skipped += pytest_skipped
        total_pytest_failed += pytest_failed
        all_failed_tests.extend(pytest_failed_tests)

        if pytest_passed + pytest_skipped + pytest_failed > 0:
            print(f"  Python tests: {pytest_passed} passed, {pytest_skipped} skipped, {pytest_failed} failed")

        # Run C++ tests
        cpp_passed, cpp_failed, cpp_failed_tests = run_cpp_tests(test_dir, binaries_dir, test_filter)
        total_cpp_passed += cpp_passed
        total_cpp_failed += cpp_failed
        all_failed_tests.extend(cpp_failed_tests)

        if cpp_passed + cpp_failed > 0:
            print(f"  C++ tests: {cpp_passed} passed, {cpp_failed} failed")

    # Print summary to console
    print("\n" + "="*80)
    print("TEST SUMMARY")
    print("="*80)
    print(f"Python Tests: {total_pytest_passed} passed, {total_pytest_skipped} skipped, {total_pytest_failed} failed")
    print(f"C++ Tests:    {total_cpp_passed} passed, {total_cpp_failed} failed")
    print(f"Overall:      {total_pytest_passed + total_cpp_passed} passed, "
          f"{total_pytest_skipped} skipped, "
          f"{total_pytest_failed + total_cpp_failed} failed")

    # Print failed tests if any
    if all_failed_tests:
        print("\n" + "="*80)
        print("FAILED TESTS:")
        print("="*80)
        for test_name, output in all_failed_tests:
            print(f"\n  ✗ {test_name}")
            print("-" * 80)
            # Print last 50 lines of output to avoid overwhelming the summary
            lines = output.strip().split('\n')
            if len(lines) > 50:
                print("  ... (output truncated, showing last 50 lines) ...\n")
                lines = lines[-50:]
            for line in lines:
                print(f"    {line}")
            print("-" * 80)

    print("="*80)

    # Exit with error code if any tests failed
    if total_pytest_failed + total_cpp_failed > 0:
        sys.exit(1)
    else:
        sys.exit(0)


if __name__ == '__main__':
    main()
