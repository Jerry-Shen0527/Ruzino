#include <BPM/BPM.h>
#include <cuda_runtime.h>
#include <thrust/complex.h>

RUZINO_NAMESPACE_OPEN_SCOPE

#define TILE_DIM 32

using CudaComplex = thrust::complex<float>;

__global__ void substep1a_cuda_kernel(
    CudaComplex* E1,
    CudaComplex* E2,
    CudaComplex* Eyx,
    int Nx, int Ny,
    CudaComplex ax, CudaComplex ay,
    uint8_t xSymmetry, uint8_t ySymmetry
) {
    __shared__ CudaComplex tile[TILE_DIM][TILE_DIM+1];

    bool xAntiSymm = xSymmetry == 2;
    bool yAntiSymm = ySymmetry == 2;

    int xTiles = (Nx + TILE_DIM - 1) / TILE_DIM;
    int yTiles = (Ny + TILE_DIM - 1) / TILE_DIM;

    for (int tileNum = blockIdx.x; tileNum < xTiles * yTiles; tileNum += gridDim.x) {
        int tilexoffset = TILE_DIM * (tileNum % xTiles);
        int tileyoffset = TILE_DIM * (tileNum / xTiles);
        int ix = tilexoffset + threadIdx.x;
        int iy = tileyoffset + threadIdx.y;

        if (ix < Nx && iy < Ny) {
            int i = ix + iy * Nx;
            tile[threadIdx.x][threadIdx.y] = E1[i];

            if (ix != 0)
                tile[threadIdx.x][threadIdx.y] += (E1[i-1] - E1[i]) * ax;
            if (ix != Nx-1 && (!yAntiSymm || ix != 0))
                tile[threadIdx.x][threadIdx.y] += (E1[i+1] - E1[i]) * ax;
            if (iy != 0)
                tile[threadIdx.x][threadIdx.y] += (E1[i-Nx] - E1[i]) * ay * 2.0f;
            if (iy != Ny-1 && (!xAntiSymm || iy != 0))
                tile[threadIdx.x][threadIdx.y] += (E1[i+Nx] - E1[i]) * ay * 2.0f;
        }
        __syncthreads();

        // Transpose to yx
        ix = tilexoffset + threadIdx.y;
        iy = tileyoffset + threadIdx.x;
        if (ix < Nx && iy < Ny) {
            Eyx[iy + ix * Ny] = tile[threadIdx.y][threadIdx.x];
        }
        __syncthreads();
    }
}

__global__ void substep1b_cuda_kernel(
    CudaComplex* Eyx,
    CudaComplex* b,
    int Nx, int Ny,
    CudaComplex ax,
    uint8_t ySymmetry
) {
    int threadNum = threadIdx.x + blockIdx.x * blockDim.x;
    bool yAntiSymm = ySymmetry == 2;

    for (int iy = threadNum; iy < Ny; iy += gridDim.x * blockDim.x) {
        // Forward sweep
        for (int ix = 0; ix < Nx; ++ix) {
            int i = iy + ix * Ny;

            if (ix == 0 && yAntiSymm)
                b[i] = 1.0f;
            else if (ix == 0)
                b[i] = 1.0f + ax;
            else if (ix < Nx-1)
                b[i] = 1.0f + 2.0f * ax;
            else
                b[i] = 1.0f + ax;

            if (ix > 0) {
                CudaComplex w = -ax / b[i - Ny];
                b[i] += w * (ix == 1 && yAntiSymm ? CudaComplex(0) : ax);
                Eyx[i] -= w * Eyx[i - Ny];
            }
        }

        // Backward sweep
        for (int ix = Nx-1; ix >= (yAntiSymm ? 1 : 0); --ix) {
            int i = iy + ix * Ny;
            Eyx[i] = (Eyx[i] + (ix == Nx-1 ? CudaComplex(0) : ax * Eyx[i + Ny])) / b[i];
        }
    }
}

