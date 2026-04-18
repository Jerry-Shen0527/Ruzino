#include <cuda_runtime.h>
#include <cudss.h>
#include <cusolverDn.h>

#include <RHI/cuda.hpp>
#include <RHI/internal/cuda_extension.hpp>
#include <RZSolver/Solver.hpp>
#include <iostream>

RUZINO_NAMESPACE_OPEN_SCOPE

namespace Solver {

class CuSolverCholeskySolver : public LinearSolver {
   private:
    cudssHandle_t cudssHandle = nullptr;
    cusolverDnHandle_t cusolverDnHandle = nullptr;
    bool initialized = false;

    // Cached buffers for solve() method (Eigen input)
    int cached_nnz = 0;
    int cached_n = 0;
    Ruzino::cuda::CUDALinearBufferHandle d_csrVal_cached;
    Ruzino::cuda::CUDALinearBufferHandle d_csrRowPtr_cached;
    Ruzino::cuda::CUDALinearBufferHandle d_csrColInd_cached;
    Ruzino::cuda::CUDALinearBufferHandle d_b_cached;
    Ruzino::cuda::CUDALinearBufferHandle d_x_cached;

    // Cached buffers for solveDenseGPU() method
    int cached_dense_n = 0;
    int cached_lwork = 0;
    float* d_work_cached = nullptr;
    int* d_info_cached = nullptr;
    float* d_A_copy_cached = nullptr;

   public:
    CuSolverCholeskySolver()
    {
        if (cudssCreate(&cudssHandle) != CUDSS_STATUS_SUCCESS) {
            throw std::runtime_error("Failed to create cuDSS handle");
        }
        if (cusolverDnCreate(&cusolverDnHandle) != CUSOLVER_STATUS_SUCCESS) {
            cudssDestroy(cudssHandle);
            throw std::runtime_error("Failed to create cuSOLVER DN handle");
        }
        initialized = true;
    }

    ~CuSolverCholeskySolver()
    {
        if (initialized) {
            cudssDestroy(cudssHandle);
            cusolverDnDestroy(cusolverDnHandle);

            if (d_work_cached)
                cudaFree(d_work_cached);
            if (d_info_cached)
                cudaFree(d_info_cached);
            if (d_A_copy_cached)
                cudaFree(d_A_copy_cached);
        }
    }

    std::string getName() const override
    {
        return "cuSOLVER Cholesky (Direct)";
    }

    bool isIterative() const override
    {
        return false;
    }

    bool requiresGPU() const override
    {
        return true;
    }

    // Sparse GPU interface using cuDSS
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
            cudssMatrix_t matA = nullptr, matX = nullptr, matB = nullptr;
            cudssConfig_t solverConfig = nullptr;
            cudssData_t solverData = nullptr;

            auto cleanup = [&]() {
                if (matA) cudssMatrixDestroy(matA);
                if (matX) cudssMatrixDestroy(matX);
                if (matB) cudssMatrixDestroy(matB);
                if (solverConfig) cudssConfigDestroy(solverConfig);
                if (solverData) cudssDataDestroy(cudssHandle, solverData);
            };

            // Create CSR matrix (SPD type for Cholesky)
            cudssStatus_t status = cudssMatrixCreateCsr(
                &matA, n, n, nnz,
                const_cast<int*>(d_row_offsets),
                nullptr,
                const_cast<int*>(d_col_indices),
                const_cast<float*>(d_values),
                CUDA_R_32I, CUDA_R_32F,
                CUDSS_MTYPE_SPD, CUDSS_MVIEW_FULL, CUDSS_BASE_ZERO);

            if (status != CUDSS_STATUS_SUCCESS) {
                result.converged = false;
                result.error_message = "cuDSS matrix create failed: " + std::to_string(status);
                cleanup();
                goto done;
            }

            status = cudssMatrixCreateDn(&matX, n, 1, n, d_x, CUDA_R_32F, CUDSS_LAYOUT_COL_MAJOR);
            if (status != CUDSS_STATUS_SUCCESS) {
                result.converged = false;
                result.error_message = "cuDSS solution matrix create failed";
                cleanup();
                goto done;
            }

            status = cudssMatrixCreateDn(&matB, n, 1, n, const_cast<float*>(d_b), CUDA_R_32F, CUDSS_LAYOUT_COL_MAJOR);
            if (status != CUDSS_STATUS_SUCCESS) {
                result.converged = false;
                result.error_message = "cuDSS RHS matrix create failed";
                cleanup();
                goto done;
            }

            cudssConfigCreate(&solverConfig);
            cudssDataCreate(cudssHandle, &solverData);

            // Analysis
            status = cudssExecute(cudssHandle,
                CUDSS_PHASE_ANALYSIS, solverConfig, solverData, matA, matX, matB);
            if (status != CUDSS_STATUS_SUCCESS) {
                result.converged = false;
                result.error_message = "cuDSS analysis failed: " + std::to_string(status);
                cleanup();
                goto done;
            }

            // Factorization
            status = cudssExecute(cudssHandle,
                CUDSS_PHASE_FACTORIZATION, solverConfig, solverData, matA, matX, matB);
            if (status != CUDSS_STATUS_SUCCESS) {
                result.converged = false;
                result.error_message = "cuDSS factorization failed: " + std::to_string(status);
                cleanup();
                goto done;
            }

            // Solve
            status = cudssExecute(cudssHandle,
                CUDSS_PHASE_SOLVE, solverConfig, solverData, matA, matX, matB);
            if (status != CUDSS_STATUS_SUCCESS) {
                result.converged = false;
                result.error_message = "cuDSS solve failed: " + std::to_string(status);
                cleanup();
                goto done;
            }

