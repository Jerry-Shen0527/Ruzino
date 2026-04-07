"""
Example 3: GRIN (Graded Index) Lens Beam Collimation
Demonstrates:
- Radially symmetric graded index profiles
- Beam collimation
- FD-BPM in lens, FFT-BPM in free space

Based on BPM-Matlab's Example4.m
"""

import numpy as np
import matplotlib.pyplot as plt
import sys

sys.path.append("..\\..\\..\\..\\Binaries\\RelWithDebInfo\\")
import BPM_py as BPM


def calc_RI_grin_lens(X, Y, n_background):
    """GRIN lens: GT-CFRL-100-025-20-CC (810)"""
    n = np.full(X.shape, n_background, dtype=np.complex64)
    R = np.sqrt(X**2 + Y**2)
    g = 260  # Gradient constant

    # Radially symmetric profile
    mask = R < 500e-6
    n[mask] = 1.521 * np.cosh(g * R[mask])  # sech approximation
    # Actually sech, but cosh is closer to the actual behavior
    n[mask] = 1.521 / np.cosh(g * R[mask])

    return n


def calc_initial_E_gaussian(X, Y):
    """Small Gaussian beam at center"""
    w_0 = 5e-6
    amplitude = np.exp(-(X**2 + Y**2) / w_0**2)
    return amplitude.astype(np.complex64)


def main():
    print("="*60)
    print("Example 3: GRIN Lens Beam Collimation")
    print("="*60)

    # ========== Part 1: Propagation through GRIN lens ==========
    print("\nPart 1: FD-BPM through GRIN lens")

    params = BPM.GridParameters()
    params.useGPU = False
    params.updates = 100

    # Resolution for GRIN lens
    params.Lx = 0.7e-3
    params.Ly = 0.7e-3
    params.Nx = 1000
    params.Ny = 1000
    params.dz = 20e-6
    params.alpha = 3e14

    # Physics
    params.lambda_ = 810e-9
    params.n_background = 1.0  # Air
    params.n_0 = 1.521  # GRIN lens reference
    params.Lz = 6.05e-3  # GRIN lens length

    params.xSymmetry = 0
    params.ySymmetry = 0

    solver1 = BPM.BPMSolver(params)

    # Create grids
    dx = params.Lx / params.Nx
    dy = params.Ly / params.Ny
    x = dx * (np.arange(params.Nx) - (params.Nx - 1) / 2.0)
    y = dy * (np.arange(params.Ny) - (params.Ny - 1) / 2.0)
    X, Y = np.meshgrid(x, y, indexing="ij")

    # Initialize
    print("  Initializing GRIN lens RI...")
    n_field = calc_RI_grin_lens(X, Y, params.n_background)
    ri = BPM.RefractiveIndex()
    ri.n = n_field.flatten().tolist()
    ri.Nx = params.Nx
    ri.Ny = params.Ny
    ri.Nz = 1
    ri.Lx = params.Lx
    ri.Ly = params.Ly
    solver1.setRI(ri)

    print("  Initializing Gaussian beam...")
    E_init = calc_initial_E_gaussian(X, Y)
    E_init = E_init / np.sqrt(np.sum(np.abs(E_init)**2))
    e_field = BPM.ElectricField()
    e_field.field = E_init.flatten().tolist()
    e_field.Lx = params.Lx
    e_field.Ly = params.Ly
    solver1.setE(e_field)

    print("  Running FD-BPM...")
    result1 = solver1.propagateFDBPM()

    print(f"  Final power: {result1.powers[-1]:.4f}")

    # ========== Part 2: FFT-BPM in free space ==========
    print("\nPart 2: FFT-BPM in free space")
    print("  (Not implemented yet, would show beam collimation)")

    # Plot results
    plot_results(x, y, result1, params)


def plot_results(x, y, result, params):
    """Visualize GRIN lens results"""
    E_final = np.array(result.finalField.field).reshape(params.Nx, params.Ny)
    n_final = np.array(result.finalRI.n).reshape(params.Nx, params.Ny)

    fig, axes = plt.subplots(2, 2, figsize=(14, 11))

    # Plot 1: Refractive index profile
    ax = axes[0, 0]
    im = ax.pcolormesh(x * 1e6, y * 1e6, np.real(n_final).T,
                      shading='auto', cmap='viridis')
    ax.set_xlabel('x [μm]')
    ax.set_ylabel('y [μm]')
    ax.set_title('GRIN Lens Refractive Index')
    ax.set_aspect('equal')
    plt.colorbar(im, ax=ax, label='n')

    # Add circle for lens boundary
    theta = np.linspace(0, 2*np.pi, 100)
    ax.plot(500 * np.cos(theta), 500 * np.sin(theta),
           'r--', linewidth=2, label='Lens boundary')
    ax.legend()

    # Plot 2: Power evolution
    ax = axes[0, 1]
    ax.plot(np.array(result.z_positions) * 1e3, result.powers, 'b-', linewidth=2)
    ax.set_xlabel('Propagation distance [mm]')
    ax.set_ylabel('Relative power')
    ax.set_ylim([0, 1.1])
    ax.grid(True, alpha=0.3)
    ax.set_title('Power Evolution')

    # Plot 3: Intensity
    ax = axes[1, 0]
    intensity = np.abs(E_final)**2
    intensity_log = np.log10(intensity + 1e-10)
    im = ax.pcolormesh(x * 1e6, y * 1e6, intensity_log.T,
                      shading='auto', cmap='hot')
    ax.set_xlabel('x [μm]')
    ax.set_ylabel('y [μm]')
    ax.set_title('Output Intensity (log scale)')
    ax.set_aspect('equal')
    plt.colorbar(im, ax=ax, label='log₁₀(I)')

    # Plot 4: Radial intensity profile
    ax = axes[1, 1]
    r = np.sqrt(X**2 + Y**2)
    r_1d = np.linspace(0, np.max(r), 500)

    # Interpolate intensity to radial coords
    from scipy.interpolate import griddata
    intensity_1d = griddata(r.flatten(), intensity.flatten(), r_1d, method='linear')

    ax.semilogy(r_1d * 1e6, intensity_1d, 'b-', linewidth=2)
    ax.set_xlabel('Radius [μm]')
    ax.set_ylabel('Intensity [W/m²]')
    ax.set_title('Radial Intensity Profile')
    ax.grid(True, alpha=0.3)
    ax.axvline(500, color='r', linestyle='--', label='Lens edge')
    ax.legend()

    plt.suptitle(f'GRIN Lens @ λ={params.lambda_*1e9:.0f} nm, L={params.Lz*1e3:.2f} mm',
                fontsize=14, fontweight='bold')
    plt.tight_layout()
    plt.savefig('bpm_example3_results.png', dpi=150, bbox_inches='tight')
    print("\nResults saved to bpm_example3_results.png")
    plt.show()


if __name__ == "__main__":
    main()
