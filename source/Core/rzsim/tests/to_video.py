#!/usr/bin/env python3
"""
Convert image sequence to video using FFmpeg.
Supports various input formats (PNG, EXR, HDR) and output formats.
"""

import argparse
import subprocess
import sys
import os
import glob
import re
from pathlib import Path


def find_image_sequence(directory, pattern=None):
    """
    Find image sequences in the given directory.

    Args:
        directory: Directory to search for images
        pattern: Optional pattern to match (e.g., "output_*.png")

    Returns:
        Dictionary with sequence info or None if no sequence found
    """
    if pattern:
        files = sorted(glob.glob(os.path.join(directory, pattern)))
    else:
        # Look for common image formats
        extensions = ['*.png', '*.jpg', '*.jpeg', '*.exr', '*.hdr']
        files = []
        for ext in extensions:
            files.extend(glob.glob(os.path.join(directory, ext)))
        files = sorted(files)

    if not files:
        return None

    # Try to detect sequence pattern
    first_file = os.path.basename(files[0])

    # Look for common patterns like name_0001.ext, name_001.ext, etc.
    match = re.match(r'(.+?)_(\d+)\.(\w+)$', first_file)
    if match:
        base_name = match.group(1)
        first_number = int(match.group(2))
        extension = match.group(3)
        num_digits = len(match.group(2))

        return {
            'files': files,
            'base_name': base_name,
            'extension': extension,
            'first_number': first_number,
            'num_digits': num_digits,
            'count': len(files)
        }

    # If no pattern detected, just return the files
    return {
        'files': files,
        'base_name': 'sequence',
        'extension': os.path.splitext(first_file)[1][1:],
        'first_number': 0,
        'num_digits': 4,
        'count': len(files)
    }


def check_ffmpeg():
    """Check if ffmpeg is installed and accessible."""
    try:
        result = subprocess.run(
            ['ffmpeg', '-version'],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False
        )
        return result.returncode == 0
    except FileNotFoundError:
        return False


def convert_to_video(
    input_pattern,
    output_file,
    fps=60,
    crf=18,
    preset='medium',
    codec='libx264',
    pix_fmt='yuv420p',
    start_number=0,
    verbose=False
):
    """
    Convert image sequence to video using FFmpeg.

    Args:
        input_pattern: Input file pattern (e.g., "output_%04d.png")
        output_file: Output video file path
        fps: Frames per second (default: 60)
        crf: Constant Rate Factor for quality, lower is better (default: 18)
        preset: Encoding preset (ultrafast, fast, medium, slow, veryslow)
        codec: Video codec (libx264, libx265, etc.)
        pix_fmt: Pixel format (yuv420p for compatibility)
        start_number: Starting frame number
        verbose: Enable verbose output
    """

    # Build ffmpeg command
    cmd = [
        'ffmpeg',
        '-y',  # Overwrite output file
        '-framerate', str(fps),
        '-start_number', str(start_number),
        '-i', input_pattern,
        '-c:v', codec,
        '-crf', str(crf),
        '-preset', preset,
        '-pix_fmt', pix_fmt,
    ]

    # Add extra options for HDR/EXR inputs
    if input_pattern.lower().endswith(('.exr', '.hdr')):
        # Apply tone mapping for HDR content
        cmd.extend([
            '-vf', 'zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv,format=yuv420p'
        ])

    cmd.append(output_file)

    if verbose:
        print(f"Running command: {' '.join(cmd)}")

    # Run ffmpeg
    try:
        result = subprocess.run(
            cmd,
            stdout=subprocess.PIPE if not verbose else None,
            stderr=subprocess.PIPE if not verbose else None,
            text=True,
            check=False
        )

        if result.returncode != 0:
            print(f"Error: FFmpeg failed with code {result.returncode}")
            if not verbose and result.stderr:
                print("Error output:")
                print(result.stderr)
            return False

        return True

    except Exception as e:
        print(f"Error running FFmpeg: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(
        description='Convert image sequence to video using FFmpeg',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )

    parser.add_argument(
        '-i', '--input',
        help='Input pattern (e.g., "output_%%04d.png") or directory to search',
        default='.'
    )

    parser.add_argument(
        '-o', '--output',
        help='Output video file',
        default='output.mp4'
    )

    parser.add_argument(
        '-f', '--fps',
        type=float,
        help='Frames per second',
        default=60.0
    )

    parser.add_argument(
        '-c', '--crf',
        type=int,
        help='Constant Rate Factor (quality: 0-51, lower is better)',
        default=18
    )

    parser.add_argument(
        '-p', '--preset',
        choices=['ultrafast', 'superfast', 'veryfast', 'faster', 'fast',
                 'medium', 'slow', 'slower', 'veryslow'],
        help='Encoding preset (speed vs compression)',
        default='medium'
    )

    parser.add_argument(
        '--codec',
        help='Video codec',
        default='libx264',
        choices=['libx264', 'libx265', 'libvpx-vp9', 'libaom-av1']
    )

    parser.add_argument(
        '--pix-fmt',
        help='Pixel format',
        default='yuv420p'
    )

    parser.add_argument(
        '-s', '--start-number',
        type=int,
        help='Starting frame number',
        default=0
    )

    parser.add_argument(
        '-v', '--verbose',
        action='store_true',
        help='Enable verbose output'
    )

    parser.add_argument(
        '--pattern',
        help='File pattern to search for (e.g., "frame_*.png")',
        default=None
    )

    args = parser.parse_args()

    # Check if ffmpeg is available
    if not check_ffmpeg():
        print("Error: FFmpeg not found. Please install FFmpeg and add it to PATH.")
        print("Download from: https://ffmpeg.org/download.html")
        return 1

    # Determine input pattern
    input_pattern = args.input

    # If input is a directory, try to find sequence
    if os.path.isdir(input_pattern):
        print(f"Searching for image sequences in: {input_pattern}")
        seq_info = find_image_sequence(input_pattern, args.pattern)

        if not seq_info:
            print("Error: No image sequence found in directory")
            return 1

        print(f"Found sequence: {seq_info['count']} images")
        print(f"  Base name: {seq_info['base_name']}")
        print(f"  Extension: {seq_info['extension']}")
        print(f"  First number: {seq_info['first_number']}")

        # Construct ffmpeg pattern
        input_pattern = os.path.join(
            input_pattern,
            f"{seq_info['base_name']}_%0{seq_info['num_digits']}d.{seq_info['extension']}"
        )
        args.start_number = seq_info['first_number']

        print(f"Using pattern: {input_pattern}")

    # Convert to video
    print(f"\nConverting to video...")
    print(f"  FPS: {args.fps}")
    print(f"  CRF: {args.crf}")
    print(f"  Preset: {args.preset}")
    print(f"  Codec: {args.codec}")
    print(f"  Output: {args.output}")

    success = convert_to_video(
        input_pattern=input_pattern,
        output_file=args.output,
        fps=args.fps,
        crf=args.crf,
        preset=args.preset,
        codec=args.codec,
        pix_fmt=args.pix_fmt,
        start_number=args.start_number,
        verbose=args.verbose
    )

    if success:
        output_size = os.path.getsize(args.output) / (1024 * 1024)  # MB
        print(f"\n✓ Video created successfully: {args.output}")
        print(f"  File size: {output_size:.2f} MB")
        return 0
    else:
        print("\n✗ Failed to create video")
        return 1


if __name__ == '__main__':
    sys.exit(main())
