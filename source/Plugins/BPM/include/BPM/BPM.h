#pragma once

#include <complex>
#include <functional>
#include <memory>
#include <vector>

#include "api.h"

RUZINO_NAMESPACE_OPEN_SCOPE

using Complex = std::complex<float>;
using ComplexField = std::vector<Complex>;

// Core data structures
struct BPM_API GridParameters {
    int Nx, Ny;        // Grid dimensions
    float Lx, Ly, Lz;  // Physical dimensions [m]
    float dx, dy, dz;  // Grid spacing [m]
    int updates;       // Number of visualization updates

    // Symmetry: 0=none, 1=symmetric, 2=antisymmetric
    uint8_t xSymmetry = 0;
    uint8_t ySymmetry = 0;

    float lambda;        // Wavelength [m]
    float n_0;           // Reference refractive index
    float n_background;  // Background refractive index
    float alpha;         // Edge absorber coefficient

    // Advanced features
#ifdef __CUDACC__
    float bendingRoC = 1e38f;  // Use large float instead of INFINITY for CUDA
#else
    float bendingRoC = INFINITY;  // Radius of curvature for bending
#endif
    float bendDirection = 0.0f;  // Bending direction [deg]
    float taperScaling = 1.0f;   // Tapering factor
    float twistRate = 0.0f;      // Twist rate [rad/m]
    float rho_e = 0.22f;         // Elasto-optic coefficient

    bool useGPU = false;
    bool useAllCPUs = false;
};

struct BPM_API ElectricField {
    ComplexField field;  // E-field data
    float Lx, Ly;        // Domain size
    uint8_t xSymmetry = 0;
    uint8_t ySymmetry = 0;

    void resize(int nx, int ny)
    {
        field.resize(nx * ny);
    }
};

struct BPM_API RefractiveIndex {
    std::vector<Complex> n;  // Can be 2D or 3D
    float Lx, Ly;
    int Nx, Ny, Nz = 1;
    uint8_t xSymmetry = 0;
    uint8_t ySymmetry = 0;
};

struct BPM_API PropagationResult {
    ElectricField finalField;
    RefractiveIndex finalRI;
    std::vector<float> powers;       // Power at each update
    std::vector<float> z_positions;  // z positions
    std::vector<ComplexField> xzSlice;
    std::vector<ComplexField> yzSlice;
    std::vector<ComplexField> E3D;  // Optional 3D storage
};

// Main BPM solver class
class BPM_API BPMSolver {
   public:
    BPMSolver(const GridParameters& params);
    ~BPMSolver();

    // Initialize from functions
    void initializeRI(std::function<Complex(float, float)> riFunc);
    void initializeE(std::function<Complex(float, float)> eFunc);

    // Initialize from data
    void setRI(const RefractiveIndex& ri);
    void setE(const ElectricField& e);

    // Main propagation methods
    PropagationResult propagateFDBPM();
    PropagationResult propagateFFTBPM();

    // Mode finding
    struct Mode {
        ComplexField field;
        Complex neff;
        std::string label;
    };
    std::vector<Mode> findModes(int nModes, bool sortByLoss = false);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// FD-BPM specific implementation
class BPM_API FDBPMPropagator {
   public:
    struct Parameters {
        int Nx, Ny;  // Grid dimensions
        float dx, dy, dz;
        Complex ax, ay;  // Finite difference coefficients
        float d;         // Phase accumulation coefficient
        float n_0;       // Reference refractive index
        float taperPerStep;
        float twistPerStep;
        float* multiplier;  // Edge absorber
        Complex* n_mat;     // Refractive index
        int Nx_n, Ny_n, Nz_n;
        float dz_n;
        uint8_t xSymmetry, ySymmetry;
        float RoC, rho_e, bendDirection;
        double precisePower;
    };

    static void propagateSegment(
        Complex* E,
        int Nx,
        int Ny,
        int iz_start,
        int iz_end,
        const Parameters& params,
        bool useGPU = false);

   private:
    // Douglas-Gunn ADI substeps
    static void
    substep1a(Complex* E1, Complex* E2, const Parameters& p, int Nx, int Ny);
    static void
    substep1b(Complex* E2, Complex* b, const Parameters& p, int Nx, int Ny);
    static void
    substep2a(Complex* E1, Complex* E2, const Parameters& p, int Nx, int Ny);
    static void
    substep2b(Complex* E2, Complex* b, const Parameters& p, int Nx, int Ny);
    static void
    applyMultiplier(Complex* E, const Parameters& p, int iz, int Nx, int Ny);

    // Thomson algorithm for tridiagonal systems
    static void solveTridiagonal(
        Complex* a,
        Complex* b,
        Complex* c,
        Complex* d,
        Complex* x,
        int n);
};

// FFT-BPM specific implementation
class BPM_API FFTBPMPropagator {
   public:
    struct Parameters {
        float dx, dy, dz;
        float lambda, n_0;
        float* absorber;
        int Nx, Ny;
    };

    static void
    propagateSegment(Complex* E, const Parameters& params, int nSteps);

   private:
    static void
    applyFresnelKernel(Complex* E, const Complex* kernel, int Nx, int Ny);
};

// Utility functions
BPM_API std::vector<float> getGridArray(int N, float delta, uint8_t symmetry);
BPM_API void calcFullField(
    const std::vector<float>& x,
    const std::vector<float>& y,
    const ComplexField& field,
    std::vector<float>& x_full,
    std::vector<float>& y_full,
    ComplexField& field_full);

// CUDA kernels (when useGPU = true)
#ifdef __CUDACC__
namespace cuda {
void substep1a_kernel(
    Complex* E1,
    Complex* E2,
    Complex* Eyx,
    const FDBPMPropagator::Parameters& p);
void substep1b_kernel(
    Complex* Eyx,
    Complex* b,
    const FDBPMPropagator::Parameters& p);
void substep2a_kernel(
    Complex* E1,
    Complex* Eyx,
    Complex* E2,
    const FDBPMPropagator::Parameters& p);
void substep2b_kernel(
    Complex* E2,
    Complex* b,
    float* EfieldPower,
    const FDBPMPropagator::Parameters& p);
void applyMultiplier_kernel(
    Complex* E2,
    Complex* n_out,
    const FDBPMPropagator::Parameters& p,
    int iz,
    float* precisePowerDiff);
}  // namespace cuda
#endif

RUZINO_NAMESPACE_CLOSE_SCOPE
