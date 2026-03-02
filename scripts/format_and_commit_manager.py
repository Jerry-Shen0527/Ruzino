#!/usr/bin/env python3
"""
Format and Commit Manager - Interactive tool for formatting and committing C/C++ files

This script:
1. Recursively finds all Git repositories (including submodules)
2. For each repository with changes, finds modified C/C++ files
3. Asks whether to format these files with clang-format
4. After formatting (or skipping), asks whether to commit and push ALL changes
5. Handles remote branch selection for repositories without default remote

Usage:
    # Interactive mode (default)
    python format_and_commit_manager.py

    # Non-interactive mode with commit message
    python format_and_commit_manager.py --non-interactive --message "Your commit message"
    python format_and_commit_manager.py -n -m "Your commit message"

    # Auto-checkout main branches
    python format_and_commit_manager.py -n -m "message" --auto-checkout-main
"""

import os
import sys
import subprocess
import argparse
from pathlib import Path
from typing import List, Set, Optional, Tuple


# C/C++ file extensions to format
CPP_EXTENSIONS = {
    ".cpp",
    ".h",
    ".hpp",
    ".cc",
    ".cxx",
    ".c",
    ".hxx",
    ".inl",
    ".cu",
    ".cuh",
}


def run_git_command(
    cwd: Path, *args, capture_output=True, check=False
) -> subprocess.CompletedProcess:
    """Run a git command in the specified directory."""
    cmd = ["git"] + list(args)
    try:
        result = subprocess.run(
            cmd, cwd=cwd, capture_output=capture_output, text=True, check=check
        )
        return result
    except subprocess.CalledProcessError as e:
        print(f"Error running git command: {' '.join(cmd)}")
        print(f"Error: {e}")
        return e


def is_git_repo(path: Path) -> bool:
    """Check if the path is a git repository."""
    return (path / ".git").exists() or (path / ".git").is_file()


def find_all_git_repos(root_dir: Path) -> List[Path]:
    """Recursively find all git repositories including the root.

    Returns repositories sorted by depth (deepest first), so submodules
    are processed before their parent repositories.
    Skips repositories named 'nvrhi'.
    """
    git_repos = []

    # Check if root is a git repo
    if is_git_repo(root_dir):
        git_repos.append(root_dir)

    # Walk through all subdirectories
    for dirpath, dirnames, _ in os.walk(root_dir):
        # Skip .git directories and nvrhi directories
        dirnames[:] = [d for d in dirnames if d != ".git" and d != "nvrhi"]

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
    args = ["clang-format"]
    if not dry_run:
        args.append("-i")  # In-place edit
    else:
        args.extend(["--dry-run", "--Werror"])  # Check mode

    args.append(str(file_path))

    try:
        result = subprocess.run(args, capture_output=True, text=True, check=False)
        return result.returncode == 0
    except FileNotFoundError:
        print("ERROR: clang-format not found in PATH")
        print("Please install clang-format or ensure it's in your PATH")
        return False


def has_changes(repo_path: Path) -> bool:
    """
    Check if repository has any changes (including untracked files and submodule changes).

    Returns:
        True if there are any changes
    """
    result = run_git_command(repo_path, "status", "--porcelain")
    return result.returncode == 0 and bool(result.stdout.strip())


def get_modified_files(repo_path: Path) -> Set[Path]:
    """
    Get all modified files in the git repository.

    Returns:
        Set of Path objects for modified files (both staged and unstaged)
        Note: Submodule changes appear as directories and are excluded
    """
    modified_files = set()

    # Get all modified files (staged and unstaged)
    result = run_git_command(repo_path, "status", "--porcelain")

    if result.returncode != 0:
        return modified_files

    for line in result.stdout.strip().split("\n"):
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
        if "D" in status:
            continue

        # Handle renamed files (format: "R  old -> new")
        if "R" in status and " -> " in filename:
            filename = filename.split(" -> ")[1]

        # Normalize path separators for Windows
        filename = filename.replace("/", os.sep)

        file_path = repo_path / filename

        # Include file if it exists as a file
        # Note: submodule changes will appear as directories, so they're excluded
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
        if response in ["y", "yes"]:
            return True
        elif response in ["n", "no"]:
            return False
        else:
            print("Please enter 'y' or 'n'")


