#include <RZSolver/Solver.hpp>
#include <functional>
#include <unordered_map>

RUZINO_NAMESPACE_OPEN_SCOPE

namespace Solver {

// Forward declarations of factory functions
#if RUZINO_WITH_CUDA
std::unique_ptr<LinearSolver> createCudaCGSolver();
std::unique_ptr<LinearSolver> createCudaBiCGStabSolver();
std::unique_ptr<LinearSolver> createCudaGMRESSolver();
// cuDSS-based solvers disabled (cuDSS not available on this system)
// std::unique_ptr<LinearSolver> createCuSolverQRSolver();
// std::unique_ptr<LinearSolver> createCuSolverCholeskySolver();
#endif
std::unique_ptr<LinearSolver> createEigenCGSolver();
std::unique_ptr<LinearSolver> createEigenBiCGStabSolver();
std::unique_ptr<LinearSolver> createEigenLUSolver();
std::unique_ptr<LinearSolver> createEigenCholeskySolver();
std::unique_ptr<LinearSolver> createEigenQRSolver();

using SolverCreator = std::function<std::unique_ptr<LinearSolver>()>;

static std::unordered_map<SolverType, SolverCreator> solverRegistry = {
#if RUZINO_WITH_CUDA
    { SolverType::CUDA_CG, createCudaCGSolver },
    { SolverType::CUDA_BICGSTAB, createCudaBiCGStabSolver },  // 新增
#endif
    { SolverType::EIGEN_ITERATIVE_CG, createEigenCGSolver },
    { SolverType::EIGEN_ITERATIVE_BICGSTAB, createEigenBiCGStabSolver },
    { SolverType::EIGEN_DIRECT_LU, createEigenLUSolver },
    { SolverType::EIGEN_DIRECT_CHOLESKY, createEigenCholeskySolver },
    { SolverType::EIGEN_DIRECT_QR, createEigenQRSolver }
};

static std::unordered_map<SolverType, std::string> solverNames = {
#if RUZINO_WITH_CUDA
    { SolverType::CUDA_CG, "CUDA Conjugate Gradient" },
    { SolverType::CUDA_BICGSTAB, "CUDA BiCGSTAB" },  // 新增
#endif
    { SolverType::EIGEN_ITERATIVE_CG, "Eigen Conjugate Gradient" },
    { SolverType::EIGEN_ITERATIVE_BICGSTAB, "Eigen BiCGSTAB" },
    { SolverType::EIGEN_DIRECT_LU, "Eigen Sparse LU" },
    { SolverType::EIGEN_DIRECT_CHOLESKY, "Eigen Sparse Cholesky" },
    { SolverType::EIGEN_DIRECT_QR, "Eigen Sparse QR" }
};

std::unique_ptr<LinearSolver> SolverFactory::create(SolverType type)
{
    try {
        switch (type) {
#if RUZINO_WITH_CUDA
            case SolverType::CUDA_CG: return createCudaCGSolver();
            case SolverType::CUDA_BICGSTAB:
                return createCudaBiCGStabSolver();  // 新增
            case SolverType::CUDA_GMRES:
                return createCudaGMRESSolver();
            // cuDSS solvers disabled
            // case SolverType::CUSOLVER_QR:
            //     return createCuSolverQRSolver();
            // case SolverType::CUSOLVER_CHOLESKY:
            //     return createCuSolverCholeskySolver();
#endif
            case SolverType::EIGEN_ITERATIVE_CG: return createEigenCGSolver();
            case SolverType::EIGEN_ITERATIVE_BICGSTAB:
                return createEigenBiCGStabSolver();
            case SolverType::EIGEN_DIRECT_LU: return createEigenLUSolver();
            case SolverType::EIGEN_DIRECT_CHOLESKY:
                return createEigenCholeskySolver();
            case SolverType::EIGEN_DIRECT_QR: return createEigenQRSolver();
            default: throw std::invalid_argument("Unknown solver type");
        }
    }
    catch (const std::exception& e) {
        throw std::runtime_error(
            "Failed to create solver: " + std::string(e.what()));
    }
}

std::vector<SolverType> SolverFactory::getAvailableTypes()
{
    std::vector<SolverType> types;

    // Try to create each solver to check availability
    std::vector<SolverType> all_types = {
#if RUZINO_WITH_CUDA
        SolverType::CUDA_CG,
        SolverType::CUDA_BICGSTAB,
        SolverType::CUDA_GMRES,
        // SolverType::CUSOLVER_QR,        // cuDSS disabled
        // SolverType::CUSOLVER_CHOLESKY,  // cuDSS disabled
#endif
        SolverType::EIGEN_ITERATIVE_CG,
        SolverType::EIGEN_ITERATIVE_BICGSTAB /*,
         SolverType::EIGEN_DIRECT_LU,
         SolverType::EIGEN_DIRECT_CHOLESKY,
         SolverType::EIGEN_DIRECT_QR*/

    };

    for (auto type : all_types) {
        try {
            auto solver = create(type);
            if (solver) {
                types.push_back(type);
            }
        }
        catch (...) {
            // Skip unavailable solvers
        }
    }

    return types;
}

std::string SolverFactory::getTypeName(SolverType type)
{
    switch (type) {
#if RUZINO_WITH_CUDA
        case SolverType::CUDA_CG: return "CUDA Conjugate Gradient";
        case SolverType::CUDA_BICGSTAB: return "CUDA BiCGSTAB";
        case SolverType::CUDA_GMRES: return "CUDA GMRES";
        // case SolverType::CUSOLVER_QR: return "cuSOLVER QR (Direct)";
        // case SolverType::CUSOLVER_CHOLESKY: return "cuSOLVER Cholesky (Direct)";
#endif
        case SolverType::EIGEN_ITERATIVE_CG: return "Eigen Conjugate Gradient";
        case SolverType::EIGEN_ITERATIVE_BICGSTAB: return "Eigen BiCGSTAB";
        case SolverType::EIGEN_DIRECT_LU: return "Eigen Sparse LU";
        case SolverType::EIGEN_DIRECT_CHOLESKY: return "Eigen Sparse Cholesky";
        case SolverType::EIGEN_DIRECT_QR: return "Eigen Sparse QR";
        default: return "Unknown";
    }
}

}  // namespace Solver

RUZINO_NAMESPACE_CLOSE_SCOPE
