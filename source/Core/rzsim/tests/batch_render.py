#!/usr/bin/env python3
"""
Batch render script for processing multiple OBJ files.

For each OBJ file in Assets/obj/:
1. Copy it to Assets/stuffedtoy.obj
2. Run headless_render.exe with the specified parameters
3. Convert the rendered frames to a video
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def find_obj_files(obj_dir):
    """Find all .obj files in the given directory."""
    obj_path = Path(obj_dir)
    if not obj_path.exists():
        print(f"Error: Directory {obj_dir} does not exist")
        return []

    obj_files = list(obj_path.glob("*.obj"))
    return sorted(obj_files)


def run_headless_render(
    exe_path,
    usd_file,
    json_file,
    output_pattern,
    width,
    height,
    samples,
    frames,
    verbose=False
):
    """Run headless_render.exe with the specified parameters."""
    cmd = [
        exe_path,
        '-u', usd_file,
        '-j', json_file,
        '-o', output_pattern,
        '-w', str(width),
        '-h', str(height),
        '-s', str(samples),
        '-f', str(frames),
        '-v',
        '-p'
    ]

    if verbose:
        print(f"Running: {' '.join(cmd)}")

    try:
        result = subprocess.run(
            cmd,
            stdout=subprocess.PIPE if not verbose else None,
            stderr=subprocess.STDOUT,
            text=True,
            check=False
        )

        if result.returncode != 0:
            print(f"Error: headless_render failed with code {result.returncode}")
            if not verbose and result.stdout:
                print(result.stdout)
            return False

        return True

    except Exception as e:
        print(f"Error running headless_render: {e}")
        return False


def convert_frames_to_video(
    output_dir,
    video_name,
    fps=60,
    crf=18,
    verbose=False
):
    """Convert rendered frames to video using to_video.py."""
    to_video_script = Path(__file__).parent / "to_video.py"

    if not to_video_script.exists():
        print(f"Error: to_video.py not found at {to_video_script}")
        return False

    video_path = Path(output_dir) / video_name

    cmd = [
        sys.executable,
        str(to_video_script),
        '-i', str(output_dir),
        '-o', str(video_path),
        '-f', str(fps),
        '-c', str(crf)
    ]

    if verbose:
        cmd.append('-v')
        print(f"Running: {' '.join(cmd)}")

    try:
        result = subprocess.run(
            cmd,
            check=False
        )

        return result.returncode == 0

    except Exception as e:
        print(f"Error converting to video: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(
        description='Batch render multiple OBJ files',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )

    parser.add_argument(
        '--obj-dir',
        help='Directory containing OBJ files',
        default='../../Assets/obj'
    )

    parser.add_argument(
        '--assets-dir',
        help='Assets directory (where stuffedtoy.obj will be replaced)',
        default='../../Assets'
    )

    parser.add_argument(
        '--output-root',
        help='Root output directory',
        default='output'
    )

    parser.add_argument(
        '--exe',
        help='Path to headless_render.exe',
        default='./headless_render.exe'
    )

    parser.add_argument(
        '-u', '--usd',
        help='USD file path',
        default='../../Assets/test_deducer_imported.usdc'
    )

    parser.add_argument(
        '-j', '--json',
        help='Render nodes JSON file',
        default='../../Assets/Hd_RUZINO_RendererPlugin/render_nodes_save.json'
    )

    parser.add_argument(
        '-w', '--width',
        type=int,
        help='Render width',
        default=1000
    )

    parser.add_argument(
        '--height',
        type=int,
        help='Render height',
        default=1000
    )

    parser.add_argument(
        '-s', '--samples',
        type=int,
        help='Samples per pixel',
        default=64
    )

    parser.add_argument(
        '-f', '--frames',
        type=int,
        help='Number of frames to render',
        default=10
    )

    parser.add_argument(
        '--fps',
        type=float,
        help='Video frames per second',
        default=60.0
    )

    parser.add_argument(
        '--crf',
        type=int,
        help='Video quality (CRF)',
        default=18
    )

    parser.add_argument(
        '-v', '--verbose',
        action='store_true',
        help='Enable verbose output'
    )

    parser.add_argument(
        '--backup',
        action='store_true',
        help='Backup original stuffedtoy.obj before processing'
    )

    args = parser.parse_args()

    # Find all OBJ files
    obj_files = find_obj_files(args.obj_dir)

    if not obj_files:
        print(f"No OBJ files found in {args.obj_dir}")
        return 1

    print(f"Found {len(obj_files)} OBJ file(s) to process:")
    for obj_file in obj_files:
        print(f"  - {obj_file.name}")
    print()

    # Path to stuffedtoy.obj
    target_obj = Path(args.assets_dir) / "stuffedtoy.obj"
    backup_obj = Path(args.assets_dir) / "stuffedtoy.obj.backup"

    # Backup original file if requested
    if args.backup and target_obj.exists():
        print(f"Backing up original {target_obj} to {backup_obj}")
        shutil.copy2(target_obj, backup_obj)

    # Process each OBJ file
    success_count = 0
    failed_count = 0

    for i, obj_file in enumerate(obj_files, 1):
        obj_name = obj_file.stem  # Filename without extension

        print(f"\n{'='*60}")
        print(f"Processing {i}/{len(obj_files)}: {obj_name}")
        print(f"{'='*60}")

        # Create output directory
        output_dir = Path(args.output_root) / obj_name
        output_dir.mkdir(parents=True, exist_ok=True)

        # Copy OBJ file to stuffedtoy.obj
        print(f"Copying {obj_file} to {target_obj}")
        try:
            shutil.copy2(obj_file, target_obj)
        except Exception as e:
            print(f"Error copying file: {e}")
            failed_count += 1
            continue

        # Render frames
        output_pattern = str(output_dir / "frame.png")
        print(f"Rendering {args.frames} frames to {output_dir}/")

        render_success = run_headless_render(
            exe_path=args.exe,
            usd_file=args.usd,
            json_file=args.json,
            output_pattern=output_pattern,
            width=args.width,
            height=args.height,
            samples=args.samples,
            frames=args.frames,
            verbose=args.verbose
        )

        if not render_success:
            print(f"Failed to render {obj_name}")
            failed_count += 1
            continue

        # Convert to video
        video_name = f"{obj_name}.mp4"
        print(f"Converting frames to video: {video_name}")

        video_success = convert_frames_to_video(
            output_dir=str(output_dir),
            video_name=video_name,
            fps=args.fps,
            crf=args.crf,
            verbose=args.verbose
        )

        if video_success:
            print(f"✓ Successfully processed {obj_name}")
            success_count += 1
        else:
            print(f"✗ Failed to create video for {obj_name}")
            failed_count += 1

    # Restore backup if it exists
    if args.backup and backup_obj.exists():
        print(f"\nRestoring original stuffedtoy.obj from backup")
        shutil.copy2(backup_obj, target_obj)
        backup_obj.unlink()

    # Summary
    print(f"\n{'='*60}")
    print(f"Batch processing complete!")
    print(f"  Successful: {success_count}/{len(obj_files)}")
    print(f"  Failed: {failed_count}/{len(obj_files)}")
    print(f"{'='*60}")

    return 0 if failed_count == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