def select_files_to_format(files: List[Path], repo_path: Path) -> Optional[List[Path]]:
    """
    Show files and let user select which ones to format.

    Returns:
        List of files to format, or None if cancelled
    """
    print("\nFormat options:")
    print("  1. Format all C/C++ files")
    print("  2. Select files individually")
    print("  0. Skip formatting (continue to commit)")

    while True:
        choice = input("\nEnter option: ").strip()

        if choice == "0":
            return None
        elif choice == "1":
            return files
        elif choice == "2":
            selected = []
            print("\nSelect files to format:")

            for i, file_path in enumerate(files, 1):
                try:
                    rel_path = file_path.relative_to(repo_path)
                except ValueError:
                    rel_path = file_path

                while True:
                    response = (
                        input(f"  {i}. {rel_path} - Format? (y/n): ").lower().strip()
                    )

                    if response in ["y", "yes"]:
                        selected.append(file_path)
                        break
                    elif response in ["n", "no"]:
                        break
                    else:
                        print("    Please enter 'y' or 'n'")

            return selected if selected else None
        else:
            print("Please enter 0, 1, or 2")


def get_current_branch(repo_path: Path) -> Optional[str]:
    """Get the current branch name."""
    result = run_git_command(repo_path, "rev-parse", "--abbrev-ref", "HEAD")
    if result.returncode == 0:
        branch = result.stdout.strip()
        return branch if branch != "HEAD" else None
    return None


def get_remote_branches(repo_path: Path) -> List[str]:
    """Get list of remote branches."""
    # First, fetch from all remotes
    run_git_command(repo_path, "fetch", "--all", capture_output=False)

    result = run_git_command(repo_path, "branch", "-r")
    if result.returncode == 0:
        branches = []
        for line in result.stdout.strip().split("\n"):
            branch = line.strip()
            if branch and "->" not in branch:  # Skip HEAD references
                branches.append(branch)
        return branches
    return []


def has_remote_tracking(repo_path: Path) -> bool:
    """Check if current branch has a remote tracking branch."""
    result = run_git_command(repo_path, "rev-parse", "--abbrev-ref", "@{upstream}")
    return result.returncode == 0


def select_from_list(items: List[str], prompt: str) -> Optional[str]:
    """Let user select an item from a list."""
    if not items:
        return None

    print(f"\n{prompt}")
    for i, item in enumerate(items, 1):
        print(f"  {i}. {item}")
    print(f"  0. Cancel")

    while True:
        try:
            choice = input("\nEnter number: ").strip()
            choice_num = int(choice)

            if choice_num == 0:
                return None
            elif 1 <= choice_num <= len(items):
                return items[choice_num - 1]
            else:
                print(f"Please enter a number between 0 and {len(items)}")
        except ValueError:
            print("Please enter a valid number")


def ensure_on_main_branch(repo_path: Path, auto_checkout: bool = True) -> bool:
    """
    Ensure repository is on main branch (or master if main doesn't exist).

    Args:
        repo_path: Path to the repository
        auto_checkout: If True, automatically checkout main/master branch

    Returns:
        True if on main/master branch, False otherwise
    """
    current_branch = get_current_branch(repo_path)

    # Already on main or master
    if current_branch in ["main", "master"]:
        return True

    if not auto_checkout:
        return False

    # Try to checkout main first, then master
    for branch in ["main", "master"]:
        result = run_git_command(repo_path, "checkout", branch, capture_output=True)
        if result.returncode == 0:
            print(f"✓ Switched to {branch} branch")
            return True

    print(f"✗ Could not checkout main or master branch")
    return False


def quick_commit_and_push(repo_path: Path, commit_msg: str) -> bool:
    """
    Quickly commit and push all changes without confirmations.

    Args:
        repo_path: Path to the repository
        commit_msg: Commit message to use

    Returns:
        True if successfully pushed, False otherwise
    """
    # Check for remote tracking
    current_branch = get_current_branch(repo_path)
    if not current_branch:
        print("Warning: Not on a branch (detached HEAD). Cannot push.")
        return False

    # Stage all changes
    result = run_git_command(repo_path, "add", "-A", capture_output=True)
    if result.returncode != 0:
        print("Failed to stage changes.")
        return False

    # Commit
    result = run_git_command(repo_path, "commit", "-m", commit_msg, capture_output=True)
    if result.returncode != 0:
        print("Failed to commit changes.")
        return False

    print(f"✓ Committed: {commit_msg}")

    # Check if we have a remote tracking branch
    if not has_remote_tracking(repo_path):
        print("Warning: No remote tracking branch. Skipping push.")
        return False

    # Push
    result = run_git_command(repo_path, "push", capture_output=True)
    if result.returncode == 0:
        print("✓ Pushed to remote")
        return True
    else:
        print("✗ Push failed")
        return False


