"""
Basic unit tests for BPM module
"""

import numpy as np

# conftest.py will handle path setup and working directory change

import BPM_py as BPM

def test_grid_parameters():
    """Test GridParameters creation and access"""
    print("Testing GridParameters...")
    params = BPM.GridParameters()

    params.Nx = 128
    params.Ny = 128
    params.Lx = 20e-6
    params.Ly = 20e-6
    params.Lz = 1e-3
    params.lambda_ = 1550e-9
    params.n_0 = 1.45
    params.n_background = 1.44
    params.alpha = 1e14
    params.updates = 10

    assert params.Nx == 128
    assert params.Ny == 128
    assert abs(params.lambda_ - 1550e-9) < 1e-13
    print("✓ GridParameters test passed")

def test_gaussian_propagation():
    """Test basic Gaussian beam propagation"""
    print("\nTesting Gaussian beam propagation...")

    # Setup
    params = BPM.GridParameters()
    params.Nx = 64
    params.Ny = 64
    params.Lx = 30e-6
    params.Ly = 30e-6
    params.dx = params.Lx / params.Nx
    params.dy = params.Ly / params.Ny
    params.dz = 1e-6
    params.Lz = 100e-6
    params.lambda_ = 1000e-9
    params.n_0 = 1.45
    params.n_background = 1.45
    params.alpha = 1e14
    params.updates = 5
    params.xSymmetry = 0
    params.ySymmetry = 0

    solver = BPM.BPMSolver(params)

    # Create Gaussian beam
    x = params.dx * (np.arange(params.Nx) - (params.Nx - 1) / 2.0)
    y = params.dy * (np.arange(params.Ny) - (params.Ny - 1) / 2.0)
    X, Y = np.meshgrid(x, y, indexing='ij')

    w0 = 5e-6
    E_init = np.exp(-(X**2 + Y**2) / w0**2).astype(np.complex64)

    # Uniform refractive index
    n = np.full((params.Nx, params.Ny), params.n_background, dtype=np.complex64)

    # Set fields
    ri = BPM.RefractiveIndex()
    ri.n = n.flatten().tolist()
    ri.Lx = params.Lx
    ri.Ly = params.Ly
    solver.setRI(ri)

    e_field = BPM.ElectricField()
    e_field.field = E_init.flatten().tolist()
    e_field.Lx = params.Lx
    e_field.Ly = params.Ly
    solver.setE(e_field)

    # Propagate
    result = solver.propagateFDBPM()

    # Check power conservation (should be ~1.0 in uniform medium)
    assert len(result.powers) == params.updates + 1
    assert result.powers[0] == 1.0
    assert result.powers[-1] > 0.9  # Allow some numerical loss

    print(f"  Initial power: {result.powers[0]:.6f}")
    print(f"  Final power: {result.powers[-1]:.6f}")
    print("✓ Gaussian propagation test passed")

def test_step_index_fiber():
    """Test propagation in step index fiber"""
    print("\nTesting step index fiber...")

    params = BPM.GridParameters()
    params.Nx = 100
    params.Ny = 100
    params.Lx = 25e-6
    params.Ly = 25e-6
    params.dx = params.Lx / params.Nx
    params.dy = params.Ly / params.Ny
    params.dz = 2e-6
    params.Lz = 500e-6
    params.lambda_ = 1550e-9
    params.n_0 = 1.46
    params.n_background = 1.45
    params.alpha = 3e14
    params.updates = 10

    solver = BPM.BPMSolver(params)

    # Create coordinate grids
    x = params.dx * (np.arange(params.Nx) - (params.Nx - 1) / 2.0)
    y = params.dy * (np.arange(params.Ny) - (params.Ny - 1) / 2.0)
    X, Y = np.meshgrid(x, y, indexing='ij')

    # Step index fiber
    core_radius = 4e-6
    n = np.full((params.Nx, params.Ny), params.n_background, dtype=np.complex64)
    core_mask = X**2 + Y**2 < core_radius**2
    n[core_mask] = 1.46

    # Gaussian beam centered on core
    w0 = 3e-6
    E_init = np.exp(-(X**2 + Y**2) / w0**2).astype(np.complex64)

    # Set up solver
    ri = BPM.RefractiveIndex()
    ri.n = n.flatten().tolist()
    ri.Lx = params.Lx
    ri.Ly = params.Ly
    solver.setRI(ri)

    e_field = BPM.ElectricField()
    e_field.field = E_init.flatten().tolist()
    e_field.Lx = params.Lx
    e_field.Ly = params.Ly
    solver.setE(e_field)

    # Propagate
    result = solver.propagateFDBPM()

    # In a waveguide, power should be relatively well confined
    assert result.powers[-1] > 0.7  # Most power should remain

    print(f"  Power retention: {result.powers[-1]*100:.2f}%")
    print("✓ Step index fiber test passed")

def test_symmetry():
    """Test symmetry handling"""
    print("\nTesting symmetry...")

    for xSym, ySym in [(0, 0), (1, 0), (0, 1), (1, 1)]:
        params = BPM.GridParameters()
        params.Nx = 64
        params.Ny = 64
        params.Lx = 20e-6
        params.Ly = 20e-6
        params.dx = params.Lx / params.Nx
        params.dy = params.Ly / params.Ny
        params.dz = 1e-6
        params.Lz = 50e-6
        params.lambda_ = 1000e-9
        params.n_0 = 1.45
        params.n_background = 1.45
        params.alpha = 1e14
        params.updates = 2
        params.xSymmetry = xSym
        params.ySymmetry = ySym

        solver = BPM.BPMSolver(params)

        # Simple uniform field
        E_init = np.ones((params.Nx, params.Ny), dtype=np.complex64)
        n = np.full((params.Nx, params.Ny), params.n_background, dtype=np.complex64)

        ri = BPM.RefractiveIndex()
        ri.n = n.flatten().tolist()
        ri.Lx = params.Lx
        ri.Ly = params.Ly
        solver.setRI(ri)

        e_field = BPM.ElectricField()
        e_field.field = E_init.flatten().tolist()
        e_field.Lx = params.Lx
        e_field.Ly = params.Ly
        solver.setE(e_field)

        result = solver.propagateFDBPM()
        assert len(result.powers) > 0

        print(f"  Symmetry ({xSym}, {ySym}): ✓")

    print("✓ Symmetry test passed")

if __name__ == '__main__':
    print("Running BPM unit tests...\n")
    print("="*50)

    test_grid_parameters()
    test_gaussian_propagation()
    test_step_index_fiber()
    test_symmetry()

    print("\n" + "="*50)
    print("All tests passed! ✓")
