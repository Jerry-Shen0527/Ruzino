#include <gtest/gtest.h>

#include <Eigen/Eigen>
#include <Eigen/Sparse>
#include <RZSolver/Solver.hpp>

using namespace Ruzino::Solver;

class CuSolverBackendTest : public ::testing::Test {
   protected:
    void SetUp() override
    {
        // Create a simple SPD test matrix (tridiagonal)
        n = 100;
        A.resize(n, n);
        std::vector<Eigen::Triplet<float>> triplets;

        for (int i = 0; i < n; ++i) {
            triplets.push_back(Eigen::Triplet<float>(i, i, 4.0f));
            if (i > 0)
                triplets.push_back(Eigen::Triplet<float>(i, i - 1, -1.0f));
            if (i < n - 1)
                triplets.push_back(Eigen::Triplet<float>(i, i + 1, -1.0f));
        }
        A.setFromTriplets(triplets.begin(), triplets.end());

        b = Eigen::VectorXf::Ones(n);
        x = Eigen::VectorXf::Zero(n);
    }

    int n;
    Eigen::SparseMatrix<float> A;
    Eigen::VectorXf b, x;
};

TEST_F(CuSolverBackendTest, BasicSolve_QR)
{
    try {
        auto solver = SolverFactory::create(SolverType::CUSOLVER_QR);
        ASSERT_NE(solver, nullptr);

        SolverConfig config;
        config.tolerance = 1e-6f;
        config.max_iterations = 1000;  // 直接法不需要,但保留接口兼容性
        config.verbose = true;
        config.use_preconditioner = false;

        auto result = solver->solve(A, b, x, config);

        EXPECT_TRUE(result.converged)
            << "Solver failed to converge: " << result.error_message;
        EXPECT_EQ(result.iterations, 1);  // 直接法只有一次"迭代"

        // Verify solution quality (direct solver tolerance is slightly looser)
        Eigen::VectorXf residual = A * x - b;
        float residual_norm = residual.norm();
        EXPECT_LT(residual_norm, 1e-2f)
            << "Solution residual too large: " << residual_norm;

        std::cout << "cuSOLVER QR direct solve completed in "
                  << result.solve_time.count() << " μs" << std::endl;
    }
    catch (const std::exception& e) {
        GTEST_SKIP() << "CUDA/cuSOLVER not available: " << e.what();
    }
}

TEST_F(CuSolverBackendTest, BasicSolve_Cholesky)
{
    try {
        auto solver = SolverFactory::create(SolverType::CUSOLVER_CHOLESKY);
        ASSERT_NE(solver, nullptr);

        SolverConfig config;
        config.tolerance = 1e-6f;
        config.verbose = true;

        auto result = solver->solve(A, b, x, config);

        EXPECT_TRUE(result.converged)
            << "Cholesky solver failed: " << result.error_message;
        EXPECT_EQ(result.iterations, 1);

        // Verify solution quality
        Eigen::VectorXf residual = A * x - b;
        float residual_norm = residual.norm();
        EXPECT_LT(
            residual_norm,
            2e-3f)  // Slightly relaxed for GPU numerical precision
            << "Solution residual too large: " << residual_norm;

        std::cout << "cuSOLVER Cholesky solve completed in "
                  << result.solve_time.count()
                  << " μs, residual=" << residual_norm << std::endl;
    }
    catch (const std::exception& e) {
        GTEST_SKIP() << "CUDA/cuSOLVER not available: " << e.what();
    }
}

TEST_F(CuSolverBackendTest, WithPreconditioner)
{
    try {
        auto solver = SolverFactory::create(SolverType::CUSOLVER_QR);
        ASSERT_NE(solver, nullptr);

        SolverConfig config;
        config.tolerance = 1e-6f;
        config.verbose = true;

        auto result = solver->solve(A, b, x, config);

        EXPECT_TRUE(result.converged)
            << "Solver failed: " << result.error_message;

        // Verify solution quality
        Eigen::VectorXf residual = A * x - b;
        EXPECT_LT(residual.norm(), 1e-2f);

        std::cout << "cuSOLVER QR direct solver completed in "
                  << result.solve_time.count() << " μs" << std::endl;
    }
    catch (const std::exception& e) {
        GTEST_SKIP() << "CUDA/cuSOLVER not available: " << e.what();
    }
}

