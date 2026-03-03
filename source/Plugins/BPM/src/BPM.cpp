#include <BPM/BPM.h>
#define _USE_MATH_DEFINES
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <numeric>
#include <stdexcept>

RUZINO_NAMESPACE_OPEN_SCOPE

// Helper functions
std::vector<float> getGridArray(int N, float delta, uint8_t symmetry)
{
    std::vector<float> grid(N);
    if (symmetry != 0) {
        for (int i = 0; i < N; ++i) {
            grid[i] = delta * i;
        }
    }
    else {
        for (int i = 0; i < N; ++i) {
            grid[i] = delta * (i - (N - 1) / 2.0f);
        }
    }
    return grid;
}

void calcFullField(
    const std::vector<float>& x,
    const std::vector<float>& y,
    const ComplexField& field,
    std::vector<float>& x_full,
    std::vector<float>& y_full,
    ComplexField& field_full)
{
    int Nx = x.size();
    int Ny = y.size();

    // Determine symmetry from grid
    bool xSymmetric = (x[0] >= 0);
    bool ySymmetric = (y[0] >= 0);

    if (!xSymmetric && !ySymmetric) {
        // No symmetry, just copy
        x_full = x;
        y_full = y;
        field_full = field;
        return;
    }

    // Build full grid with mirroring
    int Nx_full = xSymmetric ? 2 * Nx - 1 : Nx;
    int Ny_full = ySymmetric ? 2 * Ny - 1 : Ny;

    x_full.resize(Nx_full);
    y_full.resize(Ny_full);
    field_full.resize(Nx_full * Ny_full);

    // Fill x coordinates
    for (int i = 0; i < Nx; ++i) {
        x_full[Nx - 1 + i] = x[i];
        if (xSymmetric && i > 0)
            x_full[Nx - 1 - i] = -x[i];
    }

    // Fill y coordinates
    for (int j = 0; j < Ny; ++j) {
        y_full[Ny - 1 + j] = y[j];
        if (ySymmetric && j > 0)
            y_full[Ny - 1 - j] = -y[j];
    }

    // Fill field values with proper mirroring
    for (int iy = 0; iy < Ny_full; ++iy) {
        for (int ix = 0; ix < Nx_full; ++ix) {
            int ix_src = xSymmetric ? std::abs(ix - (Nx - 1)) : ix;
            int iy_src = ySymmetric ? std::abs(iy - (Ny - 1)) : iy;
            field_full[ix + iy * Nx_full] = field[ix_src + iy_src * Nx];
        }
    }
}

// BPMSolver Implementation
struct BPMSolver::Impl {
    GridParameters params;
    ElectricField E;
    RefractiveIndex RI;
    std::vector<Mode> modes;

    Impl(const GridParameters& p) : params(p)
    {
    }
};

BPMSolver::BPMSolver(const GridParameters& params)
    : impl_(std::make_unique<Impl>(params))
{
    // Auto-calculate dx, dy, dz if not set
    auto& p = impl_->params;
    if (p.dx == 0.0f && p.Nx > 0 && p.Lx > 0) {
        p.dx = p.Lx / p.Nx;
    }
    if (p.dy == 0.0f && p.Ny > 0 && p.Ly > 0) {
        p.dy = p.Ly / p.Ny;
    }
    if (p.dz == 0.0f && p.Lz > 0 && p.updates > 0) {
        p.dz = p.Lz / p.updates;  // Default: one step per update
    }
}

BPMSolver::~BPMSolver() = default;

void BPMSolver::initializeRI(std::function<Complex(float, float)> riFunc)
{
    auto& p = impl_->params;
    impl_->RI.Nx = p.Nx;
    impl_->RI.Ny = p.Ny;
    impl_->RI.Lx = p.Lx;
    impl_->RI.Ly = p.Ly;
    impl_->RI.n.resize(p.Nx * p.Ny);

    auto x = getGridArray(p.Nx, p.dx, p.ySymmetry);
    auto y = getGridArray(p.Ny, p.dy, p.xSymmetry);

    for (int iy = 0; iy < p.Ny; ++iy) {
        for (int ix = 0; ix < p.Nx; ++ix) {
            impl_->RI.n[ix + iy * p.Nx] = riFunc(x[ix], y[iy]);
        }
    }
}

