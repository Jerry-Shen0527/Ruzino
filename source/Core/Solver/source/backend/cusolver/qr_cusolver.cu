#include <cuda_runtime.h>
#include <cusolverSp.h>
#include <cusparse.h>

#include <RHI/cuda.hpp>
#include <RHI/internal/cuda_extension.hpp>
#include <RZSolver/Solver.hpp>
#include <iostream>

RUZINO_NAMESPACE_OPEN_SCOPE

namespace Solver {

namespace {
    // Helper to create or resize buffer
    void ensureBufferSize(Ruzino::cuda::CUDALinearBufferHandle& buffer, size_t size, size_t& current_size) {
        if (!buffer || current_size < size) {
            // Buffer doesn't exist or is too small
            // For now, simple recreation. In a more advanced system, we might resize.
            // But CUDALinearBuffer doesn't support resize, so we recreate.
            // We allocate slightly more to avoid frequent reallocations if size grows slowly
            size_t alloc_size = (size_t)(size * 1.2);

            // Create a temporary buffer with the right size effectively
            // Since we can't easily create a raw buffer without type, we use char/byte buffer
            // But here we rely on the specific create_cuda_linear_buffer overloads
            // This is a bit tricky with the current API being type-safe wrapping
            // So we will just use the exact size for now to be safe with the API
            // The caller is responsible for creating the right type of buffer
        }
    }
}  // namespace

class CuSolverQRSolver : public LinearSolver {
   private:
    cusolverSpHandle_t cusolverHandle;
    cusparseHandle_t cusparseHandle;
    bool initialized = false;

    // Cached buffers for solve() method (Eigen input)
    // We cache these to avoid reallocation when solve() is called repeatedly with same size matrix
    int cached_nnz = 0;
    int cached_n = 0;
    Ruzino::cuda::CUDALinearBufferHandle d_csrVal_cached;
    Ruzino::cuda::CUDALinearBufferHandle d_csrRowPtr_cached;
    Ruzino::cuda::CUDALinearBufferHandle d_csrColInd_cached;
    Ruzino::cuda::CUDALinearBufferHandle d_b_cached;
    Ruzino::cuda::CUDALinearBufferHandle d_x_cached;

   public:
    CuSolverQRSolver()
    {
        if (cusolverSpCreate(&cusolverHandle) != CUSOLVER_STATUS_SUCCESS ||
            cusparseCreate(&cusparseHandle) != CUSPARSE_STATUS_SUCCESS) {
            throw std::runtime_error("Failed to create cuSOLVER/cuSPARSE handles");
        }
        initialized = true;
    }

    ~CuSolverQRSolver()
    {
        if (initialized) {
            cusolverSpDestroy(cusolverHandle);
            cusparseDestroy(cusparseHandle);
        }
    }

    std::string getName() const override
    {
        return "cuSOLVER QR (Direct)";
    }

    bool isIterative() const override
    {
        return false;  // 直接法!
    }

    bool requiresGPU() const override
    {
        return true;
    }

