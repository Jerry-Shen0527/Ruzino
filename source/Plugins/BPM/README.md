# BPM (Beam Propagation Method) Plugin

C++/CUDA implementation of the Beam Propagation Method for electromagnetic wave propagation, inspired by [BPM-Matlab](https://github.com/ankrh/BPM-Matlab).

## Features

- **FD-BPM**: Finite Difference BPM using Douglas-Gunn ADI method
- **FFT-BPM**: Fast Fourier Transform BPM (planned)
- **CUDA Acceleration**: GPU-accelerated computation
- **Python Bindings**: Easy-to-use Python interface via nanobind
- **Symmetry Support**: Exploit symmetry to reduce computation

## Core Algorithm

The FD-BPM implementation uses the Douglas-Gunn Alternating Direction Implicit (ADI) method:

1. **Substep 1a**: Explicit x-direction update
2. **Substep 1b**: Implicit x-direction solve (Thomson algorithm)
3. **Substep 2a**: Explicit y-direction update
4. **Substep 2b**: Implicit y-direction solve (Thomson algorithm)
5. **Apply multiplier**: Phase accumulation and edge absorber

Each step propagates the field by `dz` along the z-axis.

## Building

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## Python Usage

```python
import BPM_py as BPM
import numpy as np

# Setup parameters
params = BPM.GridParameters()
params.Nx = 200
params.Ny = 200
params.Lx = 20e-6  # 20 microns
params.Ly = 20e-6
params.Lz = 2e-3   # 2 mm propagation
params.lambda_ = 1550e-9  # 1550 nm
params.n_0 = 1.46
params.updates = 100
params.useGPU = True  # Enable CUDA

# Create solver
solver = BPM.BPMSolver(params)

# Initialize fields (see tests/example1.py for complete example)
solver.initializeRI(lambda x, y: 1.46 if x**2+y**2 < (5e-6)**2 else 1.45)
solver.initializeE(lambda x, y: np.exp(-(x**2+y**2)/(2.5e-6)**2))

# Propagate
result = solver.propagateFDBPM()

# Access results
E_final = np.array(result.finalField.field).reshape(params.Nx, params.Ny)
powers = result.powers
z_positions = result.z_positions
```

## Examples

- `tests/example1.py`: Off-center Gaussian beam in multimode fiber
- `tests/test_basic.py`: Unit tests for basic functionality

## References

- Chung, Y., & Dagli, N. (1990). "An assessment of finite difference beam propagation method"
- [BPM-Matlab](https://github.com/ankrh/BPM-Matlab) by Anders Kragh Hansen