void BPMSolver::initializeE(std::function<Complex(float, float)> eFunc)
{
    auto& p = impl_->params;
    impl_->E.field.resize(p.Nx * p.Ny);
    impl_->E.Lx = p.Lx;
    impl_->E.Ly = p.Ly;

    auto x = getGridArray(p.Nx, p.dx, p.ySymmetry);
    auto y = getGridArray(p.Ny, p.dy, p.xSymmetry);

    for (int iy = 0; iy < p.Ny; ++iy) {
        for (int ix = 0; ix < p.Nx; ++ix) {
            impl_->E.field[ix + iy * p.Nx] = eFunc(x[ix], y[iy]);
        }
    }

    // Normalize power
    float power = 0.0f;
    for (const auto& e : impl_->E.field) {
        power += std::norm(e);
    }
    float norm = std::sqrt(power);
    for (auto& e : impl_->E.field) {
        e /= norm;
    }
}
void BPMSolver::setRI(const RefractiveIndex& ri)
{
    impl_->RI = ri;
}
void BPMSolver::setE(const ElectricField& e)
{
    impl_->E = e;
}

// FD-BPM Propagator Implementation
void FDBPMPropagator::propagateSegment(
    Complex* E,
    int Nx,
    int Ny,
    int iz_start,
    int iz_end,
    const Parameters& params,
    bool useGPU)
{
    if (useGPU) {
#ifdef BUILD_WITH_CUDA
        // Allocate device memory
        Complex* d_E1;
        Complex* d_E2;
        Complex* d_Eyx;
        Complex* d_b;
        Complex* d_n_out;
        float* d_multiplier;
        Complex* d_n_mat;
        float* d_EfieldPower;
        float* d_precisePowerDiff;

        size_t size_E = Nx * Ny * sizeof(Complex);
        size_t size_n =
            params.Nx_n * params.Ny_n * params.Nz_n * sizeof(Complex);

        cudaMalloc(&d_E1, size_E);
        cudaMalloc(&d_E2, size_E);
        cudaMalloc(&d_Eyx, size_E);
        cudaMalloc(&d_b, size_E);
        cudaMalloc(&d_n_out, size_E);
        cudaMalloc(&d_multiplier, Nx * Ny * sizeof(float));
        cudaMalloc(&d_n_mat, size_n);
        cudaMalloc(&d_EfieldPower, sizeof(float));
        cudaMalloc(&d_precisePowerDiff, sizeof(float));

        // Copy to device
        cudaMemcpy(d_E1, E, size_E, cudaMemcpyHostToDevice);
        cudaMemcpy(
            d_multiplier,
            params.multiplier,
            Nx * Ny * sizeof(float),
            cudaMemcpyHostToDevice);
        cudaMemcpy(d_n_mat, params.n_mat, size_n, cudaMemcpyHostToDevice);

        float h_EfieldPower;
        float h_precisePowerDiff;

        for (int iz = iz_start; iz < iz_end; ++iz) {
            // Reset power accumulators
            h_EfieldPower = 0.0f;
            h_precisePowerDiff = 0.0f;
            cudaMemcpy(
                d_EfieldPower,
                &h_EfieldPower,
                sizeof(float),
                cudaMemcpyHostToDevice);
            cudaMemcpy(
                d_precisePowerDiff,
                &h_precisePowerDiff,
                sizeof(float),
                cudaMemcpyHostToDevice);

            // Call CUDA kernels
            cuda::substep1a_kernel(d_E1, d_E2, d_Eyx, params);
            cuda::substep1b_kernel(d_Eyx, d_b, params);
            cuda::substep2a_kernel(d_E1, d_Eyx, d_E2, params);
            cuda::substep2b_kernel(d_E2, d_b, d_EfieldPower, params);
            cuda::applyMultiplier_kernel(
                d_E2, d_n_out, params, iz, d_precisePowerDiff);

            // Swap pointers for next iteration
            std::swap(d_E1, d_E2);

            // Update precise power
            cudaMemcpy(
                &h_precisePowerDiff,
                d_precisePowerDiff,
                sizeof(float),
                cudaMemcpyDeviceToHost);
            // This should update params.precisePower
        }

        // Copy result back
        cudaMemcpy(E, d_E1, size_E, cudaMemcpyDeviceToHost);

        // Free device memory
        cudaFree(d_E1);
        cudaFree(d_E2);
        cudaFree(d_Eyx);
        cudaFree(d_b);
        cudaFree(d_n_out);
        cudaFree(d_multiplier);
        cudaFree(d_n_mat);
        cudaFree(d_EfieldPower);
        cudaFree(d_precisePowerDiff);
#else
        throw std::runtime_error(
            "GPU support not compiled (BUILD_WITH_CUDA not defined)");
#endif
    }
    else {
        // CPU implementation
        std::vector<Complex> E1(Nx * Ny);
        std::vector<Complex> E2(Nx * Ny);
        std::vector<Complex> b(Nx * Ny);

        std::copy(E, E + Nx * Ny, E1.begin());

        for (int iz = iz_start; iz < iz_end; ++iz) {
            // Douglas-Gunn ADI method
            substep1a(E1.data(), E2.data(), params, Nx, Ny);
            substep1b(E2.data(), b.data(), params, Nx, Ny);
            substep2a(E1.data(), E2.data(), params, Nx, Ny);
            substep2b(E2.data(), b.data(), params, Nx, Ny);
            applyMultiplier(E2.data(), params, iz, Nx, Ny);

            std::swap(E1, E2);
        }

        std::copy(E1.begin(), E1.end(), E);
    }
}