    // Direct GPU interface implementation
    SolverResult solveGPU(
        int n,
        int nnz,
        const int* d_row_offsets,
        const int* d_col_indices,
        const float* d_values,
        const float* d_b,
        float* d_x,
        const SolverConfig& config = SolverConfig{}) override
    {
        SolverResult result;
        auto start_time = std::chrono::high_resolution_clock::now();

        try {
            // Create cuSOLVER matrix descriptor
            cusparseMatDescr_t descrA;
            cusparseCreateMatDescr(&descrA);
            cusparseSetMatType(descrA, CUSPARSE_MATRIX_TYPE_GENERAL);
            cusparseSetMatIndexBase(descrA, CUSPARSE_INDEX_BASE_ZERO);

            int singularity = 0;

            // Call cuSOLVER QR solver
            // Note: cusolverSpScsrlsvqr solves A*x = b
            cusolverStatus_t status = cusolverSpScsrlsvqr(
                cusolverHandle,
                n,
                nnz,
                descrA,
                d_values,
                d_row_offsets,
                d_col_indices,
                d_b,
                config.tolerance,
                0,  // no reordering
                d_x,
                &singularity);

            cusparseDestroyMatDescr(descrA);

            if (status != CUSOLVER_STATUS_SUCCESS) {
                result.converged = false;
                result.error_message = "cuSOLVER QR failed with status " + std::to_string(status);
            } else if (singularity >= 0) {
                result.converged = false;
                result.error_message = "Matrix is singular at column " + std::to_string(singularity);
                if (config.verbose) {
                    std::cout << result.error_message << std::endl;
                }
            } else {
                result.converged = true;
                result.iterations = 1;  // Direct solver, single "iteration"
                result.final_residual = 0.0f;  // Direct solver

                if (config.verbose) {
                    std::cout << "cuSOLVER QR direct solve completed successfully" << std::endl;
                }
            }

        } catch (const std::exception& e) {
            result.converged = false;
            result.error_message = std::string("cuSOLVER QR error: ") + e.what();
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        result.solve_time =
            std::chrono::duration_cast<std::chrono::microseconds>(
                end_time - start_time);

        return result;
    }

    SolverResult solve(
        const Eigen::SparseMatrix<float>& A,
        const Eigen::VectorXf& b,
        Eigen::VectorXf& x,
        const SolverConfig& config = SolverConfig{}) override
    {
        auto start_time = std::chrono::high_resolution_clock::now();
        SolverResult result;

        try {
            int n = A.rows();
            int nnz = A.nonZeros();

            // Reallocate cached buffers if size changed
            if (n != cached_n || nnz != cached_nnz || !d_csrVal_cached) {
                cached_n = n;
                cached_nnz = nnz;

                Ruzino::cuda::CUDALinearBufferDesc val_desc, rowptr_desc, colind_desc, vec_desc;
                val_desc.element_count = nnz;
                val_desc.element_size = sizeof(float);
                rowptr_desc.element_count = n + 1;
                rowptr_desc.element_size = sizeof(int);
                colind_desc.element_count = nnz;
                colind_desc.element_size = sizeof(int);
                vec_desc.element_count = n;
                vec_desc.element_size = sizeof(float);

                d_csrVal_cached = Ruzino::cuda::create_cuda_linear_buffer(val_desc);
                d_csrRowPtr_cached = Ruzino::cuda::create_cuda_linear_buffer(rowptr_desc);
                d_csrColInd_cached = Ruzino::cuda::create_cuda_linear_buffer(colind_desc);
                d_b_cached = Ruzino::cuda::create_cuda_linear_buffer(vec_desc);
                d_x_cached = Ruzino::cuda::create_cuda_linear_buffer(vec_desc);
            }

            // Convert to CSR format on host
            // (Note: For max performance, this conversion should happen on GPU or use Cached direct interface)
            Eigen::SparseMatrix<float, Eigen::RowMajor> A_csr = A;

            // We still use host vectors for the transfer, but we could optimize this further
            // if we had access to Eigen internal buffers or if we did the conversion on GPU
            std::vector<float> csrVal(nnz);
            std::vector<int> csrRowPtr(n + 1);
            std::vector<int> csrColInd(nnz);

            // Copy CSR data
            int idx = 0;
            csrRowPtr[0] = 0;
            for (int i = 0; i < n; ++i) {
                for (Eigen::SparseMatrix<float, Eigen::RowMajor>::InnerIterator it(A_csr, i); it; ++it) {
                    csrVal[idx] = it.value();
                    csrColInd[idx] = it.col();
                    idx++;
                }
                csrRowPtr[i + 1] = idx;
            }

            // Debug output
            if (config.verbose) {
                std::cout << "Matrix size: " << n << "x" << n << std::endl;
                std::cout << "Total nnz: " << nnz << std::endl;
            }

            // Copy to device (using cached buffers)
            cudaMemcpy(
                (void*)d_csrVal_cached->get_device_ptr(),
                csrVal.data(),
                nnz * sizeof(float),
                cudaMemcpyHostToDevice);
            cudaMemcpy(
                (void*)d_csrRowPtr_cached->get_device_ptr(),
                csrRowPtr.data(),
                (n + 1) * sizeof(int),
                cudaMemcpyHostToDevice);
            cudaMemcpy(
                (void*)d_csrColInd_cached->get_device_ptr(),
                csrColInd.data(),
                nnz * sizeof(int),
                cudaMemcpyHostToDevice);
            cudaMemcpy(
                (void*)d_b_cached->get_device_ptr(),
                b.data(),
                n * sizeof(float),
                cudaMemcpyHostToDevice);
            cudaMemcpy(
                (void*)d_x_cached->get_device_ptr(),
                x.data(),
                n * sizeof(float),
                cudaMemcpyHostToDevice);

            // Call GPU implementation
            result = solveGPU(
                n,
                nnz,
                reinterpret_cast<const int*>(d_csrRowPtr_cached->get_device_ptr()),
                reinterpret_cast<const int*>(d_csrColInd_cached->get_device_ptr()),
                reinterpret_cast<const float*>(d_csrVal_cached->get_device_ptr()),
                reinterpret_cast<const float*>(d_b_cached->get_device_ptr()),
                reinterpret_cast<float*>(d_x_cached->get_device_ptr()),
                config);

            // Copy result back
            cudaMemcpy(
                x.data(),
                (void*)d_x_cached->get_device_ptr(),
                n * sizeof(float),
                cudaMemcpyDeviceToHost);

        } catch (const std::exception& e) {
            result.converged = false;
            result.error_message = std::string("cuSOLVER QR error: ") + e.what();
        }

        // Add host overhead to timing if needed, but result.solve_time currently reflects GPU time

        return result;
    }
};

// Factory registration
std::unique_ptr<LinearSolver> createCuSolverQRSolver()
{
    return std::make_unique<CuSolverQRSolver>();
}

}  // namespace Solver

RUZINO_NAMESPACE_CLOSE_SCOPE