def commit_and_push_all_changes(repo_path: Path) -> bool:
    """
    Commit and push all changes in the repository.

    Returns:
        True if successfully pushed, False otherwise
    """
    # Check for remote tracking
    current_branch = get_current_branch(repo_path)
    if not current_branch:
        print("Warning: Not on a branch (detached HEAD). Cannot push.")
        return False

    print(f"\nCurrent branch: {current_branch}")

    # Ask if user wants to commit
    if not get_user_confirmation(
        "\nDo you want to commit all changes in this repository?"
    ):
        print("Skipping this repository.")
        return False

    # Stage all changes
    print("\nStaging all changes...")
    result = run_git_command(repo_path, "add", "-A", capture_output=False)
    if result.returncode != 0:
        print("Failed to stage changes.")
        return False

    # Get commit message
    print("\nEnter commit message (or press Enter for default message):")
    commit_msg = input("> ").strip()
    if not commit_msg:
        commit_msg = "Update: format C/C++ files and commit changes"

    # Commit
    print(f"\nCommitting with message: '{commit_msg}'")
    result = run_git_command(
        repo_path, "commit", "-m", commit_msg, capture_output=False
    )
    if result.returncode != 0:
        print("Failed to commit changes.")
        return False

    print("✓ Changes committed successfully")

    # Check if we have a remote tracking branch
    if not has_remote_tracking(repo_path):
        print("\nNo remote tracking branch configured.")

        # Get available remote branches
        remote_branches = get_remote_branches(repo_path)

        if not remote_branches:
            print("No remote branches found. Cannot push.")

            # Ask if user wants to set up a remote
            if get_user_confirmation("Do you want to set up a remote?"):
                remote_name = input("Enter remote name (e.g., 'origin'): ").strip()
                remote_url = input("Enter remote URL: ").strip()

                if remote_name and remote_url:
                    result = run_git_command(
                        repo_path, "remote", "add", remote_name, remote_url
                    )
                    if result.returncode == 0:
                        print(f"✓ Remote '{remote_name}' added")

                        # Ask for push with upstream
                        branch_name = input(
                            f"Enter branch name to push to (default: {current_branch}): "
                        ).strip()
                        if not branch_name:
                            branch_name = current_branch

                        if get_user_confirmation(
                            f"Push to {remote_name}/{branch_name}?"
                        ):
                            result = run_git_command(
                                repo_path,
                                "push",
                                "-u",
                                remote_name,
                                f"{current_branch}:{branch_name}",
                                capture_output=False,
                            )
                            if result.returncode == 0:
                                print("✓ Changes pushed successfully")
                                return True
            return False

        # Let user select a remote branch
        selected = select_from_list(remote_branches, "Select remote branch to push to:")
        if not selected:
            print("Push cancelled.")
            return False

        # Parse remote and branch from selection (e.g., "origin/main" -> "origin", "main")
        parts = selected.split("/", 1)
        if len(parts) != 2:
            print("Invalid remote branch format.")
            return False

        remote_name, remote_branch = parts

        # Ask for confirmation
        if not get_user_confirmation(f"Set upstream to {selected} and push?"):
            print("Push cancelled.")
            return False

        # Push with upstream
        print(f"\nPushing to {selected}...")
        result = run_git_command(
            repo_path,
            "push",
            "-u",
            remote_name,
            f"{current_branch}:{remote_branch}",
            capture_output=False,
        )

        if result.returncode == 0:
            print("✓ Changes pushed successfully")
            return True
        else:
            print("✗ Push failed")
            return False
    else:
        # We have a tracking branch, just push
        if get_user_confirmation("Do you want to push to the remote?"):
            print("\nPushing changes...")
            result = run_git_command(repo_path, "push", capture_output=False)

            if result.returncode == 0:
                print("✓ Changes pushed successfully")
                return True
            else:
                print("✗ Push failed")
                return False
        else:
            print("Push skipped.")
            return False


