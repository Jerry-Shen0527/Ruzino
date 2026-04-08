"""
Example 2: Multi-segment fiber with tapering and twisting
Demonstrates:
- Multiple fiber segments
- Tapering (changing fiber width)
- Twisting (rotating the structure)
- Multi-core fibers
- Graded index cores

Based on BPM-Matlab's Example2.m
"""

import numpy as np
import matplotlib.pyplot as plt
import sys

sys.path.append("..\\..\\..\\..\\Binaries\\RelWithDebInfo\\")
import BPM_py as BPM


def calc_RI_three_cores(X, Y, n_background):
    """Three cores: two step-index and one graded-index"""
    n = np.full(X.shape, n_background, dtype=np.complex64)

    # Core 1: step index at (-7um, -7um)
    mask1 = (X + 7e-6)**2 + (Y + 7e-6)**2 < (10e-6)**2
    n[mask1] = 1.46

    # Core 2: small step index at (15um, 0)
    mask2 = (X - 15e-6)**2 + Y**2 < (1.25e-6)**2
    n[mask2] = 1.46

    # Core 3: graded index at (2um, 12um)
    corepos = np.array([2e-6, 12e-6])
    r = 10e-6
    R = np.sqrt((X - corepos[0])**2 + (Y - corepos[1])**2)
    mask3 = R < r
    n[mask3] = n_background + (1.465 - n_background) * (1 - (R[mask3]/r)**2)

    return n


def calc_RI_one_core(X, Y, n_background, taperScaling=1.0, twistAngle=0.0):
    """Only the graded index core, with tapering and rotation"""
    n = np.full(X.shape, n_background, dtype=np.complex64)

    # Original core position
    corepos_initial = np.array([2e-6, 12e-6])
    r_initial = 10e-6

    # Apply rotation
    cos_angle = np.cos(twistAngle)
    sin_angle = np.sin(twistAngle)
    corepos_rotated = np.array([
        corepos_initial[0] * cos_angle - corepos_initial[1] * sin_angle,
        corepos_initial[0] * sin_angle + corepos_initial[1] * cos_angle
    ])

    # Apply scaling
    corepos_final = corepos_rotated * taperScaling
    r_final = r_initial * taperScaling

    R = np.sqrt((X - corepos_final[0])**2 + (Y - corepos_final[1])**2)
    mask = R < r_final
    n[mask] = n_background + (1.465 - n_background) * (1 - (R[mask]/r_final)**2)

    return n


def calc_initial_E(X, Y):
    """Two Gaussian beams in different cores"""
    w_0 = 5e-6
    # Beam 1 at (15um, 0)
    E1 = np.exp(-((X - 1.5e-5)**2 + Y**2) / w_0**2)
    # Beam 2 at (-12um, -7um) with tilt
    E2 = 2 * np.exp(-((X + 12e-6)**2 + (Y + 7e-6)**2) / w_0**2)
    phase2 = 8e5 * Y  # Tilted beam
    return (E1 + E2 * np.exp(1j * phase2)).astype(np.complex64)


def run_segment(solver, params, segment_name, taperScaling=1.0, twistRate=0.0):
    """Run one propagation segment"""
    print(f"\n{'='*60}")
    print(f"Running {segment_name}")
    print(f"  Lz = {params.Lz*1e3:.1f} mm")
    print(f"  Taper scaling = {taperScaling:.3f}")
    print(f"  Twist rate = {twistRate:.2f} rad/m")

    result = solver.propagateFDBPM()

    print(f"  Final power = {result.powers[-1]:.4f}")
    print(f"  Power loss = {(1-result.powers[-1])*100:.2f}%")

    return result