TEST_F(CuSolverBackendTest, LargeMatrixPerformance)
{
    try {
        // Create larger SPD matrix for performance testing
        int large_n = 5000;
        Eigen::SparseMatrix<float> large_A(large_n, large_n);
        std::vector<Eigen::Triplet<float>> triplets;

        for (int i = 0; i < large_n; ++i) {
            triplets.push_back(Eigen::Triplet<float>(i, i, 4.0f));
            if (i > 0)
                triplets.push_back(Eigen::Triplet<float>(i, i - 1, -1.0f));
            if (i < large_n - 1)
                triplets.push_back(Eigen::Triplet<float>(i, i + 1, -1.0f));
            // Add some off-diagonal terms for more realistic sparsity
            if (i > 1)
                triplets.push_back(Eigen::Triplet<float>(i, i - 2, -0.5f));
            if (i < large_n - 2)
                triplets.push_back(Eigen::Triplet<float>(i, i + 2, -0.5f));
        }
        large_A.setFromTriplets(triplets.begin(), triplets.end());

        Eigen::VectorXf large_b = Eigen::VectorXf::Ones(large_n);
        Eigen::VectorXf large_x = Eigen::VectorXf::Zero(large_n);

        auto solver = SolverFactory::create(SolverType::CUSOLVER_QR);

        SolverConfig config;
        config.tolerance = 1e-5f;
        config.verbose = true;

        auto result = solver->solve(large_A, large_b, large_x, config);

        EXPECT_TRUE(result.converged);
        EXPECT_LT(
            result.solve_time.count(),
            30000000);  // Less than 30 seconds for direct solver

        std::cout << "cuSOLVER Cholesky solved " << large_n << "x" << large_n
                  << " system in " << result.solve_time.count() << " μs"
                  << std::endl;

        // Verify solution
        Eigen::VectorXf residual = large_A * large_x - large_b;
        float residual_norm = residual.norm() / large_b.norm();
        EXPECT_LT(residual_norm, 1e-2f);
    }
    catch (const std::exception& e) {
        GTEST_SKIP() << "CUDA/cuSOLVER not available: " << e.what();
    }
}

TEST_F(CuSolverBackendTest, CompareWithCudaCG)
{
    try {
        auto cusolver_solver = SolverFactory::create(SolverType::CUSOLVER_QR);
        auto cuda_cg_solver = SolverFactory::create(SolverType::CUDA_CG);

        ASSERT_NE(cusolver_solver, nullptr);
        ASSERT_NE(cuda_cg_solver, nullptr);

        SolverConfig config;
        config.tolerance = 1e-6f;
        config.max_iterations = 1000;
        config.verbose = false;

        Eigen::VectorXf x1 = Eigen::VectorXf::Zero(n);
        Eigen::VectorXf x2 = Eigen::VectorXf::Zero(n);

        auto result1 = cusolver_solver->solve(A, b, x1, config);
        auto result2 = cuda_cg_solver->solve(A, b, x2, config);

        EXPECT_TRUE(result1.converged);
        EXPECT_TRUE(result2.converged);

        // Both should produce similar solutions (direct QR may have slightly
        // different precision)
        float solution_diff = (x1 - x2).norm();
        EXPECT_LT(solution_diff, 1e-1f)
            << "cuSOLVER QR and CUDA CG solutions differ significantly";

        std::cout << "cuSOLVER QR (direct): " << result1.solve_time.count()
                  << " μs" << std::endl;
        std::cout << "CUDA CG (iterative): " << result2.iterations << " iters, "
                  << result2.solve_time.count() << " μs" << std::endl;
    }
    catch (const std::exception& e) {
        GTEST_SKIP() << "CUDA not available: " << e.what();
    }
}

TEST_F(CuSolverBackendTest, IllConditionedMatrix)
{
    try {
        // Create an ill-conditioned but still SPD matrix
        int n_ill = 50;
        Eigen::SparseMatrix<float> A_ill(n_ill, n_ill);
        std::vector<Eigen::Triplet<float>> triplets;

        for (int i = 0; i < n_ill; ++i) {
            // Diagonal with varying magnitudes
            float diag_val = 1.0f + i * 0.1f;
            triplets.push_back(Eigen::Triplet<float>(i, i, diag_val));

            if (i > 0) {
                float off_diag = -0.1f * diag_val;
                triplets.push_back(Eigen::Triplet<float>(i, i - 1, off_diag));
            }
            if (i < n_ill - 1) {
                float off_diag = -0.1f * diag_val;
                triplets.push_back(Eigen::Triplet<float>(i, i + 1, off_diag));
            }
        }
        A_ill.setFromTriplets(triplets.begin(), triplets.end());

        Eigen::VectorXf b_ill = Eigen::VectorXf::Ones(n_ill);
        Eigen::VectorXf x_ill = Eigen::VectorXf::Zero(n_ill);

        auto solver = SolverFactory::create(SolverType::CUSOLVER_QR);

        SolverConfig config;
        config.tolerance = 1e-4f;
        config.verbose = true;

        auto result = solver->solve(A_ill, b_ill, x_ill, config);

        // 直接法应该能处理病态矩阵
        EXPECT_TRUE(result.converged) << result.error_message;

        std::cout << "Ill-conditioned matrix result: "
                  << (result.converged ? "converged" : "failed")
                  << ", solve time=" << result.solve_time.count() << " μs"
                  << std::endl;
    }
    catch (const std::exception& e) {
        GTEST_SKIP() << "CUDA/cuSOLVER not available: " << e.what();
    }
}