def process_repository_quick(
    repo_path: Path, project_root: Path
) -> Tuple[int, int, bool]:
    """
    Process a single repository in quick mode - format all C/C++ files and commit.

    Returns:
        Tuple of (files_formatted, files_failed, committed_and_pushed)
    """
    # Get relative path for display
    try:
        rel_path = repo_path.relative_to(project_root)
        display_path = str(rel_path) if str(rel_path) != "." else "(root)"
    except ValueError:
        display_path = str(repo_path)

    print("\n" + "=" * 80)
    print(f"Repository: {display_path}")
    print("=" * 80)

    # Get modified files
    modified_files = get_modified_files(repo_path)
    cpp_files = filter_cpp_files(modified_files)

    success_count = 0
    error_count = 0

    # Format all C/C++ files automatically
    if cpp_files:
        print(f"Formatting {len(cpp_files)} C/C++ files...")
        for file_path in cpp_files:
            if run_clang_format(file_path, dry_run=False):
                success_count += 1
            else:
                error_count += 1

        if success_count > 0:
            print(f"✓ Formatted {success_count} files")
        if error_count > 0:
            print(f"✗ Failed to format {error_count} files")

    # Get commit message
    print(f"\nEnter commit message for {display_path}:")
    commit_msg = input("> ").strip()

    if not commit_msg:
        print("Empty commit message. Skipping repository.")
        return success_count, error_count, False

    # Commit and push
    pushed = quick_commit_and_push(repo_path, commit_msg)

    return success_count, error_count, pushed


def process_repository(repo_path: Path, project_root: Path) -> Tuple[int, int, bool]:
    """
    Process a single repository interactively.

    Returns:
        Tuple of (files_formatted, files_failed, committed_and_pushed)
    """
    # Get relative path for display
    try:
        rel_path = repo_path.relative_to(project_root)
        display_path = str(rel_path) if str(rel_path) != "." else "(root)"
    except ValueError:
        display_path = str(repo_path)

    print("\n" + "=" * 80)
    print(f"Repository: {display_path}")
    print(f"Path: {repo_path}")
    print("=" * 80)

    # Check if repository has any changes
    if not has_changes(repo_path):
        print("No changes detected.")
        return 0, 0, False

    # Get modified files (actual files, not submodules)
    modified_files = get_modified_files(repo_path)

    # Show all modified files (or note about submodules if no files)
    if modified_files:
        print(f"\nFound {len(modified_files)} modified files:")
        for file_path in list(modified_files)[:10]:  # Show first 10
            try:
                rel_path = file_path.relative_to(repo_path)
            except ValueError:
                rel_path = file_path
            print(f"  • {rel_path}")
        if len(modified_files) > 10:
            print(f"  ... and {len(modified_files) - 10} more")
    else:
        print("\nNo modified files detected (may have submodule changes only).")

    # Filter C/C++ files
    cpp_files = filter_cpp_files(modified_files)

    success_count = 0
    error_count = 0

    # If there are C/C++ files, ask about formatting
    if cpp_files:
        print(f"\nFound {len(cpp_files)} C/C++ files that can be formatted:")
        for file_path in cpp_files:
            try:
                rel_path = file_path.relative_to(repo_path)
            except ValueError:
                rel_path = file_path
            print(f"  • {rel_path}")

        # Select files to format
        files_to_format = select_files_to_format(cpp_files, repo_path)

        if files_to_format:
            # Format files
            print(f"\nFormatting {len(files_to_format)} files...")
            print("-" * 80)

            for file_path in files_to_format:
                try:
                    rel_path = file_path.relative_to(repo_path)
                except ValueError:
                    rel_path = file_path

                print(f"  {rel_path}... ", end="", flush=True)

                if run_clang_format(file_path, dry_run=False):
                    print("✓")
                    success_count += 1
                else:
                    print("✗")
                    error_count += 1

            print("-" * 80)

            if success_count > 0:
                print(f"✓ Successfully formatted {success_count} files")
            if error_count > 0:
                print(f"✗ Failed to format {error_count} files")
        else:
            print("\nSkipping format step.")

    # Ask about committing ALL changes (not just formatted files)
    pushed = commit_and_push_all_changes(repo_path)

    return success_count, error_count, pushed


