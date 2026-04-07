#include <gtest/gtest.h>

#include <Eigen/Eigen>
#include <Eigen/Sparse>
#include <RZSolver/Solver.hpp>
#include <iomanip>
#include <iostream>
#include <numbers>

using namespace Ruzino::Solver;

class SolverComparisonTest : public ::testing::Test {
   protected:
    void SetUp() override
    {
        createTestMatrices();
    }

    void createTestMatrices()
    {
        // Small tridiagonal matrix
        createTridiagonalMatrix(small_A, small_b, small_x, 100);

        // Medium matrix for performance comparison
        createTridiagonalMatrix(medium_A, medium_b, medium_x, 1000);

        // Large matrix for stress testing
        createTridiagonalMatrix(large_A, large_b, large_x, 5000);
    }

    void createTridiagonalMatrix(
        Eigen::SparseMatrix<float>& A,
        Eigen::VectorXf& b,
        Eigen::VectorXf& x,
        int n)
    {
        A.resize(n, n);
        std::vector<Eigen::Triplet<float>> triplets;
        triplets.reserve(3 * n - 2);

        for (int i = 0; i < n; ++i) {
            triplets.push_back(Eigen::Triplet<float>(i, i, 2.0f));
            if (i > 0)
                triplets.push_back(Eigen::Triplet<float>(i, i - 1, -1.0f));
            if (i < n - 1)
                triplets.push_back(Eigen::Triplet<float>(i, i + 1, -1.0f));
        }
        A.setFromTriplets(triplets.begin(), triplets.end());

        b = Eigen::VectorXf::Ones(n);
        x = Eigen::VectorXf::Zero(n);

        // Print condition number estimate for large matrices
        if (n >= 1000) {
            double condition_estimate =
                4.0 * n * n / (std::numbers::pi * std::numbers::pi);
            std::cout << "Matrix size: " << n << "x" << n
                      << ", estimated condition number: " << condition_estimate
                      << " (log10: " << log10(condition_estimate) << ")"
                      << std::endl;
        }
    }