__global__ void substep2a_cuda_kernel(
    CudaComplex* E1,
    CudaComplex* Eyx,
    CudaComplex* E2,
    int Nx, int Ny,
    CudaComplex ay,
    uint8_t xSymmetry
) {
    __shared__ CudaComplex tile[TILE_DIM][TILE_DIM+1];

    bool xAntiSymm = xSymmetry == 2;
    int xTiles = (Nx + TILE_DIM - 1) / TILE_DIM;
    int yTiles = (Ny + TILE_DIM - 1) / TILE_DIM;

    for (int tileNum = blockIdx.x; tileNum < xTiles * yTiles; tileNum += gridDim.x) {
        int tilexoffset = TILE_DIM * (tileNum % xTiles);
        int tileyoffset = TILE_DIM * (tileNum / xTiles);
        int ix = tilexoffset + threadIdx.y;
        int iy = tileyoffset + threadIdx.x;

        __syncthreads();
        if (ix < Nx && iy < Ny)
            tile[threadIdx.y][threadIdx.x] = Eyx[ix*Ny + iy];
        __syncthreads();

        ix = tilexoffset + threadIdx.x;
        iy = tileyoffset + threadIdx.y;
        if (ix < Nx && iy < Ny) {
            int i = ix + iy * Nx;
            CudaComplex deltaE = 0;
            if (iy != 0)
                deltaE -= (E1[i-Nx] - E1[i]) * ay;
            if (iy != Ny-1 && (!xAntiSymm || iy != 0))
                deltaE -= (E1[i+Nx] - E1[i]) * ay;
            E2[i] = tile[threadIdx.x][threadIdx.y] + deltaE;
        }
    }
}

__global__ void substep2b_cuda_kernel(
    CudaComplex* E2,
    CudaComplex* b,
    float* EfieldPower,
    int Nx, int Ny,
    CudaComplex ay,
    uint8_t xSymmetry
) {
    float powerThread = 0.0f;
    int threadNum = threadIdx.x + blockIdx.x * blockDim.x;
    bool xAntiSymm = xSymmetry == 2;

    for (int ix = threadNum; ix < Nx; ix += gridDim.x * blockDim.x) {
        // Forward sweep
        for (int iy = 0; iy < Ny; ++iy) {
            int i = ix + iy * Nx;

            if (iy == 0 && xAntiSymm)
                b[i] = 1.0f;
            else if (iy == 0)
                b[i] = 1.0f + ay;
            else if (iy < Ny-1)
                b[i] = 1.0f + 2.0f * ay;
            else
                b[i] = 1.0f + ay;

            if (iy > 0) {
                CudaComplex w = -ay / b[i-Nx];
                b[i] += w * (iy == 1 && xAntiSymm ? CudaComplex(0) : ay);
                E2[i] -= w * E2[i-Nx];
            }
        }

        // Backward sweep - compute power here
        for (int iy = Ny-1; iy >= (xAntiSymm ? 1 : 0); --iy) {
            int i = ix + iy * Nx;
            E2[i] = (E2[i] + (iy == Ny-1 ? CudaComplex(0) : ay * E2[i+Nx])) / b[i];
            powerThread += thrust::norm(E2[i]);
        }
    }

    atomicAdd(EfieldPower, powerThread);
}

__global__ void applyMultiplier_cuda_kernel(
    CudaComplex* E2,
    CudaComplex* n_out,
    CudaComplex* n_mat,
    float* multiplier,
    float* precisePowerDiff,
    int Nx, int Ny, int iz, int iz_end,
    float dx, float dy, float dz,
    float d, float n_0,
    double precisePower, float EfieldPower,
    uint8_t xSymmetry, uint8_t ySymmetry
) {
    float powerDiffThread = 0.0f;
    int threadNum = threadIdx.x + blockIdx.x * blockDim.x;
    float fieldCorrection = sqrtf(static_cast<float>(precisePower) / EfieldPower);

    for (int i = threadNum; i < Nx*Ny; i += gridDim.x * blockDim.x) {
        //int ix = i % Nx;  // unused
        //int iy = i / Nx;  // unused

        // Calculate coordinates (unused for now)
        //float x = dx * (ix - (Nx - 1) / 2.0f * (ySymmetry == 0));
        //float y = dy * (iy - (Ny - 1) / 2.0f * (xSymmetry == 0));

        CudaComplex n = n_mat[i];
        if (iz == iz_end - 1) n_out[i] = n;

        float n_real = n.real();
        float n_imag = n.imag();
        CudaComplex phase = thrust::exp(CudaComplex(0,
            d * (n_imag + (n_real*n_real - n_0*n_0) / (2.0f*n_0))));
        CudaComplex a = multiplier[i] * phase;

        E2[i] *= fieldCorrection * a;

        float a_norm_sqr = thrust::norm(a);
        if (fabsf(a_norm_sqr - 1.0f) < 10*FLT_EPSILON) a_norm_sqr = 1.0f;
        powerDiffThread += thrust::norm(E2[i]) * (1.0f - 1.0f/a_norm_sqr);
    }

    atomicAdd(precisePowerDiff, powerDiffThread);
}

