#!/usr/bin/env python3
"""
PDF Debug Script

Debug PDF calculation by sampling specific directions and checking PDF values.
This is a diagnostic/utility script, not a test.
Run directly: python pdf_debug.py
"""

import sys
import os
from pathlib import Path
import numpy as np

# Setup paths
script_dir = Path(__file__).parent.resolve()
workspace_root = script_dir.parent.parent.parent.parent
binary_dir = workspace_root / "Binaries" / "Release"
assets_dir = workspace_root / "Assets"

# Set environment variables
os.environ["PXR_USD_WINDOWS_DLL_PATH"] = str(binary_dir)
mtlx_stdlib = binary_dir / "libraries"
if mtlx_stdlib.exists():
    os.environ["PXR_MTLX_STDLIB_SEARCH_PATHS"] = str(mtlx_stdlib)
os.environ["PATH"] = str(binary_dir) + os.pathsep + os.environ.get("PATH", "")

if hasattr(os, "add_dll_directory"):
    os.add_dll_directory(str(binary_dir))

os.chdir(str(binary_dir))
sys.path.insert(0, str(binary_dir))

# Import USD first
from pxr import Usd, UsdGeom, UsdShade, Sdf, UsdMtlx

# Import modules
import hd_RUZINO_py as renderer
import nodes_core_py as core

print("Test: Analyze PDF for specific sampled directions")
print("="*80)

# For Acryl_Plastic at 35° incident angle:
# - metalness = 0 (so always nonmetal)
# - transmission = 0.75 (high transmission)
# - specular_roughness = 0.15 (low roughness)
# - specular_IOR = 1.45

print("\nMaterial properties:")
print("  metalness = 0 (100% nonmetal)")
print("  transmission = 0.75")
print("  specular_roughness = 0.15")
print("  specular_IOR = 1.45")
print("  subsurface = 1")

print("\nIncident angle: 35°")
print("  V = (sin(35°), 0, cos(35°)) ≈ (0.574, 0, 0.819)")
print("  NdotV = 0.819")

print("\nCompute Fresnel:")
eta = 1.45
NdotV = 0.819
F = ((1 - eta*NdotV) / (1 + eta*NdotV))**2  # Simplified Fresnel
print(f"  Fresnel ≈ {F:.4f} (rough estimate)")

print("\nCompute lobe weights (before normalization):")
print("  diffuse_weight ∝ (1-F) * base_color_lum * (1-transmission)")
print("  reflection_weight ∝ F * specular")  
print("  transmission_weight ∝ (1-F) * transmission * transmission_color_lum")

print("\nFor transmission=0.75, reflection should dominate over diffuse.")
print("Expected sampling distribution:")
print("  ~5-10% diffuse")
print("  ~20-30% reflection")
print("  ~60-75% transmission")

print("\n" + "="*80)
print("HYPOTHESIS:")
print("="*80)
print("If CV=3.21 persists, the issue might be:")
print("1. VNDF sampling doesn't match VNDF PDF formula")
print("2. Fresnel calculation in sampling vs PDF differs")
print("3. Weight calculation has subtle bugs")
print()
print("To debug: Add printf statements in shader to log:")
print("  - Sampled lobe (diffuse/reflection/transmission)")
print("  - Returned PDF value")
print("  - Re-computed PDF value in importance test")
print("="*80)