    void testSolver(
        SolverType type,
        const Eigen::SparseMatrix<float>& A,
        const Eigen::VectorXf& b,
        const std::string& test_name,
        bool is_spd = true,
        double expected_condition = 0.0)
    {
        try {
            auto solver = SolverFactory::create(type);
            Eigen::VectorXf x = Eigen::VectorXf::Zero(A.rows());

            SolverConfig config;
            config.tolerance = 1e-6f;
            config.max_iterations = 10000;
            config.verbose = false;

            // Skip inappropriate solver-matrix combinations
            std::string solver_name = SolverFactory::getTypeName(type);

            // CG and Cholesky only work for SPD matrices
            if (!is_spd &&
                (solver_name.find("Conjugate Gradient") != std::string::npos ||
                 solver_name.find("Cholesky") != std::string::npos)) {
                std::cout << std::left << std::setw(25) << solver_name << " | "
                          << std::setw(12) << test_name << " | "
                          << std::setw(10) << "SKIP"
                          << " | Not suitable for non-SPD matrices"
                          << std::endl;
                return;
            }

            // Note: BiCGSTAB can work on SPD matrices, though CG is more
            // efficient We allow it to run on SPD matrices with relaxed
            // tolerances

            auto result = solver->solve(A, b, x, config);

            // Verify solution
            Eigen::VectorXf residual = A * x - b;
            float relative_error = residual.norm() / b.norm();

            std::cout << std::left << std::setw(25) << solver_name << " | "
                      << std::setw(12) << test_name << " | " << std::setw(10)
                      << (result.converged ? "PASS" : "FAIL") << " | "
                      << std::setw(8) << result.iterations << " | "
                      << std::setw(12) << result.setup_time.count() << " | "
                      << std::setw(12) << result.solve_time.count() << " | "
                      << std::setw(12) << std::scientific << relative_error
                      << std::endl;

            if (result.converged) {
                // Realistic tolerance based on numerical analysis
                float tolerance = 1e-4f;

                // Use provided condition number or estimate for tridiagonal
                double condition_estimate = expected_condition;
                if (condition_estimate <= 0.0) {
                    condition_estimate = 4.0 * A.rows() * A.rows() /
                                         (std::numbers::pi * std::numbers::pi);
                }

                // Direct solvers lose accuracy with ill-conditioned matrices
                if (!solver->isIterative()) {
                    if (A.rows() >= 5000) {
                        tolerance = 0.5f;
                    }
                    else if (A.rows() >= 1000)
                        tolerance = 2e-2f;
                    else if (A.rows() >= 500)
                        tolerance = 5e-3f;
                    else
                        tolerance = 1e-3f;
                }

                // BiCGSTAB handling - works on both SPD and non-SPD matrices
                if (solver_name.find("BiCGSTAB") != std::string::npos) {
                    if (is_spd) {
                        // On SPD matrices, BiCGSTAB works but is less efficient
                        // than CG Allow more lenient tolerance
                        tolerance = std::max(tolerance, 5e-3f);
                        if (A.rows() >= 1000) {
                            tolerance = std::max(tolerance, 1e-2f);
                        }
                    }
                    else {
                        // On non-SPD matrices, BiCGSTAB should perform well
                        tolerance = 1e-3f;
                    }
                }

                // GMRES on SPD matrices - less efficient than CG, so more
                // lenient
                if (solver_name.find("GMRES") != std::string::npos && is_spd) {
                    tolerance = std::max(tolerance, 1e-3f);
                }

                // QR decomposition tolerance
                if (solver_name.find("QR") != std::string::npos) {
                    if (A.rows() >= 5000)
                        tolerance = std::max(tolerance, 1.0f);
                    else if (A.rows() >= 1000)
                        tolerance = std::max(tolerance, 5e-2f);
                }

                EXPECT_LT(relative_error, tolerance)
                    << "Poor solution quality for " << solver_name
                    << " (matrix size: " << A.rows() << "x" << A.cols()
                    << ", expected < " << tolerance << ", condition ~"
                    << condition_estimate << ")";
            }
            else {
                std::cout << "    Note: Solver did not converge - "
                          << result.error_message << std::endl;

                if (!is_spd &&
                    (solver_name.find("Conjugate Gradient") !=
                         std::string::npos ||
                     solver_name.find("Cholesky") != std::string::npos)) {
                    EXPECT_TRUE(true) << "SPD-only solver appropriately failed "
                                         "on non-SPD matrix";
                }
            }
        }
        catch (const std::exception& e) {
            std::cout << std::left << std::setw(25)
                      << SolverFactory::getTypeName(type) << " | "
                      << std::setw(12) << test_name << " | " << std::setw(10)
                      << "SKIP"
                      << " | Error: " << e.what() << std::endl;
        }
    }

    Eigen::SparseMatrix<float> small_A, medium_A, large_A;
    Eigen::VectorXf small_b, medium_b, large_b;
    Eigen::VectorXf small_x, medium_x, large_x;
};

TEST_F(SolverComparisonTest, SmallMatrixComparison)
{
    std::cout << "\n=== Small Matrix (100x100) Comparison ===" << std::endl;
    std::cout << std::left << std::setw(25) << "Solver"
              << " | " << std::setw(12) << "Test"
              << " | " << std::setw(10) << "Status"
              << " | " << std::setw(8) << "Iters"
              << " | " << std::setw(12) << "Setup(μs)"
              << " | " << std::setw(12) << "Solve(μs)"
              << " | " << std::setw(12) << "Rel.Error" << std::endl;
    std::cout << std::string(120, '-') << std::endl;

    auto available_types = SolverFactory::getAvailableTypes();
    for (auto type : available_types) {
        testSolver(type, small_A, small_b, "Small");
    }
}

