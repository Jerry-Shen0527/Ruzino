#!/usr/bin/env python3
"""
Batch BRDF Testing Script with Multiprocessing

Scans all materials in matx_library and tests them in parallel,
then aggregates individual JSON results into a summary report.

Run directly: python brdf_batch.py
"""

import sys
import os
from pathlib import Path
import json
from datetime import datetime
import subprocess
from multiprocessing import Pool, cpu_count
import time

# Setup paths
script_dir = Path(__file__).parent.resolve()
workspace_root = script_dir.parent.parent.parent.parent
binary_dir = workspace_root / "Binaries" / "Release"
assets_dir = workspace_root / "Assets"

# Test script path
test_script = script_dir / "brdf_comprehensive.py"


def find_all_material_folders():
    """Find all material folders in matx_library"""
    matx_library = assets_dir / "matx_library"
    material_folders = []
    
    for item in matx_library.iterdir():
        if item.is_dir():
            # Check if folder contains .mtlx file
            mtlx_files = list(item.glob("*.mtlx"))
            if mtlx_files:
                material_folders.append(item)
    
    material_folders.sort()
    return material_folders


def test_single_material(material_folder):
    """
    Test a single material by calling brdf_comprehensive.py
    
    Returns:
        tuple: (material_name, success, elapsed_time)
    """
    material_name = material_folder.name
    start_time = time.time()
    
    try:
        # Call the test script
        result = subprocess.run(
            [sys.executable, str(test_script), str(material_folder)],
            capture_output=True,
            text=True,
            cwd=str(script_dir)
        )
        
        elapsed = time.time() - start_time
        success = result.returncode == 0
        
        if not success:
            print(f"  ✗ {material_name} - FAILED (stderr: {result.stderr[:100]})")
        else:
            print(f"  ✓ {material_name} - OK ({elapsed:.1f}s)")
        
        return (material_name, success, elapsed)
        
    except subprocess.TimeoutExpired:
        elapsed = time.time() - start_time
        print(f"  ✗ {material_name} - TIMEOUT ({elapsed:.1f}s)")
        return (material_name, False, elapsed)
    except Exception as e:
        elapsed = time.time() - start_time
        print(f"  ✗ {material_name} - ERROR: {e}")
        return (material_name, False, elapsed)


def load_material_result(material_folder):
    """Load the JSON result for a material"""
    material_name = list(material_folder.glob("*.mtlx"))[0].stem
    result_json = binary_dir / "brdf_tests" / material_name / f"result_{material_name}.json"
    
    if not result_json.exists():
        return None
    
    try:
        with open(result_json, 'r') as f:
            return json.load(f)
    except Exception as e:
        print(f"  Warning: Could not load result for {material_name}: {e}")
        return None


def aggregate_results(material_folders):
    """Aggregate all individual JSON results into a summary"""
    all_results = []
    success_count = 0
    fail_count = 0
    total_time = 0
    
    for folder in material_folders:
        result_data = load_material_result(folder)
        if result_data and result_data.get("result"):
            result = result_data["result"]
            all_results.append(result)
            
            if result["status"] == "success":
                success_count += 1
            else:
                fail_count += 1
    
    return all_results, success_count, fail_count


def main():
    # Parse arguments
    num_workers = cpu_count()
    if len(sys.argv) > 1:
        try:
            num_workers = int(sys.argv[1])
        except ValueError:
            pass
    
    print("="*80)
    print("Batch BRDF Testing with Multiprocessing")
    print("="*80)
    print(f"Workers: {num_workers}")
    print(f"Test script: {test_script.name}")
    print("="*80)
    
    # Find all materials
    material_folders = find_all_material_folders()
    print(f"\nFound {len(material_folders)} materials\n")
    
    if not material_folders:
        print("No materials found!")
        return 1
    
    # Create output directory
    output_dir = binary_dir / "brdf_tests"
    output_dir.mkdir(exist_ok=True)
    
    # Test materials in parallel
    print("Testing materials in parallel...\n")
    start_time = time.time()
    
    with Pool(processes=num_workers) as pool:
        test_results = pool.map(test_single_material, material_folders)
    
    total_elapsed = time.time() - start_time
    
    # Count results
    success_count = sum(1 for _, success, _ in test_results if success)
    fail_count = len(test_results) - success_count
    
    print("\n" + "="*80)
    print("Aggregating results...")
    print("="*80)
    
    # Aggregate all JSON results
    all_results, json_success, json_fail = aggregate_results(material_folders)
    
    # Save aggregated results
    output_json = output_dir / f"brdf_test_summary_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
    
    # Get test config from first successful result
    test_config = {}
    for result_data in [load_material_result(f) for f in material_folders]:
        if result_data and "test_config" in result_data:
            test_config = result_data["test_config"]
            break
    
    summary = {
        "test_config": test_config,
        "batch_info": {
            "num_workers": num_workers,
            "total_time_seconds": total_elapsed,
            "timestamp": datetime.now().isoformat()
        },
        "summary": {
            "total_materials": len(material_folders),
            "successful": json_success,
            "failed": json_fail
        },
        "results": all_results
    }
    
    with open(output_json, 'w') as f:
        json.dump(summary, f, indent=2)
    
    # Print summary
    print("\n" + "="*80)
    print("SUMMARY")
    print("="*80)
    print(f"Total materials:  {len(material_folders)}")
    print(f"Successful:       {json_success}")
    print(f"Failed:           {json_fail}")
    print(f"Total time:       {total_elapsed:.1f}s ({total_elapsed/60:.1f}m)")
    print(f"Avg time/mat:     {total_elapsed/len(material_folders):.1f}s")
    print(f"\nResults saved to: {output_json.name}")
    print("="*80)
    
    # Print top 5 best and worst materials by L2 distance
    successful_results = [r for r in all_results if r["status"] == "success" and r.get("aggregate_stats")]
    if successful_results:
        sorted_by_l2 = sorted(successful_results, key=lambda x: x["aggregate_stats"]["avg_eval_sample_l2"])
        
        print("\nTop 5 Best Materials (lowest L2 distance):")
        for i, r in enumerate(sorted_by_l2[:5], 1):
            print(f"  {i}. {r['material_name']}: L2={r['aggregate_stats']['avg_eval_sample_l2']:.6f}, Var={r['aggregate_stats']['avg_importance_variance']:.6f}")
        
        if len(sorted_by_l2) >= 5:
            print("\nTop 5 Worst Materials (highest L2 distance):")
            for i, r in enumerate(sorted_by_l2[-5:][::-1], 1):
                print(f"  {i}. {r['material_name']}: L2={r['aggregate_stats']['avg_eval_sample_l2']:.6f}, Var={r['aggregate_stats']['avg_importance_variance']:.6f}")
    
    print("\n" + "="*80)
    print(f"Individual results stored in: {output_dir}/[material_name]/result_[material_name].json")
    print("="*80)
    
    return 0 if json_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