def process_repository_non_interactive(
    repo_path: Path,
    project_root: Path,
    commit_msg: str,
    auto_checkout_main: bool = False,
) -> Tuple[int, int, bool]:
    """
    Process a repository in non-interactive mode.

    Args:
        repo_path: Path to the repository
        project_root: Root of the project
        commit_msg: Commit message to use
        auto_checkout_main: If True, automatically checkout main branch before committing

    Returns:
        Tuple of (files_formatted, files_failed, committed_and_pushed)
    """
    # Get relative path for display
    try:
        rel_path = repo_path.relative_to(project_root)
        display_path = str(rel_path) if str(rel_path) != "." else "(root)"
    except ValueError:
        display_path = str(repo_path)

    print(f"\n{'=' * 80}")
    print(f"Repository: {display_path}")
    print(f"Path: {repo_path}")
    print("=" * 80)

    # Check if repository has any changes
    if not has_changes(repo_path):
        print("No changes detected.")
        return 0, 0, False

    # Auto-checkout main branch if requested (only for submodules, not root repo)
    is_root_repo = repo_path == project_root
    if auto_checkout_main and not is_root_repo:
        if not ensure_on_main_branch(repo_path):
            print("✗ Could not checkout main branch. Skipping")
            return 0, 0, False

    # Get modified files (actual files, not submodules)
    modified_files = get_modified_files(repo_path)

    # Show all modified files
    if modified_files:
        print(f"\nFound {len(modified_files)} modified files:")
        for file_path in list(modified_files)[:10]:  # Show first 10
            try:
                rel_path = file_path.relative_to(repo_path)
            except ValueError:
                rel_path = file_path
            print(f"  • {rel_path}")
        if len(modified_files) > 10:
            print(f"  ... and {len(modified_files) - 10} more")
    else:
        print("\nNo modified files detected (may have submodule changes only).")

    # Filter C/C++ files
    cpp_files = filter_cpp_files(modified_files)

    success_count = 0
    error_count = 0

    # Format all C/C++ files automatically
    if cpp_files:
        print(f"Formatting {len(cpp_files)} C/C++ files...")
        for file_path in cpp_files:
            if run_clang_format(file_path, dry_run=False):
                success_count += 1
            else:
                error_count += 1

        if success_count > 0:
            print(f"✓ Formatted {success_count} files")
        if error_count > 0:
            print(f"✗ Failed to format {error_count} files")

    # Commit and push
    pushed = quick_commit_and_push(repo_path, commit_msg)

    return success_count, error_count, pushed


def parse_args():
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(
        description="Format and Commit Manager - Manage C/C++ formatting and git commits"
    )

    parser.add_argument(
        "-n",
        "--non-interactive",
        action="store_true",
        help="Run in non-interactive mode (no prompts, commit with single message)",
    )

    parser.add_argument(
        "-m",
        "--message",
        type=str,
        help="Commit message to use for all repositories (required in non-interactive mode)",
    )

    parser.add_argument(
        "--auto-checkout-main",
        action="store_true",
        help="Automatically checkout main/master branch before committing",
    )

    return parser.parse_args()


def main_non_interactive(commit_msg: str, auto_checkout_main: bool = False):
    """
    Non-interactive mode: format and commit all repositories with a single commit message.

    Args:
        commit_msg: Commit message to use for all repositories
        auto_checkout_main: If True, automatically checkout main branch before committing
    """
    script_dir = Path(__file__).parent
    project_root = script_dir.parent

    print("=" * 80)
    print("Non-Interactive Mode")
    print("=" * 80)
    print(f"Commit message: {commit_msg}")
    if auto_checkout_main:
        print("Auto-checkout main: ENABLED")
    print()

    # Check clang-format
    print("Checking for clang-format...")
    try:
        result = subprocess.run(
            ["clang-format", "--version"], capture_output=True, text=True
        )
        if result.returncode == 0:
            print(f"✓ Found: {result.stdout.strip().split(chr(10))[0]}")
        else:
            print("✗ ERROR: clang-format not found")
            sys.exit(1)
    except FileNotFoundError:
        print("✗ ERROR: clang-format not found in PATH")
        sys.exit(1)

    # Find all repositories
    print(f"\nScanning for repositories...")
    repos = find_all_git_repos(project_root)

    # Find repos with changes
    repos_to_process = []
    for repo in repos:
        if has_changes(repo):
            repos_to_process.append(repo)

    if not repos_to_process:
        print("No repositories with changes found.")
        return

    print(f"Found {len(repos_to_process)} repositories with changes\n")

    # Process each repository
    total_success = 0
    total_error = 0
    repos_committed = 0

    for repo in repos_to_process:
        files_formatted, files_failed, pushed = process_repository_non_interactive(
            repo, project_root, commit_msg, auto_checkout_main
        )
        total_success += files_formatted
        total_error += files_failed
        if pushed:
            repos_committed += 1

    # Summary
    print("\n" + "=" * 80)
    print("SUMMARY")
    print("=" * 80)
    print(f"Repositories processed: {len(repos_to_process)}")
    print(f"Files formatted: {total_success}")
    print(f"Files with errors: {total_error}")
    print(f"Repositories committed: {repos_committed}")
    print("=" * 80)
    print("\n✓ Done!")


