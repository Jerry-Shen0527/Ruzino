#!/usr/bin/env python3
"""
Clang Format Manager - Interactive tool for formatting modified files

This script:
1. Recursively finds all Git repositories (including submodules)
2. For each repository, finds modified files (staged and unstaged)
3. Filters C/C++ source files (.cpp, .h, .hpp, .cc, .cxx, etc.)
4. Asks for confirmation per repository before formatting
5. Runs clang-format on selected files

Usage:
    python clang_format_manager.py
"""

import os
import sys
import subprocess
from pathlib import Path
from typing import List, Set, Optional, Tuple


# C/C++ file extensions to format
CPP_EXTENSIONS = {'.cpp', '.h', '.hpp', '.cc', '.cxx', '.c', '.hxx', '.inl', '.cu', '.cuh'}


def run_git_command(cwd: Path, *args, capture_output=True, check=False) -> subprocess.CompletedProcess:
    """Run a git command in the specified directory."""
    cmd = ['git'] + list(args)
    try:
        result = subprocess.run(
            cmd,
            cwd=cwd,
            capture_output=capture_output,
            text=True,
            check=check
        )
        return result
    except subprocess.CalledProcessError as e:
        print(f"Error running git command: {' '.join(cmd)}")
        print(f"Error: {e}")
        return e


def is_git_repo(path: Path) -> bool:
    """Check if the path is a git repository."""
    return (path / '.git').exists() or (path / '.git').is_file()


def find_all_git_repos(root_dir: Path) -> List[Path]:
    """Recursively find all git repositories including the root.

    Returns repositories sorted by depth (deepest first), so submodules
    are processed before their parent repositories.
    """
    git_repos = []

    # Check if root is a git repo
    if is_git_repo(root_dir):
        git_repos.append(root_dir)

    # Walk through all subdirectories
    for dirpath, dirnames, _ in os.walk(root_dir):
        # Skip .git directories
        dirnames[:] = [d for d in dirnames if d != '.git']

        current_path = Path(dirpath)
        for dirname in dirnames:
            potential_repo = current_path / dirname
            if is_git_repo(potential_repo):
                git_repos.append(potential_repo)

    # Sort by depth (deepest first) so we process leaf repos before root
    git_repos.sort(key=lambda p: (len(p.parts), str(p)), reverse=True)

    return git_repos


def run_clang_format(file_path: Path, dry_run: bool = False) -> bool:
    """
    Run clang-format on a file.

    Args:
        file_path: Path to the file to format
        dry_run: If True, only check without modifying the file

    Returns:
        True if successful, False otherwise
    """
    args = ['clang-format']
    if not dry_run:
        args.append('-i')  # In-place edit
    else:
        args.extend(['--dry-run', '--Werror'])  # Check mode

    args.append(str(file_path))

    try:
        result = subprocess.run(
            args,
            capture_output=True,
            text=True,
            check=False
        )
        return result.returncode == 0
    except FileNotFoundError:
        print("ERROR: clang-format not found in PATH")
        print("Please install clang-format or ensure it's in your PATH")
        return False


def get_modified_files(repo_path: Path) -> Set[Path]:
    """
    Get all modified files in the git repository.

    Returns:
        Set of Path objects for modified files (both staged and unstaged)
    """
    modified_files = set()

    # Get all modified files (staged and unstaged)
    result = run_git_command(repo_path, 'status', '--porcelain')

    if result.returncode != 0:
        return modified_files

    for line in result.stdout.strip().split('\n'):
        if not line:
            continue

        # Parse git status output
        # Format: XY filename
        # X = staged status, Y = unstaged status
        # Status is 2 characters, followed by a space, then filename
        if len(line) < 3:
            continue

        status = line[:2]
        # The filename starts after the 2-char status and any following spaces
        filename = line[2:].lstrip()

        # Skip deleted files
        if 'D' in status:
            continue

        # Handle renamed files (format: "R  old -> new")
        if 'R' in status and ' -> ' in filename:
            filename = filename.split(' -> ')[1]

        # Normalize path separators for Windows
        filename = filename.replace('/', os.sep)

        file_path = repo_path / filename

        if file_path.exists() and file_path.is_file():
            modified_files.add(file_path)

    return modified_files


def filter_cpp_files(files: Set[Path]) -> List[Path]:
    """Filter and sort C/C++ files from a set of files."""
    cpp_files = [f for f in files if f.suffix.lower() in CPP_EXTENSIONS]
    cpp_files.sort()
    return cpp_files


def get_user_confirmation(prompt: str) -> bool:
    """Get yes/no confirmation from user."""
    while True:
        response = input(f"{prompt} (y/n): ").lower().strip()
        if response in ['y', 'yes']:
            return True
        elif response in ['n', 'no']:
            return False
        else:
            print("Please enter 'y' or 'n'")


def select_files_to_format(files: List[Path], repo_path: Path) -> Optional[List[Path]]:
    """
    Show files and let user select which ones to format.

    Returns:
        List of files to format, or None if cancelled
    """
    print("\nOptions:")
    print("  1. Format all files in this repository")
    print("  2. Select files individually")
    print("  0. Skip this repository")

    while True:
        choice = input("\nEnter option: ").strip()

        if choice == '0':
            return None
        elif choice == '1':
            return files
        elif choice == '2':
            selected = []
            print("\nSelect files to format:")

            for i, file_path in enumerate(files, 1):
                try:
                    rel_path = file_path.relative_to(repo_path)
                except ValueError:
                    rel_path = file_path

                while True:
                    response = input(f"  {i}. {rel_path} - Format? (y/n): ").lower().strip()

                    if response in ['y', 'yes']:
                        selected.append(file_path)
                        break
                    elif response in ['n', 'no']:
                        break
                    else:
                        print("    Please enter 'y' or 'n'")

            return selected if selected else None
        else:
            print("Please enter 0, 1, or 2")