void FDBPMPropagator::substep1a(
    Complex* E1,
    Complex* E2,
    const Parameters& p,
    int Nx,
    int Ny)
{
    // Explicit x-direction update
    bool yAntiSymm = p.ySymmetry == 2;
    bool xAntiSymm = p.xSymmetry == 2;

#ifdef _OPENMP
#pragma omp parallel for collapse(2)
#endif
    for (int iy = 0; iy < Ny; ++iy) {
        for (int ix = 0; ix < Nx; ++ix) {
            int i = ix + iy * Nx;
            E2[i] = E1[i];

            if (ix > 0)
                E2[i] += (E1[i - 1] - E1[i]) * p.ax;
            if (ix < Nx - 1 && (!yAntiSymm || ix != 0))
                E2[i] += (E1[i + 1] - E1[i]) * p.ax;
            if (iy > 0)
                E2[i] += (E1[i - Nx] - E1[i]) * p.ay * 2.0f;
            if (iy < Ny - 1 && (!xAntiSymm || iy != 0))
                E2[i] += (E1[i + Nx] - E1[i]) * p.ay * 2.0f;
        }
    }
}

void FDBPMPropagator::substep1b(
    Complex* E2,
    Complex* b,
    const Parameters& p,
    int Nx,
    int Ny)
{
    // Implicit x-direction solve using Thomson algorithm
    bool yAntiSymm = p.ySymmetry == 2;

#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (int iy = 0; iy < Ny; ++iy) {
        // Forward sweep
        for (int ix = 0; ix < Nx; ++ix) {
            int i = ix + iy * Nx;

            if (ix == 0 && yAntiSymm)
                b[i] = 1.0f;
            else if (ix == 0)
                b[i] = 1.0f + p.ax;
            else if (ix < Nx - 1)
                b[i] = 1.0f + 2.0f * p.ax;
            else
                b[i] = 1.0f + p.ax;

            if (ix > 0) {
                Complex w = -p.ax / b[i - 1];
                b[i] += w * (ix == 1 && yAntiSymm ? 0.0f : p.ax);
                E2[i] -= w * E2[i - 1];
            }
        }

        // Backward sweep
        for (int ix = Nx - 1; ix >= (yAntiSymm ? 1 : 0); --ix) {
            int i = ix + iy * Nx;
            E2[i] = (E2[i] + (ix == Nx - 1 ? 0.0f : p.ax * E2[i + 1])) / b[i];
        }
    }
}

void FDBPMPropagator::substep2a(
    Complex* E1,
    Complex* E2,
    const Parameters& p,
    int Nx,
    int Ny)
{
    // Explicit y-direction update
    bool xAntiSymm = p.xSymmetry == 2;

#ifdef _OPENMP
#pragma omp parallel for collapse(2)
#endif
    for (int iy = 0; iy < Ny; ++iy) {
        for (int ix = 0; ix < Nx; ++ix) {
            int i = ix + iy * Nx;

            if (iy > 0)
                E2[i] -= (E1[i - Nx] - E1[i]) * p.ay;
            if (iy < Ny - 1 && (!xAntiSymm || iy != 0))
                E2[i] -= (E1[i + Nx] - E1[i]) * p.ay;
        }
    }
}