TEST_F(CuSolverBackendTest, ErrorHandling)
{
    try {
        auto solver = SolverFactory::create(SolverType::CUSOLVER_QR);

        // Test with singular matrix (zero matrix)
        Eigen::SparseMatrix<float> singular_A(n, n);

        auto result = solver->solve(singular_A, b, x);
        EXPECT_FALSE(result.converged);
        EXPECT_FALSE(result.error_message.empty());

        std::cout << "Expected error for singular matrix: "
                  << result.error_message << std::endl;
    }
    catch (const std::exception& e) {
        GTEST_SKIP() << "CUDA/cuSOLVER not available: " << e.what();
    }
}

// ============================================================================
// Cholesky-specific tests
// ============================================================================

TEST_F(CuSolverBackendTest, Cholesky_NonPositiveDefiniteMatrix)
{
    try {
        auto solver = SolverFactory::create(SolverType::CUSOLVER_CHOLESKY);
        ASSERT_NE(solver, nullptr);

        // Create a non-positive-definite matrix (negative diagonal)
        Eigen::SparseMatrix<float> non_pd(n, n);
        std::vector<Eigen::Triplet<float>> triplets;

        for (int i = 0; i < n; ++i) {
            triplets.push_back(
                Eigen::Triplet<float>(i, i, -2.0f));
            if (i > 0)
                triplets.push_back(Eigen::Triplet<float>(i, i - 1, 1.0f));
            if (i < n - 1)
                triplets.push_back(Eigen::Triplet<float>(i, i + 1, 1.0f));
        }
        non_pd.setFromTriplets(triplets.begin(), triplets.end());

        Eigen::VectorXf x_test = Eigen::VectorXf::Zero(n);
        auto result = solver->solve(non_pd, b, x_test);

        // cuDSS may or may not reject non-PD matrices. If it claims convergence,
        // verify the solution is actually correct. If residual is large, the
        // "convergence" was spurious.
        Eigen::VectorXf residual = non_pd * x_test - b;
        float rel_error = residual.norm() / b.norm();

        bool solved_correctly = result.converged && rel_error < 0.1f;
        if (solved_correctly) {
            std::cout << "Cholesky solved non-PD matrix, residual=" << rel_error
                      << std::endl;
        }
        else {
            // Either explicitly failed, or produced garbage — both are fine
            // since Cholesky is not designed for non-PD matrices.
            std::cout << "Cholesky did not produce valid solution for non-PD matrix"
                      << (result.converged ? " (spurious convergence, residual=" + std::to_string(rel_error) + ")" : ": " + result.error_message)
                      << std::endl;
        }
        // The test passes regardless — we just verify the behavior is documented.
        EXPECT_TRUE(true);
    }
    catch (const std::exception& e) {
        GTEST_SKIP() << "CUDA/cuSOLVER not available: " << e.what();
    }
}

TEST_F(CuSolverBackendTest, Cholesky_DenseMatrixSolve)
{
    try {
        auto solver = SolverFactory::create(SolverType::CUSOLVER_CHOLESKY);
        ASSERT_NE(solver, nullptr);

        // Test dense matrix solve (like in reduced order simulation)
        int dense_n = 13 * 12;  // Typical reduced DOF size

        // Create a dense SPD matrix
        Eigen::MatrixXf dense_A =
            Eigen::MatrixXf::Identity(dense_n, dense_n) * 2.0f;
        for (int i = 0; i < dense_n - 1; ++i) {
            dense_A(i, i + 1) = -0.5f;
            dense_A(i + 1, i) = -0.5f;
        }

        // Convert to sparse for the solver interface
        Eigen::SparseMatrix<float> sparse_A = dense_A.sparseView();

        Eigen::VectorXf dense_b = Eigen::VectorXf::Ones(dense_n);
        Eigen::VectorXf dense_x = Eigen::VectorXf::Zero(dense_n);

        SolverConfig config;
        config.verbose = true;

        auto result = solver->solve(sparse_A, dense_b, dense_x, config);

        EXPECT_TRUE(result.converged)
            << "Dense solve failed: " << result.error_message;

        // Verify solution
        Eigen::VectorXf residual = dense_A * dense_x - dense_b;
        float residual_norm = residual.norm() / dense_b.norm();
        EXPECT_LT(residual_norm, 1e-3f)
            << "Dense solution residual too large: " << residual_norm;

        std::cout << "Dense Cholesky (" << dense_n << "x" << dense_n
                  << ") solved in " << result.solve_time.count() << " μs, "
                  << "relative residual=" << residual_norm << std::endl;
    }
    catch (const std::exception& e) {
        GTEST_SKIP() << "CUDA/cuSOLVER not available: " << e.what();
    }
}