def main():
    # Common parameters
    params = BPM.GridParameters()
    params.useGPU = False
    params.useAllCPUs = False
    params.updates = 30

    # Resolution
    params.Lx = 50e-6 * 1.5
    params.Ly = 50e-6 * 1.5
    params.Nx = int(200 * 1.5)
    params.Ny = int(200 * 1.5)
    params.dz = 1e-6
    params.alpha = 3e14

    # Physics
    params.lambda_ = 1000e-9
    params.n_background = 1.45
    params.n_0 = 1.46

    params.xSymmetry = 0
    params.ySymmetry = 0

    # Grid
    dx = params.Lx / params.Nx
    dy = params.Ly / params.Ny
    x = dx * (np.arange(params.Nx) - (params.Nx - 1) / 2.0)
    y = dy * (np.arange(params.Ny) - (params.Ny - 1) / 2.0)
    X, Y = np.meshgrid(x, y, indexing="ij")

    all_results = []

    # ========== Segment 1: Straight fiber ==========
    params.Lz = 2e-3
    params.taperScaling = 1.0
    params.twistRate = 0.0

    print("Initializing BPM solver for Segment 1...")
    solver = BPM.BPMSolver(params)

    # Set RI
    n_field = calc_RI_three_cores(X, Y, params.n_background)
    ri = BPM.RefractiveIndex()
    ri.n = n_field.flatten().tolist()
    ri.Nx = params.Nx
    ri.Ny = params.Ny
    ri.Nz = 1
    ri.Lx = params.Lx
    ri.Ly = params.Ly
    solver.setRI(ri)

    # Set E field
    E_init = calc_initial_E(X, Y)
    E_init = E_init / np.sqrt(np.sum(np.abs(E_init)**2))
    e_field = BPM.ElectricField()
    e_field.field = E_init.flatten().tolist()
    e_field.Lx = params.Lx
    e_field.Ly = params.Ly
    solver.setE(e_field)

    result1 = run_segment(solver, params, "Segment 1: Straight fiber")
    all_results.append(result1)

    # ========== Segment 2: Tapering and twisting ==========
    params.Lz = 5e-3
    params.taperScaling = 0.15
    params.twistRate = 2 * np.pi / params.Lz

    solver2 = BPM.BPMSolver(params)

    # Use output from segment 1 as input
    e_field.field = result1.finalField.field
    solver2.setE(e_field)
    solver2.setRI(result1.finalRI)

    result2 = run_segment(solver2, params, "Segment 2: Tapering & Twisting")
    all_results.append(result2)

    # ========== Segment 3: Straight again ==========
    params.Lz = 2e-3
    params.taperScaling = 1.0
    params.twistRate = 0.0

    solver3 = BPM.BPMSolver(params)
    e_field.field = result2.finalField.field
    solver3.setE(e_field)
    solver3.setRI(result2.finalRI)

    result3 = run_segment(solver3, params, "Segment 3: Straight fiber")
    all_results.append(result3)

    # ========== Segment 4: Single core only ==========
    params.Lz = 3e-3

    solver4 = BPM.BPMSolver(params)

    # New RI with only one core (scaled and rotated)
    n_field = calc_RI_one_core(X, Y, params.n_background,
                                taperScaling=0.15, twistAngle=2*np.pi)
    ri.n = n_field.flatten().tolist()
    solver4.setRI(ri)

    e_field.field = result3.finalField.field
    solver4.setE(e_field)

    result4 = run_segment(solver4, params, "Segment 4: Single core")
    all_results.append(result4)

    # Plot results
    plot_results(x, y, all_results, params)


def plot_results(x, y, results, params):
    """Plot final state and power evolution"""
    fig = plt.figure(figsize=(16, 10))

    # Combine all z positions and powers
    z_all = []
    powers_all = []
    for result in results:
        z_all.extend(result.z_positions)
        powers_all.extend(result.powers)

    # Plot 1: Power evolution
    ax1 = plt.subplot(2, 3, 1)
    ax1.plot(np.array(z_all) * 1e3, powers_all, 'b-', linewidth=2)
    ax1.set_xlabel('Propagation distance [mm]')
    ax1.set_ylabel('Relative power')
    ax1.set_ylim([0, 1.1])
    ax1.grid(True, alpha=0.3)
    ax1.set_title('Power Evolution Across All Segments')

    # Add segment boundaries
    cumulative_z = 0
    for i, result in enumerate(results):
        cumulative_z += result.z_positions[-1]
        if i < len(results) - 1:
            ax1.axvline(cumulative_z * 1e3, color='r', linestyle='--', alpha=0.5)
            ax1.text(cumulative_z * 1e3, 0.95, f'Seg {i+1}→{i+2}',
                    rotation=90, va='top', fontsize=8)

    # Plot 2-5: Final field of each segment
    for i, result in enumerate(results[:4]):
        E_final = np.array(result.finalField.field).reshape(params.Nx, params.Ny)
        intensity = np.abs(E_final)**2
        intensity_log = np.log10(intensity + 1e-10)

        ax = plt.subplot(2, 3, i + 2)
        im = ax.pcolormesh(x * 1e6, y * 1e6, intensity_log.T,
                          shading='auto', cmap='hot')
        ax.set_xlabel('x [μm]')
        ax.set_ylabel('y [μm]')
        ax.set_title(f'Segment {i+1} Final Intensity')
        ax.set_aspect('equal')
        plt.colorbar(im, ax=ax, label='log₁₀(I)')

    # Plot 6: Final phase
    ax = plt.subplot(2, 3, 6)
    E_final = np.array(results[-1].finalField.field).reshape(params.Nx, params.Ny)
    phase = np.angle(E_final)
    im = ax.pcolormesh(x * 1e6, y * 1e6, phase.T,
                      shading='auto', cmap='hsv', vmin=-np.pi, vmax=np.pi)
    ax.set_xlabel('x [μm]')
    ax.set_ylabel('y [μm]')
    ax.set_title('Final Phase')
    ax.set_aspect('equal')
    cbar = plt.colorbar(im, ax=ax, label='Phase [rad]')
    cbar.set_ticks([-np.pi, 0, np.pi])
    cbar.set_ticklabels(['-π', '0', 'π'])

    plt.suptitle('BPM Example 2: Multi-core Fiber with Tapering & Twisting',
                fontsize=14, fontweight='bold')
    plt.tight_layout()
    plt.savefig('bpm_example2_results.png', dpi=150, bbox_inches='tight')
    print("\nResults saved to bpm_example2_results.png")
    plt.show()


if __name__ == "__main__":
    main()