def main():
    """Main function."""
    args = parse_args()

    # Non-interactive mode
    if args.non_interactive:
        if not args.message:
            print("ERROR: --message (-m) is required in non-interactive mode")
            print(
                'Usage: python format_and_commit_manager.py -n -m "Your commit message"'
            )
            sys.exit(1)

        main_non_interactive(args.message, args.auto_checkout_main)
        return

    # Interactive mode
    print("Format and Commit Manager")
    print("=" * 80)

    # Get script directory and go up to project root
    script_dir = Path(__file__).parent
    project_root = script_dir.parent

    print(f"Project root: {project_root}")

    # Select mode
    print("\nSelect mode:")
    print("  1. Quick mode (auto-format, only ask for commit messages)")
    print("  2. Interactive mode (confirm each step)")

    while True:
        mode_choice = input("\nEnter mode (1 or 2): ").strip()
        if mode_choice in ["1", "2"]:
            quick_mode = mode_choice == "1"
            break
        print("Please enter 1 or 2")

    if quick_mode:
        print("\n✓ Quick mode enabled")
    else:
        print("\n✓ Interactive mode enabled")

    # Check if clang-format is available
    print("\nChecking for clang-format...")
    try:
        result = subprocess.run(
            ["clang-format", "--version"], capture_output=True, text=True, check=False
        )
        if result.returncode == 0:
            version_line = result.stdout.strip().split("\n")[0]
            print(f"✓ Found: {version_line}")
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
    clang_format_config = project_root / ".clang-format"
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

    # Find repositories with any modifications
    repos_with_changes = []
    for repo in repos:
        # Check if repository has any changes at all (including submodules)
        if not has_changes(repo):
            continue

        modified_files = get_modified_files(repo)

        try:
            rel_path = repo.relative_to(project_root)
            display_path = str(rel_path) if str(rel_path) != "." else "(root)"
        except ValueError:
            display_path = str(repo)

        cpp_files = filter_cpp_files(modified_files)
        repos_with_changes.append(
            (repo, display_path, len(modified_files), len(cpp_files))
        )

    if not repos_with_changes:
        print("No repositories with changes found.")
        return

    print(f"Found {len(repos_with_changes)} repositories with changes:")
    for _, display_path, total_files, cpp_count in repos_with_changes:
        if cpp_count > 0:
            print(f"  • {display_path} ({total_files} files, {cpp_count} C/C++)")
        else:
            print(f"  • {display_path} ({total_files} files)")

    if not quick_mode:
        if not get_user_confirmation(
            f"\nProcess these {len(repos_with_changes)} repositories?"
        ):
            print("Operation cancelled.")
            return
    else:
        print(f"\nWill process {len(repos_with_changes)} repositories in quick mode.")
        print("You will only need to provide commit messages.\n")

    # Process each repository
    total_success = 0
    total_error = 0
    repos_committed = 0

    for repo, display_path, _, _ in repos_with_changes:
        if quick_mode:
            success, error, pushed = process_repository_quick(repo, project_root)
        else:
            success, error, pushed = process_repository(repo, project_root)

        if pushed:
            repos_committed += 1

        total_success += success
        total_error += error

    # Print summary
    print("\n" + "=" * 80)
    print("SUMMARY")
    print("=" * 80)
    print(f"Repositories with changes: {len(repos_with_changes)}")
    print(f"Total files formatted successfully: {total_success}")
    print(f"Total files with format errors: {total_error}")
    print(f"Repositories committed and pushed: {repos_committed}")
    print("=" * 80)

    print("\n✓ Operation complete!")


if __name__ == "__main__":
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
