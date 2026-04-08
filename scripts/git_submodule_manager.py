#!/usr/bin/env python3
"""
Git Submodule Manager - Interactive tool for managing modified submodules

This script:
1. Recursively finds all Git repositories (including submodules) with changes
2. Shows modified files and asks for confirmation before committing
3. Handles remote branch selection for repositories without default remote
4. Commits and pushes changes interactively

Usage:
    python git_submodule_manager.py
"""

import os
import sys
import subprocess
from pathlib import Path
from typing import List, Tuple, Optional


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
    # This ensures submodule commits are done before parent repo commits
    git_repos.sort(key=lambda p: (len(p.parts), str(p)), reverse=True)

    return git_repos


def get_repo_status(repo_path: Path) -> Tuple[bool, List[str], List[str], List[str]]:
    """
    Get the status of a git repository.

    Returns:
        Tuple of (has_changes, modified_files, untracked_files, staged_files)
    """
    # Get modified and staged files
    result = run_git_command(repo_path, 'status', '--porcelain')

    if result.returncode != 0:
        return False, [], [], []

    modified_files = []
    untracked_files = []
    staged_files = []

    for line in result.stdout.strip().split('\n'):
        if not line:
            continue

        status = line[:2]
        filename = line[3:]

        # Check if staged (first character is not space)
        if status[0] != ' ' and status[0] != '?':
            staged_files.append(f"{status} {filename}")

        # Check if modified (second character indicates working tree status)
        if status[1] == 'M':
            modified_files.append(filename)
        elif status == '??':
            untracked_files.append(filename)
        elif status[0] == 'M' and status[1] == ' ':
            staged_files.append(f"{status} {filename}")

    has_changes = bool(modified_files or untracked_files or staged_files)
    return has_changes, modified_files, untracked_files, staged_files


def get_current_branch(repo_path: Path) -> Optional[str]:
    """Get the current branch name."""
    result = run_git_command(repo_path, 'rev-parse', '--abbrev-ref', 'HEAD')
    if result.returncode == 0:
        branch = result.stdout.strip()
        return branch if branch != 'HEAD' else None
    return None


def get_remote_branches(repo_path: Path) -> List[str]:
    """Get list of remote branches."""
    # First, fetch from all remotes
    run_git_command(repo_path, 'fetch', '--all', capture_output=False)

    result = run_git_command(repo_path, 'branch', '-r')
    if result.returncode == 0:
        branches = []
        for line in result.stdout.strip().split('\n'):
            branch = line.strip()
            if branch and '->' not in branch:  # Skip HEAD references
                branches.append(branch)
        return branches
    return []


def has_remote_tracking(repo_path: Path) -> bool:
    """Check if current branch has a remote tracking branch."""
    result = run_git_command(repo_path, 'rev-parse', '--abbrev-ref', '@{upstream}')
    return result.returncode == 0


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


def process_repository(repo_path: Path, root_dir: Path) -> bool:
    """
    Process a single repository interactively.

    Returns:
        True if changes were committed and pushed, False otherwise
    """
    # Get relative path for display
    try:
        rel_path = repo_path.relative_to(root_dir)
        display_path = str(rel_path) if str(rel_path) != '.' else '(root)'
    except ValueError:
        display_path = str(repo_path)

    print("\n" + "="*80)
    print(f"Repository: {display_path}")
    print(f"Path: {repo_path}")
    print("="*80)

    # Get status
    has_changes, modified, untracked, staged = get_repo_status(repo_path)

    if not has_changes:
        print("No changes detected.")
        return False

    # Show changes
    print("\nChanges detected:")
    if staged:
        print(f"\n  Staged files ({len(staged)}):")
        for file in staged:
            print(f"    {file}")

    if modified:
        print(f"\n  Modified files ({len(modified)}):")
        for file in modified:
            print(f"    M {file}")

    if untracked:
        print(f"\n  Untracked files ({len(untracked)}):")
        for file in untracked:
            print(f"    ?? {file}")

    # Ask if user wants to stage and commit
    if not get_user_confirmation("\nDo you want to add and commit these changes?"):
        print("Skipping this repository.")
        return False

    # Stage all changes
    print("\nStaging all changes...")
    result = run_git_command(repo_path, 'add', '-A', capture_output=False)
    if result.returncode != 0:
        print("Failed to stage changes.")
        return False

    # Get commit message
    print("\nEnter commit message (or press Enter for default message):")
    commit_msg = input("> ").strip()
    if not commit_msg:
        commit_msg = "Update: automated commit via git_submodule_manager"

    # Commit
    print(f"\nCommitting with message: '{commit_msg}'")
    result = run_git_command(repo_path, 'commit', '-m', commit_msg, capture_output=False)
    if result.returncode != 0:
        print("Failed to commit changes.")
        return False

    print("✓ Changes committed successfully")

    # Check for remote tracking
    current_branch = get_current_branch(repo_path)
    if not current_branch:
        print("Warning: Not on a branch (detached HEAD). Cannot push.")
        return False

    print(f"\nCurrent branch: {current_branch}")

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
                    result = run_git_command(repo_path, 'remote', 'add', remote_name, remote_url)
                    if result.returncode == 0:
                        print(f"✓ Remote '{remote_name}' added")

                        # Ask for push with upstream
                        branch_name = input(f"Enter branch name to push to (default: {current_branch}): ").strip()
                        if not branch_name:
                            branch_name = current_branch

                        if get_user_confirmation(f"Push to {remote_name}/{branch_name}?"):
                            result = run_git_command(
                                repo_path, 'push', '-u', remote_name, f"{current_branch}:{branch_name}",
                                capture_output=False
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
        parts = selected.split('/', 1)
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
            repo_path, 'push', '-u', remote_name, f"{current_branch}:{remote_branch}",
            capture_output=False
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
            result = run_git_command(repo_path, 'push', capture_output=False)

            if result.returncode == 0:
                print("✓ Changes pushed successfully")
                return True
            else:
                print("✗ Push failed")
                return False
        else:
            print("Push skipped.")
            return False


def main():
    """Main function."""
    print("Git Submodule Manager")
    print("=" * 80)

    # Get script directory and go up to project root
    script_dir = Path(__file__).parent
    project_root = script_dir.parent

    print(f"Scanning for Git repositories in: {project_root}")
    print("This may take a moment...\n")

    # Find all git repositories
    repos = find_all_git_repos(project_root)
    print(f"Found {len(repos)} Git repositories\n")

    # Filter repositories with changes
    repos_with_changes = []
    for repo in repos:
        has_changes, _, _, _ = get_repo_status(repo)
        if has_changes:
            try:
                rel_path = repo.relative_to(project_root)
                display_path = str(rel_path) if str(rel_path) != '.' else '(root)'
            except ValueError:
                display_path = str(repo)
            repos_with_changes.append((repo, display_path))

    if not repos_with_changes:
        print("No repositories with changes found.")
        return

    print(f"Found {len(repos_with_changes)} repositories with changes:")
    for _, display_path in repos_with_changes:
        print(f"  • {display_path}")

    if not get_user_confirmation(f"\nProcess these {len(repos_with_changes)} repositories?"):
        print("Operation cancelled.")
        return

    # Process each repository
    processed = 0
    pushed = 0

    for repo, display_path in repos_with_changes:
        if process_repository(repo, project_root):
            pushed += 1
        processed += 1

    # Print summary
    print("\n" + "="*80)
    print("SUMMARY")
    print("="*80)
    print(f"Repositories processed: {processed}")
    print(f"Repositories pushed: {pushed}")
    print(f"Repositories skipped: {processed - pushed}")
    print("="*80)


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