void FDBPMPropagator::substep2b(
    Complex* E2,
    Complex* b,
    const Parameters& p,
    int Nx,
    int Ny)
{
    // Implicit y-direction solve
    bool xAntiSymm = p.xSymmetry == 2;

#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (int ix = 0; ix < Nx; ++ix) {
        // Forward sweep
        for (int iy = 0; iy < Ny; ++iy) {
            int i = ix + iy * Nx;

            if (iy == 0 && xAntiSymm)
                b[i] = 1.0f;
            else if (iy == 0)
                b[i] = 1.0f + p.ay;
            else if (iy < Ny - 1)
                b[i] = 1.0f + 2.0f * p.ay;
            else
                b[i] = 1.0f + p.ay;

            if (iy > 0) {
                Complex w = -p.ay / b[i - Nx];
                b[i] += w * (iy == 1 && xAntiSymm ? 0.0f : p.ay);
                E2[i] -= w * E2[i - Nx];
            }
        }

        // Backward sweep
        for (int iy = Ny - 1; iy >= (xAntiSymm ? 1 : 0); --iy) {
            int i = ix + iy * Nx;
            E2[i] = (E2[i] + (iy == Ny - 1 ? 0.0f : p.ay * E2[i + Nx])) / b[i];
        }
    }
}

void FDBPMPropagator::applyMultiplier(
    Complex* E,
    const Parameters& p,
    int iz,
    int Nx,
    int Ny)
{
    float fieldCorrection = std::sqrt(
        static_cast<float>(p.precisePower) /
        std::accumulate(E, E + Nx * Ny, 0.0f, [](float sum, Complex c) {
            return sum + std::norm(c);
        }));

    float cosValue = std::cos(-p.twistPerStep * iz);
    float sinValue = std::sin(-p.twistPerStep * iz);
    float scaling = 1.0f / (1.0f - p.taperPerStep * iz);

    for (int i = 0; i < Nx * Ny; ++i) {
        int ix = i % Nx;
        int iy = i / Nx;

        float x = p.dx * (ix - (Nx - 1) / 2.0f * (p.ySymmetry == 0));
        float y = p.dy * (iy - (Ny - 1) / 2.0f * (p.xSymmetry == 0));

        // Get refractive index (simplified - should handle 3D interpolation)
        Complex n = p.n_mat[i];
        float n_real = n.real();

        // Apply bending if needed
        float n_bend = n_real;
        if (std::isfinite(p.RoC)) {
            float bendCoord =
                x * std::cos(p.bendDirection) + y * std::sin(p.bendDirection);
            n_bend =
                n_real *
                (1 - (n_real * n_real * bendCoord * p.rho_e) / (2 * p.RoC)) *
                std::exp(bendCoord / p.RoC);
        }

        Complex phase = std::exp(Complex(
            0,
            p.d * (n.imag() +
                   (n_bend * n_bend - p.n_0 * p.n_0) / (2.0f * p.n_0))));

        E[i] *= fieldCorrection * p.multiplier[i] * phase;
    }
}