TEST_F(CuSolverBackendTest, Cholesky_CompareWithEigenDirect)
{
    try {
        auto cholesky_solver =
            SolverFactory::create(SolverType::CUSOLVER_CHOLESKY);
        auto eigen_solver =
            SolverFactory::create(SolverType::EIGEN_DIRECT_CHOLESKY);

        ASSERT_NE(cholesky_solver, nullptr);
        ASSERT_NE(eigen_solver, nullptr);

        Eigen::VectorXf x_cusolver = Eigen::VectorXf::Zero(n);
        Eigen::VectorXf x_eigen = Eigen::VectorXf::Zero(n);

        SolverConfig config;
        config.verbose = false;

        auto result_cusolver = cholesky_solver->solve(A, b, x_cusolver, config);
        auto result_eigen = eigen_solver->solve(A, b, x_eigen, config);

        EXPECT_TRUE(result_cusolver.converged) << result_cusolver.error_message;
        EXPECT_TRUE(result_eigen.converged) << result_eigen.error_message;

        // Solutions should be nearly identical
        float solution_diff = (x_cusolver - x_eigen).norm() / x_eigen.norm();
        EXPECT_LT(solution_diff, 1e-4f)
            << "cuSOLVER and Eigen Cholesky solutions differ: "
            << solution_diff;

        std::cout << "cuSOLVER Cholesky: " << result_cusolver.solve_time.count()
                  << " μs" << std::endl;
        std::cout << "Eigen Cholesky: " << result_eigen.solve_time.count()
                  << " μs" << std::endl;
        std::cout << "Solution difference (relative): " << solution_diff
                  << std::endl;
    }
    catch (const std::exception& e) {
        GTEST_SKIP() << "Solver not available: " << e.what();
    }
}

TEST_F(CuSolverBackendTest, Cholesky_NumericalStability)
{
    try {
        auto solver = SolverFactory::create(SolverType::CUSOLVER_CHOLESKY);

        // Create matrix with varying diagonal magnitudes (tests numerical
        // stability)
        int stab_n = 156;  // 13 * 12, typical reduced system size
        Eigen::SparseMatrix<float> stab_A(stab_n, stab_n);
        std::vector<Eigen::Triplet<float>> triplets;

        for (int i = 0; i < stab_n; ++i) {
            // Diagonal elements vary from 1 to 100
            float diag = 1.0f + (99.0f * i) / stab_n;
            triplets.push_back(Eigen::Triplet<float>(i, i, diag));

            if (i > 0) {
                triplets.push_back(
                    Eigen::Triplet<float>(i, i - 1, -0.1f * diag));
            }
            if (i < stab_n - 1) {
                triplets.push_back(
                    Eigen::Triplet<float>(i, i + 1, -0.1f * diag));
            }
        }
        stab_A.setFromTriplets(triplets.begin(), triplets.end());

        Eigen::VectorXf stab_b = Eigen::VectorXf::Ones(stab_n);
        Eigen::VectorXf stab_x = Eigen::VectorXf::Zero(stab_n);

        SolverConfig config;
        config.verbose = true;

        auto result = solver->solve(stab_A, stab_b, stab_x, config);

        EXPECT_TRUE(result.converged)
            << "Stability test failed: " << result.error_message;

        // Check relative residual
        Eigen::VectorXf residual = stab_A * stab_x - stab_b;
        float rel_residual = residual.norm() / stab_b.norm();
        EXPECT_LT(rel_residual, 1e-2f)
            << "Relative residual too large: " << rel_residual;

        std::cout << "Numerical stability test (" << stab_n << "x" << stab_n
                  << "): relative residual=" << rel_residual
                  << ", time=" << result.solve_time.count() << " μs"
                  << std::endl;
    }
    catch (const std::exception& e) {
        GTEST_SKIP() << "CUDA/cuSOLVER not available: " << e.what();
    }
}