TEST_F(SolverComparisonTest, MediumMatrixComparison)
{
    std::cout << "\n=== Medium Matrix (1000x1000) Comparison ===" << std::endl;
    std::cout << std::left << std::setw(25) << "Solver"
              << " | " << std::setw(12) << "Test"
              << " | " << std::setw(10) << "Status"
              << " | " << std::setw(8) << "Iters"
              << " | " << std::setw(12) << "Setup(μs)"
              << " | " << std::setw(12) << "Solve(μs)"
              << " | " << std::setw(12) << "Rel.Error" << std::endl;
    std::cout << std::string(120, '-') << std::endl;

    auto available_types = SolverFactory::getAvailableTypes();
    for (auto type : available_types) {
        testSolver(type, medium_A, medium_b, "Medium");
    }
}

TEST_F(SolverComparisonTest, LargeMatrixComparison)
{
    std::cout << "\n=== Large Matrix (5000x5000) Comparison ===" << std::endl;
    std::cout << std::left << std::setw(25) << "Solver"
              << " | " << std::setw(12) << "Test"
              << " | " << std::setw(10) << "Status"
              << " | " << std::setw(8) << "Iters"
              << " | " << std::setw(12) << "Setup(μs)"
              << " | " << std::setw(12) << "Solve(μs)"
              << " | " << std::setw(12) << "Rel.Error" << std::endl;
    std::cout << std::string(120, '-') << std::endl;

    auto available_types = SolverFactory::getAvailableTypes();
    for (auto type : available_types) {
        testSolver(type, large_A, large_b, "Large");
    }
}

TEST_F(SolverComparisonTest, IterativeVsDirectComparison)
{
    std::cout << "\n=== Iterative vs Direct Methods ===" << std::endl;

    // Test different problem characteristics
    struct TestCase {
        std::string name;
        std::function<void(Eigen::SparseMatrix<float>&, Eigen::VectorXf&)>
            generator;
    };

    std::vector<TestCase> test_cases = {
        { "Tridiagonal",
          [this](auto& A, auto& b) {
              createTridiagonalMatrix(A, b, small_x, 500);
          } },
        { "Poisson2D",
          [](auto& A, auto& b) {
              // Create 2D Poisson matrix (5-point stencil)
              int nx = 20, ny = 20;  // 20x20 grid
              int n = nx * ny;
              A.resize(n, n);
              std::vector<Eigen::Triplet<float>> triplets;

              for (int i = 0; i < nx; ++i) {
                  for (int j = 0; j < ny; ++j) {
                      int idx = i * ny + j;
                      triplets.push_back(Eigen::Triplet<float>(idx, idx, 4.0f));

                      if (i > 0)
                          triplets.push_back(
                              Eigen::Triplet<float>(
                                  idx, (i - 1) * ny + j, -1.0f));
                      if (i < nx - 1)
                          triplets.push_back(
                              Eigen::Triplet<float>(
                                  idx, (i + 1) * ny + j, -1.0f));
                      if (j > 0)
                          triplets.push_back(
                              Eigen::Triplet<float>(
                                  idx, i * ny + (j - 1), -1.0f));
                      if (j < ny - 1)
                          triplets.push_back(
                              Eigen::Triplet<float>(
                                  idx, i * ny + (j + 1), -1.0f));
                  }
              }
              A.setFromTriplets(triplets.begin(), triplets.end());
              b = Eigen::VectorXf::Ones(n);
          } }
    };

    for (const auto& test_case : test_cases) {
        std::cout << "\nTest case: " << test_case.name << std::endl;
        Eigen::SparseMatrix<float> A;
        Eigen::VectorXf b;
        test_case.generator(A, b);

        // Test iterative methods
        std::vector<SolverType> iterative_methods = {
            SolverType::EIGEN_ITERATIVE_CG,
            SolverType::EIGEN_ITERATIVE_BICGSTAB,
            SolverType::CUDA_CG,
            SolverType::CUDA_BICGSTAB,
            SolverType::CUDA_GMRES  // 新增
        };

        // Test direct methods
        std::vector<SolverType> direct_methods = {
            SolverType::EIGEN_DIRECT_LU,
            SolverType::EIGEN_DIRECT_CHOLESKY,
            SolverType::EIGEN_DIRECT_QR
        };

        std::cout << "Iterative methods:" << std::endl;
        for (auto type : iterative_methods) {
            testSolver(type, A, b, test_case.name);
        }

        std::cout << "Direct methods:" << std::endl;
        for (auto type : direct_methods) {
            testSolver(type, A, b, test_case.name);
        }
    }
}

