#!/bin/bash

# Recursive commit and push script - follows format_and_commit_manager pattern
# Skips nvrhi as requested

set -e  # Exit on error

PROJECT_ROOT="/c/Users/Pengfei/WorkSpace/Ruzino"
cd "$PROJECT_ROOT"

echo "========================================"
echo "Recursive Commit and Push"
echo "========================================"
echo "Project root: $PROJECT_ROOT"
echo "Skipping nvrhi submodule as requested"
echo "Processing in depth-first order (submodules first, then root)"
echo ""

# Function to process a repository
process_repo() {
    local repo_path="$1"
    local repo_name="$2"

    echo "========================================"
    echo "Processing: $repo_name"
    echo "Path: $repo_path"
    echo "========================================"

    cd "$repo_path"

    # Check if there are changes
    if ! git status --porcelain | grep -q .; then
        echo "No changes detected. Skipping."
        cd "$PROJECT_ROOT"
        return
    fi

    # Show changes
    echo ""
    echo "Changes:"
    git status --short | head -10
    if [ $(git status --short | wc -l) -gt 10 ]; then
        echo "  ... and $(( $(git status --short | wc -l) - 10 )) more"
    fi

    # Stage all changes
    echo ""
    echo "Staging all changes..."
    git add -A

    # Commit with consistent message
    local commit_msg="Update: recursive commit and push following format_and_commit_manager pattern"
    echo ""
    echo "Committing with message: '$commit_msg'"
    if ! git commit -m "$commit_msg"; then
        echo "✗ Failed to commit or nothing to commit"
        cd "$PROJECT_ROOT"
        return
    fi

    echo "✓ Committed successfully"

    # Push to remote
    echo ""
    echo "Pushing to remote..."
    if git push; then
        echo "✓ Pushed successfully"
    else
        echo "✗ Push failed (changes are committed but not pushed)"
    fi

    cd "$PROJECT_ROOT"
    echo ""
}

# Process repositories in depth-first order (submodules first, then root)

# 1. Process source/Core/rznode
process_repo "$PROJECT_ROOT/source/Core/rznode" "source/Core/rznode"

# 2. Process source/Editor/geometry
process_repo "$PROJECT_ROOT/source/Editor/geometry" "source/Editor/geometry"

# 3. Process root repository (last)
process_repo "$PROJECT_ROOT" "(root)"

echo "========================================"
echo "SUMMARY"
echo "========================================"
echo "Recursive commit and push completed"
echo "Skipped nvrhi submodule as requested"
echo "========================================"

echo ""
echo "✓ Done!"