PropagationResult BPMSolver::propagateFDBPM()
{
    PropagationResult result;
    auto& p = impl_->params;

    // Calculate step sizes
    int Nz = static_cast<int>(p.Lz / p.dz);
    std::vector<int> updateIndices;
    for (int i = 1; i <= p.updates; ++i) {
        updateIndices.push_back(static_cast<int>(i * Nz / p.updates));
    }

    // Setup FD-BPM parameters
    float k_0 = 2.0f * M_PI / p.lambda;
    FDBPMPropagator::Parameters fdParams;
    fdParams.Nx = p.Nx;
    fdParams.Ny = p.Ny;
    fdParams.dx = p.dx;
    fdParams.dy = p.dy;
    fdParams.dz = p.dz;
    fdParams.n_0 = p.n_0;
    fdParams.ax = Complex(0, p.dz / (4.0f * p.dx * p.dx * k_0 * p.n_0));
    fdParams.ay = Complex(0, p.dz / (4.0f * p.dy * p.dy * k_0 * p.n_0));
    fdParams.d = -p.dz * k_0;
    fdParams.taperPerStep = (1.0f - p.taperScaling) / Nz;
    fdParams.twistPerStep = p.twistRate * p.Lz / Nz;
    fdParams.xSymmetry = p.xSymmetry;
    fdParams.ySymmetry = p.ySymmetry;
    fdParams.RoC = p.bendingRoC;
    fdParams.rho_e = p.rho_e;
    fdParams.bendDirection = p.bendDirection * M_PI / 180.0f;
    fdParams.n_mat = impl_->RI.n.data();
    fdParams.Nx_n = impl_->RI.Nx;
    fdParams.Ny_n = impl_->RI.Ny;
    fdParams.Nz_n = impl_->RI.Nz;
    fdParams.precisePower = 1.0;

    // Calculate edge absorber
    auto x = getGridArray(p.Nx, p.dx, p.ySymmetry);
    auto y = getGridArray(p.Ny, p.dy, p.xSymmetry);
    std::vector<float> multiplier(p.Nx * p.Ny);
    float xEdge = p.Lx * (1 + (p.ySymmetry != 0)) / 2.0f;
    float yEdge = p.Ly * (1 + (p.xSymmetry != 0)) / 2.0f;

    for (int iy = 0; iy < p.Ny; ++iy) {
        for (int ix = 0; ix < p.Nx; ++ix) {
            float dist = std::max(
                0.0f,
                std::max(std::abs(y[iy]) - yEdge, std::abs(x[ix]) - xEdge));
            multiplier[ix + iy * p.Nx] =
                std::exp(-p.dz * dist * dist * p.alpha);
        }
    }
    fdParams.multiplier = multiplier.data();

    // Copy initial field
    std::vector<Complex> E_current = impl_->E.field;

    result.powers.resize(p.updates + 1);
    result.z_positions.resize(p.updates + 1);
    result.powers[0] = 1.0f;
    result.z_positions[0] = 0.0f;

    // Propagate through each segment
    int iz_start = 0;
    for (size_t i = 0; i < updateIndices.size(); ++i) {
        int iz_end = updateIndices[i];

        FDBPMPropagator::propagateSegment(
            E_current.data(), p.Nx, p.Ny, iz_start, iz_end, fdParams, p.useGPU);

        // Calculate power correctly - account for symmetry
        float powerFraction =
            1.0f / (1.0f + (p.xSymmetry != 0)) / (1.0f + (p.ySymmetry != 0));
        float power = 0.0f;
        for (const auto& e : E_current) {
            power += std::norm(e);
        }
        result.powers[i + 1] = power / powerFraction;
        result.z_positions[i + 1] = iz_end * p.dz;

        // Update precisePower for next segment
        fdParams.precisePower = power;

        iz_start = iz_end;
    }

    result.finalField.field = E_current;
    result.finalField.Lx = p.Lx;
    result.finalField.Ly = p.Ly;
    result.finalField.xSymmetry = p.xSymmetry;
    result.finalField.ySymmetry = p.ySymmetry;

    result.finalRI = impl_->RI;

    return result;
}

PropagationResult BPMSolver::propagateFFTBPM()
{
    throw std::runtime_error("FFT-BPM not yet implemented");
}

std::vector<BPMSolver::Mode> BPMSolver::findModes(int nModes, bool sortByLoss)
{
    auto& p = impl_->params;

    // This is a simplified mode finder using power iteration / inverse
    // iteration For a full eigenmode solver, you'd need ARPACK or similar

    std::vector<Mode> modes;

    // Setup parameters
    float k_0 = 2.0f * M_PI / p.lambda;
    float dz = 1e-10f;  // Very small step for mode finding

    Complex ax = Complex(0, dz / (4.0f * p.dx * p.dx * k_0 * p.n_0));
    Complex ay = Complex(0, dz / (4.0f * p.dy * p.dy * k_0 * p.n_0));

    auto x = getGridArray(p.Nx, p.dx, p.ySymmetry);
    auto y = getGridArray(p.Ny, p.dy, p.xSymmetry);

    // Build propagation operator
    // This is a simplified version - full implementation would use eigs()

    // For now, throw informative error
    throw std::runtime_error(
        "findModes() requires eigenvalue solver (ARPACK/Spectra). "
        "This is a complex feature - for now, use MATLAB's BPM-Matlab for mode "
        "finding, "
        "then import the modes using setE()");
}

RUZINO_NAMESPACE_CLOSE_SCOPE