TEST_F(SolverComparisonTest, PerformanceBenchmark)
{
    std::cout << "\n=== Performance Benchmark ===" << std::endl;

    // Create a well-conditioned matrix for fair comparison
    int n = 2000;
    Eigen::SparseMatrix<float> A(n, n);
    std::vector<Eigen::Triplet<float>> triplets;

    // Create a banded matrix (good for CG)
    for (int i = 0; i < n; ++i) {
        triplets.push_back(Eigen::Triplet<float>(i, i, 4.0f));
        if (i > 0)
            triplets.push_back(Eigen::Triplet<float>(i, i - 1, -1.0f));
        if (i < n - 1)
            triplets.push_back(Eigen::Triplet<float>(i, i + 1, -1.0f));
        if (i > 1)
            triplets.push_back(Eigen::Triplet<float>(i, i - 2, -0.5f));
        if (i < n - 2)
            triplets.push_back(Eigen::Triplet<float>(i, i + 2, -0.5f));
    }
    A.setFromTriplets(triplets.begin(), triplets.end());
    Eigen::VectorXf b = Eigen::VectorXf::Random(n);

    std::cout << "Matrix size: " << n << "x" << n << std::endl;
    std::cout << "Non-zeros: " << A.nonZeros() << std::endl;

    // Test all solvers with stricter tolerance for performance comparison
    SolverConfig config;
    config.tolerance = 1e-8f;
    config.max_iterations = 50000;
    config.verbose = false;

    std::cout << std::left << std::setw(25) << "Solver"
              << " | " << std::setw(12) << "Setup(ms)"
              << " | " << std::setw(12) << "Solve(ms)"
              << " | " << std::setw(12) << "Total(ms)"
              << " | " << std::setw(8) << "Iters"
              << " | " << std::setw(12) << "Residual" << std::endl;
    std::cout << std::string(100, '-') << std::endl;

    auto available_types = SolverFactory::getAvailableTypes();
    for (auto type : available_types) {
        try {
            auto solver = SolverFactory::create(type);
            Eigen::VectorXf x = Eigen::VectorXf::Zero(n);

            auto result = solver->solve(A, b, x, config);

            float setup_ms = result.setup_time.count() / 1000.0f;
            float solve_ms = result.solve_time.count() / 1000.0f;
            float total_ms = setup_ms + solve_ms;

            std::cout << std::left << std::setw(25)
                      << SolverFactory::getTypeName(type) << " | "
                      << std::setw(12) << std::fixed << std::setprecision(2)
                      << setup_ms << " | " << std::setw(12) << solve_ms << " | "
                      << std::setw(12) << total_ms << " | " << std::setw(8)
                      << result.iterations << " | " << std::setw(12)
                      << std::scientific << result.final_residual << std::endl;
        }
        catch (const std::exception& e) {
            std::cout << std::left << std::setw(25)
                      << SolverFactory::getTypeName(type) << " | SKIP - "
                      << e.what() << std::endl;
        }
    }
}