def process_repository(repo_path: Path, project_root: Path, clang_format_available: bool) -> Tuple[int, int]:
    """
    Process a single repository interactively.

    Returns:
        Tuple of (files_formatted, files_failed)
    """
    # Get relative path for display
    try:
        rel_path = repo_path.relative_to(project_root)
        display_path = str(rel_path) if str(rel_path) != '.' else '(root)'
    except ValueError:
        display_path = str(repo_path)

    print("\n" + "="*80)
    print(f"Repository: {display_path}")
    print(f"Path: {repo_path}")
    print("="*80)

    # Get modified files
    modified_files = get_modified_files(repo_path)

    if not modified_files:
        print("No modified files in this repository.")
        return 0, 0

    # Filter C/C++ files
    cpp_files = filter_cpp_files(modified_files)

    if not cpp_files:
        print(f"Found {len(modified_files)} modified files, but no C/C++ files.")
        return 0, 0

    # Show files
    print(f"\nFound {len(cpp_files)} C/C++ files with modifications:")
    for file_path in cpp_files:
        try:
            rel_path = file_path.relative_to(repo_path)
        except ValueError:
            rel_path = file_path
        print(f"  • {rel_path}")

    # Select files to format
    files_to_format = select_files_to_format(cpp_files, repo_path)

    if not files_to_format:
        print("Skipping this repository.")
        return 0, 0

    # Format files
    print(f"\nFormatting {len(files_to_format)} files...")
    print("-" * 80)

    success_count = 0
    error_count = 0

    for file_path in files_to_format:
        try:
            rel_path = file_path.relative_to(repo_path)
        except ValueError:
            rel_path = file_path

        print(f"  {rel_path}... ", end='', flush=True)

        if run_clang_format(file_path, dry_run=False):
            print("✓")
            success_count += 1
        else:
            print("✗")
            error_count += 1

    print("-" * 80)

    if success_count > 0:
        print(f"✓ Successfully formatted {success_count} files in this repository")
    if error_count > 0:
        print(f"✗ Failed to format {error_count} files in this repository")

    return success_count, error_count


def main():
    """Main function."""
    print("Clang Format Manager")
    print("=" * 80)

    # Get script directory and go up to project root
    script_dir = Path(__file__).parent
    project_root = script_dir.parent

    print(f"Project root: {project_root}")

    # Check if clang-format is available
    print("\nChecking for clang-format...")
    clang_format_available = False
    try:
        result = subprocess.run(
            ['clang-format', '--version'],
            capture_output=True,
            text=True,
            check=False
        )
        if result.returncode == 0:
            version_line = result.stdout.strip().split('\n')[0]
            print(f"✓ Found: {version_line}")
            clang_format_available = True
        else:
            raise FileNotFoundError()
    except FileNotFoundError:
        print("✗ ERROR: clang-format not found in PATH")
        print("\nPlease install clang-format:")
        print("  • Windows: Install LLVM from https://llvm.org/builds/")
        print("  • Linux: sudo apt install clang-format")
        print("  • macOS: brew install clang-format")
        return

    # Check for .clang-format config
    clang_format_config = project_root / '.clang-format'
    if clang_format_config.exists():
        print(f"✓ Found .clang-format config")
    else:
        print(f"⚠ Warning: No .clang-format config found in project root")
        if not get_user_confirmation("Continue without config?"):
            return

    print(f"\nScanning for Git repositories in: {project_root}")
    print("This may take a moment...\n")

    # Find all git repositories
    repos = find_all_git_repos(project_root)
    print(f"Found {len(repos)} Git repositories\n")

    # Find repositories with modified C/C++ files
    repos_with_cpp_changes = []
    for repo in repos:
        try:
            rel_path = repo.relative_to(project_root)
            display_path = str(rel_path) if str(rel_path) != '.' else '(root)'
        except ValueError:
            display_path = str(repo)

        modified_files = get_modified_files(repo)
        cpp_files = filter_cpp_files(modified_files)

        if cpp_files:
            repos_with_cpp_changes.append((repo, display_path, len(cpp_files)))

    if not repos_with_cpp_changes:
        print("No repositories with modified C/C++ files found.")
        return

    print(f"Found {len(repos_with_cpp_changes)} repositories with modified C/C++ files:")
    for _, display_path, file_count in repos_with_cpp_changes:
        print(f"  • {display_path} ({file_count} files)")

    if not get_user_confirmation(f"\nProcess these {len(repos_with_cpp_changes)} repositories?"):
        print("Operation cancelled.")
        return

    # Process each repository
    total_success = 0
    total_error = 0
    repos_processed = 0

    for repo, display_path, _ in repos_with_cpp_changes:
        success, error = process_repository(repo, project_root, clang_format_available)

        if success > 0 or error > 0:
            repos_processed += 1

        total_success += success
        total_error += error

    # Print summary
    print("\n" + "="*80)
    print("SUMMARY")
    print("="*80)
    print(f"Repositories processed: {repos_processed}")
    print(f"Total files formatted successfully: {total_success}")
    print(f"Total files with errors: {total_error}")
    print("="*80)

    if total_success > 0:
        print("\n✓ Formatting complete!")
        print("Note: Files have been modified in-place.")
        print("Review the changes with 'git diff' before committing.")


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\n\nOperation cancelled by user.")
        sys.exit(1)
    except Exception as e:
        print(f"\nUnexpected error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