            result.converged = true;
            result.iterations = 1;
            result.final_residual = 0.0f;

            if (config.verbose) {
                std::cout << "cuDSS Cholesky direct solve completed successfully" << std::endl;
            }

            cleanup();

        } catch (const std::exception& e) {
            result.converged = false;
            result.error_message = std::string("cuDSS Cholesky error: ") + e.what();
        }

done:
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

            Eigen::SparseMatrix<float, Eigen::RowMajor> A_csr = A;

            std::vector<float> csrVal(nnz);
            std::vector<int> csrRowPtr(n + 1);
            std::vector<int> csrColInd(nnz);

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

            if (config.verbose) {
                std::cout << "Matrix size: " << n << "x" << n << std::endl;
                std::cout << "Total nnz: " << nnz << std::endl;
            }

            cudaMemcpy((void*)d_csrVal_cached->get_device_ptr(), csrVal.data(), nnz * sizeof(float), cudaMemcpyHostToDevice);
            cudaMemcpy((void*)d_csrRowPtr_cached->get_device_ptr(), csrRowPtr.data(), (n + 1) * sizeof(int), cudaMemcpyHostToDevice);
            cudaMemcpy((void*)d_csrColInd_cached->get_device_ptr(), csrColInd.data(), nnz * sizeof(int), cudaMemcpyHostToDevice);
            cudaMemcpy((void*)d_b_cached->get_device_ptr(), b.data(), n * sizeof(float), cudaMemcpyHostToDevice);
            cudaMemcpy((void*)d_x_cached->get_device_ptr(), x.data(), n * sizeof(float), cudaMemcpyHostToDevice);

            result = solveGPU(
                n, nnz,
                reinterpret_cast<const int*>(d_csrRowPtr_cached->get_device_ptr()),
                reinterpret_cast<const int*>(d_csrColInd_cached->get_device_ptr()),
                reinterpret_cast<const float*>(d_csrVal_cached->get_device_ptr()),
                reinterpret_cast<const float*>(d_b_cached->get_device_ptr()),
                reinterpret_cast<float*>(d_x_cached->get_device_ptr()),
                config);

            if (result.converged) {
                cudaMemcpy(x.data(), (void*)d_x_cached->get_device_ptr(), n * sizeof(float), cudaMemcpyDeviceToHost);
            }
        }
        catch (const std::exception& e) {
            result.converged = false;
            result.error_message = std::string("cuDSS Cholesky error: ") + e.what();
        }

        return result;
    }

    // Dense matrix GPU interface using cusolverDn (unchanged)
    SolverResult solveDenseGPU(
        int n,
        const float* d_A,
        const float* d_b,
        float* d_x,
        const SolverConfig& config = SolverConfig{}) override
    {
        SolverResult result;
        auto start_time = std::chrono::high_resolution_clock::now();

        try {
            int lwork = 0;
            cusolverStatus_t status = cusolverDnSpotrf_bufferSize(
                cusolverDnHandle,
                CUBLAS_FILL_MODE_LOWER,
                n,
                const_cast<float*>(d_A),
                n,
                &lwork);

            if (status != CUSOLVER_STATUS_SUCCESS) {
                result.converged = false;
                result.error_message = "Failed to query Cholesky workspace size: " + std::to_string(status);
                return result;
            }

            if (n != cached_dense_n || lwork != cached_lwork) {
                if (d_work_cached)
                    cudaFree(d_work_cached);
                if (d_A_copy_cached)
                    cudaFree(d_A_copy_cached);

                cudaMalloc(&d_work_cached, lwork * sizeof(float));
                cudaMalloc(&d_A_copy_cached, n * n * sizeof(float));

                cached_dense_n = n;
                cached_lwork = lwork;
            }

            if (!d_info_cached) {
                cudaMalloc(&d_info_cached, sizeof(int));
            }

            cudaMemcpy(d_A_copy_cached, d_A, n * n * sizeof(float), cudaMemcpyDeviceToDevice);

            status = cusolverDnSpotrf(
                cusolverDnHandle, CUBLAS_FILL_MODE_LOWER, n,
                d_A_copy_cached, n, d_work_cached, lwork, d_info_cached);

            if (status != CUSOLVER_STATUS_SUCCESS) {
                result.converged = false;
                result.error_message = "cusolverDnSpotrf failed: " + std::to_string(status);
                return result;
            }

            cudaMemcpy(d_x, d_b, n * sizeof(float), cudaMemcpyDeviceToDevice);

            status = cusolverDnSpotrs(
                cusolverDnHandle, CUBLAS_FILL_MODE_LOWER, n, 1,
                d_A_copy_cached, n, d_x, n, d_info_cached);

            if (status != CUSOLVER_STATUS_SUCCESS) {
                result.converged = false;
                result.error_message = "cusolverDnSpotrs failed: status=" + std::to_string(status);
                return result;
            }

            result.converged = true;
            result.iterations = 1;
            result.final_residual = 0.0f;

            if (config.verbose) {
                std::cout << "Dense Cholesky solve completed successfully (n=" << n << ")" << std::endl;
            }
        }
        catch (const std::exception& e) {
            result.converged = false;
            result.error_message = std::string("Dense Cholesky error: ") + e.what();
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        result.solve_time =
            std::chrono::duration_cast<std::chrono::microseconds>(
                end_time - start_time);

        return result;
    }
};

// Factory registration
std::unique_ptr<LinearSolver> createCuSolverCholeskySolver()
{
    return std::make_unique<CuSolverCholeskySolver>();
}

}  // namespace Solver

RUZINO_NAMESPACE_CLOSE_SCOPE