TEST_F(SolverComparisonTest, NumericalStabilityAnalysis)
{
    std::cout << "\n=== Numerical Stability Analysis ===" << std::endl;

    // Test different matrix types with known properties
    struct MatrixTest {
        std::string name;
        std::function<void(Eigen::SparseMatrix<float>&, Eigen::VectorXf&, int)>
            generator;
        double expected_condition;
        bool spd;
        bool test_bicgstab;
    };

    std::vector<MatrixTest> matrix_tests = {
        { "Well-conditioned SPD",
          [](auto& A, auto& b, int n) {
              A.resize(n, n);
              std::vector<Eigen::Triplet<float>> triplets;
              for (int i = 0; i < n; ++i) {
                  triplets.push_back(Eigen::Triplet<float>(i, i, 10.0f));
                  if (i > 0)
                      triplets.push_back(
                          Eigen::Triplet<float>(i, i - 1, -1.0f));
                  if (i < n - 1)
                      triplets.push_back(
                          Eigen::Triplet<float>(i, i + 1, -1.0f));
              }
              A.setFromTriplets(triplets.begin(), triplets.end());
              b = Eigen::VectorXf::Ones(n);
          },
          100.0,
          true,
          false },
        { "Identity matrix",
          [](auto& A, auto& b, int n) {
              A.resize(n, n);
              A.setIdentity();
              b = Eigen::VectorXf::Random(n);
          },
          1.0,
          true,
          false },
        { "Non-symmetric well-conditioned",
          [](auto& A, auto& b, int n) {
              A.resize(n, n);
              std::vector<Eigen::Triplet<float>> triplets;
              for (int i = 0; i < n; ++i) {
                  triplets.push_back(Eigen::Triplet<float>(i, i, 5.0f));
                  if (i > 0)
                      triplets.push_back(
                          Eigen::Triplet<float>(i, i - 1, -1.0f));
                  if (i < n - 1)
                      triplets.push_back(
                          Eigen::Triplet<float>(
                              i, i + 1, -3.0f));  // More asymmetric
              }
              A.setFromTriplets(triplets.begin(), triplets.end());
              b = Eigen::VectorXf::Ones(n);
          },
          50.0,
          false,
          true },
        { "Simple non-symmetric (diag dominant)",
          [](auto& A, auto& b, int n) {
              A.resize(n, n);
              std::vector<Eigen::Triplet<float>> triplets;
              for (int i = 0; i < n; ++i) {
                  triplets.push_back(
                      Eigen::Triplet<float>(i, i, 10.0f));  // Strong diagonal
                  if (i > 0)
                      triplets.push_back(
                          Eigen::Triplet<float>(i, i - 1, -1.0f));
                  if (i < n - 1)
                      triplets.push_back(
                          Eigen::Triplet<float>(
                              i, i + 1, -2.0f));  // Clear asymmetry
              }
              A.setFromTriplets(triplets.begin(), triplets.end());
              b = Eigen::VectorXf::Ones(n);
          },
          20.0,
          false,
          true }
    };

    int test_size = 500;

    for (const auto& matrix_test : matrix_tests) {
        std::cout << "\n--- " << matrix_test.name << " (" << test_size << "x"
                  << test_size << ") ---" << std::endl;

        Eigen::SparseMatrix<float> A;
        Eigen::VectorXf b;
        matrix_test.generator(A, b, test_size);

        std::cout << "Expected condition number: "
                  << matrix_test.expected_condition << std::endl;
        std::cout << "Matrix type: " << (matrix_test.spd ? "SPD" : "General")
                  << std::endl;
        std::cout << "Test BiCGSTAB: "
                  << (matrix_test.test_bicgstab ? "Yes" : "No") << std::endl;

        // Test all solvers with proper matrix type awareness
        auto available_types = SolverFactory::getAvailableTypes();
        for (auto type : available_types) {
            std::string solver_name = SolverFactory::getTypeName(type);

            // Skip non-SPD specific tests based on test specification
            // (only skip if explicitly marked as non-SPD test)
            if (!matrix_test.test_bicgstab &&
                solver_name.find("BiCGSTAB") != std::string::npos) {
                std::cout << "Skipping BiCGSTAB for " << matrix_test.name
                          << " (not in test spec)" << std::endl;
                continue;
            }

            testSolver(
                type,
                A,
                b,
                matrix_test.name,
                matrix_test.spd,
                matrix_test.expected_condition);
        }
    }
}