namespace cuda {
    void substep1a_kernel(
        Complex* E1, Complex* E2, Complex* Eyx,
        const FDBPMPropagator::Parameters& p
    ) {
        int nBlocks = 256;
        dim3 blockDims(TILE_DIM, TILE_DIM);

        substep1a_cuda_kernel<<<nBlocks, blockDims>>>(
            reinterpret_cast<CudaComplex*>(E1),
            reinterpret_cast<CudaComplex*>(E2),
            reinterpret_cast<CudaComplex*>(Eyx),
            p.Nx, p.Ny,
            *reinterpret_cast<const CudaComplex*>(&p.ax),
            *reinterpret_cast<const CudaComplex*>(&p.ay),
            p.xSymmetry, p.ySymmetry
        );
    }

    void substep1b_kernel(
        Complex* Eyx, Complex* b,
        const FDBPMPropagator::Parameters& p
    ) {
        int nBlocks = 256;
        substep1b_cuda_kernel<<<nBlocks, 256>>>(
            reinterpret_cast<CudaComplex*>(Eyx),
            reinterpret_cast<CudaComplex*>(b),
            p.Nx, p.Ny,
            *reinterpret_cast<const CudaComplex*>(&p.ax),
            p.ySymmetry
        );
    }

    void substep2a_kernel(Complex* E1, Complex* Eyx, Complex* E2,
                          const FDBPMPropagator::Parameters& p) {
        int nBlocks = 256;
        dim3 blockDims(TILE_DIM, TILE_DIM);
        substep2a_cuda_kernel<<<nBlocks, blockDims>>>(
            reinterpret_cast<CudaComplex*>(E1),
            reinterpret_cast<CudaComplex*>(Eyx),
            reinterpret_cast<CudaComplex*>(E2),
            p.Nx, p.Ny,
            *reinterpret_cast<const CudaComplex*>(&p.ay),
            p.xSymmetry
        );
    }

    void substep2b_kernel(Complex* E2, Complex* b, float* EfieldPower,
                          const FDBPMPropagator::Parameters& p) {
        int nBlocks = 256;
        substep2b_cuda_kernel<<<nBlocks, 256>>>(
            reinterpret_cast<CudaComplex*>(E2),
            reinterpret_cast<CudaComplex*>(b),
            EfieldPower,
            p.Nx, p.Ny,
            *reinterpret_cast<const CudaComplex*>(&p.ay),
            p.xSymmetry
        );
        cudaDeviceSynchronize();
    }

    void applyMultiplier_kernel(Complex* E2, Complex* n_out,
                                const FDBPMPropagator::Parameters& p,
                                int iz, float* precisePowerDiff) {
        int nBlocks = 256;
        float EfieldPower = 0.0f; // This should be passed from substep2b

        applyMultiplier_cuda_kernel<<<nBlocks, 256>>>(
            reinterpret_cast<CudaComplex*>(E2),
            reinterpret_cast<CudaComplex*>(n_out),
            reinterpret_cast<CudaComplex*>(p.n_mat),
            p.multiplier,
            precisePowerDiff,
            p.Nx, p.Ny, iz, iz + 1,
            p.dx, p.dy, p.dz,
            p.d, p.n_0,
            p.precisePower, EfieldPower,
            p.xSymmetry, p.ySymmetry
        );
        cudaDeviceSynchronize();
    }
}

RUZINO_NAMESPACE_CLOSE_SCOPE