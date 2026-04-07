#include <cublas_v2.h>
#include <cusparse.h>

#include <RHI/cuda.hpp>
#include <algorithm>
#include <glm/glm.hpp>
#include <limits>
#include <numeric>

#include "GCore/Components/MeshComponent.h"
#include "GCore/Components/PointsComponent.h"
#include "GCore/geom_payload.hpp"
#include "RHI/internal/cuda_extension.hpp"
#include "RZSolver/Solver.hpp"
#include "glm/ext/vector_float3.hpp"
#include "nodes/core/def/node_def.hpp"
#include "nvrhi/nvrhi.h"
#include "rzsim/reduced_order_basis.h"
#include "rzsim_cuda/adjacency_map.cuh"
#include "rzsim_cuda/mass_spring_implicit.cuh"
#include "rzsim_cuda/neo_hookean.cuh"
#include "rzsim_cuda/reduced_order_neo_hookean.cuh"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

// Storage for persistent GPU simulation state
struct ReducedNeoHookeanGPUStorage {
    cuda::CUDALinearBufferHandle positions_buffer;
    cuda::CUDALinearBufferHandle velocities_buffer;
    cuda::CUDALinearBufferHandle next_positions_buffer;
    cuda::CUDALinearBufferHandle mass_matrix_buffer;
    cuda::CUDALinearBufferHandle gradients_buffer;

    // Mesh topology buffers (cached)
    cuda::CUDALinearBufferHandle face_vertex_indices_buffer;
    cuda::CUDALinearBufferHandle face_counts_buffer;
    cuda::CUDALinearBufferHandle normals_buffer;

    // Volume adjacency map encapsulates all tetrahedral topology
    std::unique_ptr<rzsim_cuda::VolumeAdjacencyMap> volume_adjacency;

    // Neo-Hookean specific reference data
    cuda::CUDALinearBufferHandle Dm_inv_buffer;
    cuda::CUDALinearBufferHandle volumes_buffer;

    // Pre-built CSR structure (built once, reused forever)
    rzsim_cuda::NeoHookeanCSRStructure hessian_structure;
    cuda::CUDALinearBufferHandle hessian_values;

    // Temporary buffers for Newton iterations
    cuda::CUDALinearBufferHandle x_new_buffer;
    cuda::CUDALinearBufferHandle newton_direction_buffer;
    cuda::CUDALinearBufferHandle neg_gradient_buffer;
    cuda::CUDALinearBufferHandle x_candidate_buffer;

    // Temporary buffers for energy computation
    cuda::CUDALinearBufferHandle inertial_terms_buffer;
    cuda::CUDALinearBufferHandle element_energies_buffer;
    cuda::CUDALinearBufferHandle potential_terms_buffer;
    cuda::CUDALinearBufferHandle
        barrier_hessian_diagonal;  // For barrier Hessian

    // Reduced order model data
    rzsim_cuda::ReducedOrderData ro_data;
    cuda::CUDALinearBufferHandle q_reduced;        // [num_basis * 12]
    cuda::CUDALinearBufferHandle q_dot_reduced;    // [num_basis * 12]
    cuda::CUDALinearBufferHandle q_tilde_reduced;  // [num_basis * 12]
    cuda::CUDALinearBufferHandle q_new_reduced;    // [num_basis * 12]
    cuda::CUDALinearBufferHandle
        jacobian;  // [num_particles * 3, num_basis * 12]
    cuda::CUDALinearBufferHandle grad_reduced;  // [num_basis * 12]
    cuda::CUDALinearBufferHandle
        hessian_reduced;  // [num_basis * 12, num_basis * 12]
    cuda::CUDALinearBufferHandle
        temp_hessian_buffer;  // [num_particles * 3, num_basis * 12]
    cuda::CUDALinearBufferHandle newton_direction_reduced;  // [num_basis * 12]
    cuda::CUDALinearBufferHandle neg_gradient_reduced;      // [num_basis * 12]
    cuda::CUDALinearBufferHandle q_candidate_reduced;       // [num_basis * 12]
    int num_basis = 0;

    // Reuse solver instance
    std::unique_ptr<Ruzino::Solver::LinearSolver> solver;

    // Dirichlet boundary conditions
    cuda::CUDALinearBufferHandle bc_dofs_buffer;  // DOF indices with BC
    int num_bc_dofs = 0;
    std::vector<int> bc_dofs;  // Host copy for reference

    // Barrier function energy buffer
    cuda::CUDALinearBufferHandle barrier_energy_buffer;

    // Cached cuBLAS and cuSPARSE handles
    cublasHandle_t cublas_handle = nullptr;
    cusparseHandle_t cusparse_handle = nullptr;

    // Cached matrix descriptors for Hessian computation
    cusparseSpMatDescr_t hessian_csr_descriptor = nullptr;
    cusparseDnMatDescr_t jacobian_descriptor = nullptr;
    cusparseDnMatDescr_t temp_hessian_descriptor = nullptr;
    cusparseDnMatDescr_t hessian_reduced_descriptor = nullptr;

    // cuSPARSE workspace
    void* cusparse_workspace = nullptr;
    size_t cusparse_workspace_size = 0;

    bool initialized = false;
    int num_particles = 0;
    int num_elements = 0;

    constexpr static bool has_storage = false;

    // Destructor to clean up CUDA resources
    ~ReducedNeoHookeanGPUStorage()
    {
        if (cublas_handle) {
            cublasDestroy(cublas_handle);
            cublas_handle = nullptr;
        }
        if (cusparse_handle) {
            cusparseDestroy(cusparse_handle);
            cusparse_handle = nullptr;
        }
        if (hessian_csr_descriptor) {
            cusparseDestroySpMat(hessian_csr_descriptor);
            hessian_csr_descriptor = nullptr;
        }
        if (jacobian_descriptor) {
            cusparseDestroyDnMat(jacobian_descriptor);
            jacobian_descriptor = nullptr;
        }
        if (temp_hessian_descriptor) {
            cusparseDestroyDnMat(temp_hessian_descriptor);
            temp_hessian_descriptor = nullptr;
        }
        if (hessian_reduced_descriptor) {
            cusparseDestroyDnMat(hessian_reduced_descriptor);
            hessian_reduced_descriptor = nullptr;
        }
        if (cusparse_workspace) {
            cudaFree(cusparse_workspace);
            cusparse_workspace = nullptr;
        }
    }

    // Initialize all GPU buffers and structures
    void initialize(
        const std::vector<glm::vec3>& positions,
        const std::vector<int>& face_vertex_indices,
        const std::vector<int>& face_counts,
        const std::vector<float>& surface_of_vol,
        float density,
        std::shared_ptr<Ruzino::ReducedOrderedBasis> reduced_basis)
    {
        num_particles = positions.size();

        // Validate reduced basis
        if (!reduced_basis || reduced_basis->basis.empty()) {
            spdlog::error("[ReducedNeoHookean] Reduced basis is empty or null");
            return;
        }

        num_basis = reduced_basis->basis.size();
        spdlog::debug("[ReducedNeoHookean] Using {} basis modes", num_basis);

        // Validate basis dimensions
        for (int i = 0; i < num_basis; ++i) {
            if (reduced_basis->basis[i].size() != num_particles) {
                spdlog::error(
                    "[ReducedNeoHookean] Basis {} size mismatch: {} vs {}",
                    i,
                    reduced_basis->basis[i].size(),
                    num_particles);
                return;
            }
        }

        // Write positions to GPU buffer
        positions_buffer = cuda::create_cuda_linear_buffer(positions);
        // Cache face topology buffers
        face_vertex_indices_buffer =
            cuda::create_cuda_linear_buffer(face_vertex_indices);
        face_counts_buffer = cuda::create_cuda_linear_buffer(face_counts);
        normals_buffer = cuda::create_cuda_linear_buffer<glm::vec3>(
            face_vertex_indices.size());

        // Build volume adjacency map (encapsulates all tetrahedral topology)
        volume_adjacency = std::make_unique<rzsim_cuda::VolumeAdjacencyMap>(
            positions_buffer, face_vertex_indices);

        num_elements = volume_adjacency->num_elements();

        if (num_elements == 0) {
            spdlog::error(
                "No tetrahedral elements found! Neo-Hookean requires "
                "volumetric mesh.");
            return;
        }

        // Compute Neo-Hookean specific reference data (Dm_inv and volumes)
        auto [Dm_inv, volumes] = rzsim_cuda::compute_nh_reference_data_gpu(
            positions_buffer, *volume_adjacency, num_elements);

        Dm_inv_buffer = Dm_inv;
        volumes_buffer = volumes;

        // Diagnostic: Check volumes
        auto volumes_host = volumes->get_host_vector<float>();
        float min_volume =
            *std::min_element(volumes_host.begin(), volumes_host.end());
        float max_volume =
            *std::max_element(volumes_host.begin(), volumes_host.end());
        float total_volume =
            std::accumulate(volumes_host.begin(), volumes_host.end(), 0.0f);
        spdlog::info(
            "[ReducedNeoHookean] Volume statistics: min={:.6e}, max={:.6e}, "
            "total={:.6e}, avg={:.6e}",
            min_volume,
            max_volume,
            total_volume,
            total_volume / num_elements);

        // Check for problematic volumes
        int num_small = std::count_if(
            volumes_host.begin(), volumes_host.end(), [](float v) {
                return v < 1e-10f;
            });
        if (num_small > 0) {
            spdlog::warn(
                "[ReducedNeoHookean] Found {} degenerate tetrahedra (volume < "
                "1e-10)",
                num_small);
        }

        // Initialize velocities to zero
        std::vector<glm::vec3> initial_velocities(
            num_particles, glm::vec3(0.0f));
        velocities_buffer = cuda::create_cuda_linear_buffer(initial_velocities);

        auto dof = num_particles * 3;

        next_positions_buffer = cuda::create_cuda_linear_buffer<float>(dof);
        gradients_buffer = cuda::create_cuda_linear_buffer<float>(dof);
        mass_matrix_buffer = cuda::create_cuda_linear_buffer<float>(dof);

        // Compute lumped mass matrix from density and element volumes
        // For each element: m_elem = density * volume
        // Distribute equally to 4 vertices: m_vertex += m_elem / 4
        rzsim_cuda::compute_lumped_mass_matrix_gpu(
            *volume_adjacency,
            volumes_buffer,
            density,
            num_particles,
            num_elements,
            mass_matrix_buffer);

        // Build Hessian CSR structure
        hessian_structure = rzsim_cuda::build_hessian_structure_nh_gpu(
            *volume_adjacency, num_particles, num_elements);

        hessian_values =
            cuda::create_cuda_linear_buffer<float>(hessian_structure.nnz);

        // Allocate temporary buffers for Newton iterations
        x_new_buffer = cuda::create_cuda_linear_buffer<float>(dof);
        newton_direction_buffer = cuda::create_cuda_linear_buffer<float>(dof);
        neg_gradient_buffer = cuda::create_cuda_linear_buffer<float>(dof);
        x_candidate_buffer = cuda::create_cuda_linear_buffer<float>(dof);

        // Allocate temporary buffers for energy computation
        inertial_terms_buffer = cuda::create_cuda_linear_buffer<float>(dof);
        element_energies_buffer =
            cuda::create_cuda_linear_buffer<float>(num_elements);
        potential_terms_buffer = cuda::create_cuda_linear_buffer<float>(dof);
        barrier_hessian_diagonal = cuda::create_cuda_linear_buffer<float>(dof);

        // Build reduced order data
        ro_data = rzsim_cuda::build_reduced_order_data_gpu(
            &reduced_basis->basis, &positions);

        // Allocate reduced coordinate buffers (num_basis * 12 DOF for affine
        // transforms)
        int reduced_dof = num_basis * 12;
        q_reduced = cuda::create_cuda_linear_buffer<float>(reduced_dof);
        q_dot_reduced = cuda::create_cuda_linear_buffer<float>(reduced_dof);
        q_tilde_reduced = cuda::create_cuda_linear_buffer<float>(reduced_dof);
        q_new_reduced = cuda::create_cuda_linear_buffer<float>(reduced_dof);
        grad_reduced = cuda::create_cuda_linear_buffer<float>(reduced_dof);
        newton_direction_reduced =
            cuda::create_cuda_linear_buffer<float>(reduced_dof);
        neg_gradient_reduced =
            cuda::create_cuda_linear_buffer<float>(reduced_dof);
        q_candidate_reduced =
            cuda::create_cuda_linear_buffer<float>(reduced_dof);

        // Allocate Jacobian and Hessian buffers
        jacobian = cuda::create_cuda_linear_buffer<float>(dof * reduced_dof);
        hessian_reduced =
            cuda::create_cuda_linear_buffer<float>(reduced_dof * reduced_dof);
        temp_hessian_buffer =
            cuda::create_cuda_linear_buffer<float>(dof * reduced_dof);

        // Allocate barrier energy buffer (max possible BC vertices)
        barrier_energy_buffer =
            cuda::create_cuda_linear_buffer<float>(num_particles);

        // Initialize reduced coordinates to identity (R=I, t=0 for each basis)
        rzsim_cuda::initialize_reduced_coords_identity_gpu(
            num_basis, q_reduced);

        // Initialize reduced velocities to zero
        cudaMemset(
            reinterpret_cast<void*>(q_dot_reduced->get_device_ptr()),
            0,
            reduced_dof * sizeof(float));

        // Create solver instance
        solver = Ruzino::Solver::SolverFactory::create(
            Ruzino::Solver::SolverType::CUSOLVER_CHOLESKY);

        // Create and cache cuBLAS handle
        cublasStatus_t cublas_status = cublasCreate(&cublas_handle);
        if (cublas_status != CUBLAS_STATUS_SUCCESS) {
            spdlog::error(
                "[ReducedNeoHookean] Failed to create cuBLAS handle: {}",
                static_cast<int>(cublas_status));
            return;
        }

        // Create and cache cuSPARSE handle
        cusparseStatus_t cusparse_status = cusparseCreate(&cusparse_handle);
        if (cusparse_status != CUSPARSE_STATUS_SUCCESS) {
            spdlog::error(
                "[ReducedNeoHookean] Failed to create cuSPARSE handle: {}",
                static_cast<int>(cusparse_status));
            return;
        }

        // Create CSR descriptor for Hessian matrix [full_dof, full_dof]
        const int* row_offsets_ptr =
            hessian_structure.row_offsets->get_device_ptr<int>();
        const int* col_indices_ptr =
            hessian_structure.col_indices->get_device_ptr<int>();
        float* hessian_values_ptr = hessian_values->get_device_ptr<float>();

        cusparse_status = cusparseCreateCsr(
            &hessian_csr_descriptor,
            dof,                    // rows
            dof,                    // cols
            hessian_structure.nnz,  // nnz
            const_cast<int*>(row_offsets_ptr),
            const_cast<int*>(col_indices_ptr),
            hessian_values_ptr,
            CUSPARSE_INDEX_32I,
            CUSPARSE_INDEX_32I,
            CUSPARSE_INDEX_BASE_ZERO,
            CUDA_R_32F);

        if (cusparse_status != CUSPARSE_STATUS_SUCCESS) {
            spdlog::error(
                "[ReducedNeoHookean] Failed to create CSR descriptor: {}",
                static_cast<int>(cusparse_status));
            return;
        }

        // Create dense matrix descriptor for Jacobian [full_dof, reduced_dof]
        // (row-major)
        float* jacobian_ptr = jacobian->get_device_ptr<float>();
        cusparse_status = cusparseCreateDnMat(
            &jacobian_descriptor,
            dof,          // rows
            reduced_dof,  // cols
            reduced_dof,  // leading dimension (row-major)
            jacobian_ptr,
            CUDA_R_32F,
            CUSPARSE_ORDER_ROW);

        if (cusparse_status != CUSPARSE_STATUS_SUCCESS) {
            spdlog::error(
                "[ReducedNeoHookean] Failed to create Jacobian descriptor: {}",
                static_cast<int>(cusparse_status));
            return;
        }

        // Create dense matrix descriptor for temp buffer [full_dof,
        // reduced_dof] (row-major)
        float* temp_buffer_ptr = temp_hessian_buffer->get_device_ptr<float>();
        cusparse_status = cusparseCreateDnMat(
            &temp_hessian_descriptor,
            dof,          // rows
            reduced_dof,  // cols
            reduced_dof,  // leading dimension (row-major)
            temp_buffer_ptr,
            CUDA_R_32F,
            CUSPARSE_ORDER_ROW);

        if (cusparse_status != CUSPARSE_STATUS_SUCCESS) {
            spdlog::error(
                "[ReducedNeoHookean] Failed to create temp Hessian descriptor: "
                "{}",
                static_cast<int>(cusparse_status));
            return;
        }

        // Create dense matrix descriptor for reduced Hessian [reduced_dof,
        // reduced_dof] (row-major)
        float* H_q_ptr = hessian_reduced->get_device_ptr<float>();
        cusparse_status = cusparseCreateDnMat(
            &hessian_reduced_descriptor,
            reduced_dof,  // rows
            reduced_dof,  // cols
            reduced_dof,  // leading dimension (row-major)
            H_q_ptr,
            CUDA_R_32F,
            CUSPARSE_ORDER_ROW);

        if (cusparse_status != CUSPARSE_STATUS_SUCCESS) {
            spdlog::error(
                "[ReducedNeoHookean] Failed to create reduced Hessian "
                "descriptor: {}",
                static_cast<int>(cusparse_status));
            return;
        }

        // Allocate cuSPARSE workspace buffer
        float alpha = 1.0f;
        float beta = 0.0f;
        cusparse_status = cusparseSpMM_bufferSize(
            cusparse_handle,
            CUSPARSE_OPERATION_NON_TRANSPOSE,
            CUSPARSE_OPERATION_NON_TRANSPOSE,
            &alpha,
            hessian_csr_descriptor,
            jacobian_descriptor,
            &beta,
            temp_hessian_descriptor,
            CUDA_R_32F,
            CUSPARSE_SPMM_ALG_DEFAULT,
            &cusparse_workspace_size);

        if (cusparse_status != CUSPARSE_STATUS_SUCCESS) {
            spdlog::error(
                "[ReducedNeoHookean] Failed to get cuSPARSE workspace size: {}",
                static_cast<int>(cusparse_status));
            return;
        }

        if (cusparse_workspace_size > 0) {
            cudaMalloc(&cusparse_workspace, cusparse_workspace_size);
            spdlog::debug(
                "[ReducedNeoHookean] Allocated cuSPARSE workspace: {} bytes",
                cusparse_workspace_size);
        }

        spdlog::debug(
            "[ReducedNeoHookean] Initialized with {} particles, {} elements, "
            "{} basis modes, {} reduced DOF",
            num_particles,
            num_elements,
            num_basis,
            num_basis * 12);

        initialized = true;
    }
};

NODE_DECLARATION_FUNCTION(reduced_order_neo_hookean_gpu)
{
    b.add_input<Geometry>("Geometry");
    b.add_input<Geometry>("Init Geometry");
    b.add_input<std::shared_ptr<ReducedOrderedBasis>>("Reduced Basis");
    b.add_input<std::shared_ptr<Ruzino::AffineTransform>>("Initial Transform");

    b.add_input<float>("Density").default_val(1000.0f).min(1.0f).max(10000.0f);
    b.add_input<float>("Young's Modulus").default_val(5e4f).min(1e3f).max(1e9f);
    b.add_input<float>("Poisson's Ratio")
        .default_val(0.35f)
        .min(0.0f)
        .max(0.49f);
    b.add_input<float>("Damping").default_val(0.99f).min(0.0f).max(1.0f);
    b.add_input<int>("Substeps").default_val(5).min(1).max(20);
    b.add_input<int>("Newton Iterations").default_val(50).min(1).max(100);
    b.add_input<float>("Newton Tolerance")
        .default_val(1e-2f)
        .min(1e-8f)
        .max(1e-1f);
    b.add_input<float>("Gravity").default_val(-9.81f).min(-20.0f).max(0.0f);
    b.add_input<float>("Ground Restitution")
        .default_val(0.3f)
        .min(0.0f)
        .max(1.0f);
    b.add_input<bool>("Consider BC").default_val(false);
    b.add_input<float>("Barrier Stiffness")
        .default_val(1e5f)
        .min(1e3f)
        .max(1e8f);
    b.add_input<float>("Barrier Width").default_val(0.01f).min(1e-4f).max(0.1f);
    b.add_input<bool>("Flip Normal").default_val(false);

    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(reduced_order_neo_hookean_gpu)
{
    auto& global_payload = params.get_global_payload<GeomPayload&>();
    auto& storage = params.get_storage<ReducedNeoHookeanGPUStorage&>();

    // Get inputs
    auto input_geom = params.get_input<Geometry>("Geometry");
    input_geom.apply_transform();

    auto reduced_basis =
        params.get_input<std::shared_ptr<Ruzino::ReducedOrderedBasis>>(
            "Reduced Basis");

    auto initial_transform =
        params.get_input<std::shared_ptr<Ruzino::AffineTransform>>(
            "Initial Transform");

    float density = params.get_input<float>("Density");
    float youngs_modulus = params.get_input<float>("Young's Modulus");
    float poisson_ratio = params.get_input<float>("Poisson's Ratio");
    float damping = params.get_input<float>("Damping");
    int substeps = params.get_input<int>("Substeps");
    int max_iterations = params.get_input<int>("Newton Iterations");
    float tolerance = params.get_input<float>("Newton Tolerance");
    tolerance =
        std::max(tolerance, 1e-10f);  // Use very tight tolerance for symmetry
    float gravity = params.get_input<float>("Gravity");
    float restitution = params.get_input<float>("Ground Restitution");
    bool consider_bc = params.get_input<bool>("Consider BC");
    float barrier_stiffness = params.get_input<float>("Barrier Stiffness");
    float barrier_width = params.get_input<float>("Barrier Width");
    bool flip_normal = params.get_input<bool>("Flip Normal");
    float dt = global_payload.delta_time;

    // Convert Young's modulus and Poisson's ratio to Lamé parameters
    float mu = youngs_modulus / (2.0f * (1.0f + poisson_ratio));
    float lambda = youngs_modulus * poisson_ratio /
                   ((1.0f + poisson_ratio) * (1.0f - 2.0f * poisson_ratio));

    // Get mesh component for topology
    auto mesh_component = input_geom.get_component<MeshComponent>();
    std::vector<glm::vec3> positions;
    std::vector<int> face_vertex_indices;
    std::vector<int> face_counts;
    std::vector<float> dirichlet_face_values;  // Face quantity for BC

    if (mesh_component) {
        positions = mesh_component->get_vertices();
        face_vertex_indices = mesh_component->get_face_vertex_indices();
        face_counts = mesh_component->get_face_vertex_counts();
        // Try to get dirichlet face quantity
        dirichlet_face_values =
            mesh_component->get_face_scalar_quantity("dirichlet");
    }
    else {
        auto points_component = input_geom.get_component<PointsComponent>();
        if (!points_component) {
            params.set_output<Geometry>("Geometry", std::move(input_geom));
            return true;
        }
        positions = points_component->get_vertices();
    }

    int num_particles = positions.size();
    if (num_particles == 0) {
        params.set_output<Geometry>("Geometry", std::move(input_geom));
        return true;
    }

    // Initialize buffers only once or when particle count changes
    // ALWAYS use rest pose (input_geom positions) for reference configuration
    if (!storage.initialized || storage.num_particles != num_particles) {
        auto surface_of_vol =
            mesh_component->get_face_scalar_quantity("surface_of_vol");
        storage.initialize(
            positions,  // Use rest pose for Dm_inv, volumes calculation
            face_vertex_indices,
            face_counts,
            surface_of_vol,
            density,
            reduced_basis);
    }

    if (!global_payload.is_simulating) {
        // Get mesh component from init geometry for initialization
        auto init_geom = params.get_input<Geometry>("Init Geometry");
        init_geom.apply_transform();
        auto init_mesh_component = init_geom.get_component<MeshComponent>();
        std::vector<glm::vec3> init_positions;

        if (init_mesh_component) {
            init_positions = init_mesh_component->get_vertices();
        }
        else {
            auto init_points_component =
                init_geom.get_component<PointsComponent>();
            if (init_points_component) {
                init_positions = init_points_component->get_vertices();
            }
        }
        // If Init Geometry is provided, use it as the starting point for
        // simulation
        if (!init_positions.empty()) {
            // Check topology consistency
            bool topology_matches = (init_positions.size() == positions.size());

            if (topology_matches) {
                // Write init positions to GPU buffer as simulation starting
                // point
                storage.positions_buffer->assign_host_vector(init_positions);
                spdlog::info(
                    "[ReducedNeoHookean] Using Init Geometry as simulation "
                    "starting point (vertices={})",
                    init_positions.size());
            }
            else {
                spdlog::warn(
                    "[ReducedNeoHookean] Init Geometry topology mismatch! "
                    "Init: {} vertices; Rest pose: {} vertices. Using rest "
                    "pose as "
                    "starting point.",
                    init_positions.size(),
                    positions.size());
            }
        }
    }

    // Apply initial transform on the first frame
    if (global_payload.is_simulating == false && initial_transform) {
        // Update Dirichlet boundary conditions from face quantities
        // Find all vertices that belong to faces marked as dirichlet
        std::set<int> bc_vertices;
        if (!dirichlet_face_values.empty() &&
            dirichlet_face_values.size() == face_counts.size()) {
            int face_idx = 0;
            int vertex_offset = 0;
            for (int face = 0; face < face_counts.size(); ++face) {
                // If this face is marked as dirichlet (non-zero value)
                if (dirichlet_face_values[face] > 0.5f) {
                    int num_verts = face_counts[face];
                    for (int v = 0; v < num_verts; ++v) {
                        int vert_idx = face_vertex_indices[vertex_offset + v];
                        bc_vertices.insert(vert_idx);
                    }
                }
                vertex_offset += face_counts[face];
            }
        }

        // Convert vertex indices to DOF indices (each vertex has 3 DOFs: x, y,
        // z)
        storage.bc_dofs.clear();
        for (int v : bc_vertices) {
            storage.bc_dofs.push_back(v * 3 + 0);  // x DOF
            storage.bc_dofs.push_back(v * 3 + 1);  // y DOF
            storage.bc_dofs.push_back(v * 3 + 2);  // z DOF
        }
        storage.num_bc_dofs = storage.bc_dofs.size();

        // Upload BC DOFs to GPU
        if (storage.num_bc_dofs > 0) {
            storage.bc_dofs_buffer =
                cuda::create_cuda_linear_buffer(storage.bc_dofs);
            spdlog::info(
                "[ReducedNeoHookean] Dirichlet BC applied to {} vertices ({} "
                "DOFs)",
                bc_vertices.size(),
                storage.num_bc_dofs);
        }
        else {
            spdlog::info(
                "[ReducedNeoHookean] No Dirichlet boundary conditions");
        }

        int reduced_dof = storage.num_basis * 12;

        // Validate that transform has the right number of modes
        if (initial_transform->num_modes() != storage.num_basis) {
            spdlog::warn(
                "[ReducedNeoHookean] Initial transform has {} modes but "
                "reduced basis has {} modes",
                initial_transform->num_modes(),
                storage.num_basis);
        }
        else {
            auto q_host = storage.q_reduced->get_host_vector<float>();

            // For each basis, copy the transform from initial_transform
            for (int i = 0; i < storage.num_basis; ++i) {
                const auto& transform = initial_transform->get_transform(i);

                // Copy all 12 DOF (rotation matrix + translation)
                for (int j = 0; j < 12; ++j) {
                    q_host[i * 12 + j] = transform[j];
                }
            }

            storage.q_reduced->assign_host_vector(q_host);
            spdlog::info(
                "[ReducedNeoHookean] Applied initial transform to all {} bases",
                storage.num_basis);
        }
    }

    if (!storage.initialized || storage.num_elements == 0) {
        spdlog::warn(
            "[NeoHookean] Neo-Hookean simulation requires tetrahedral mesh. "
            "Skipping simulation.");
        params.set_output<Geometry>("Geometry", std::move(input_geom));
        return true;
    }

    auto d_positions = storage.positions_buffer;
    auto d_M_diag = storage.mass_matrix_buffer;
    auto d_gradients = storage.gradients_buffer;

    // Substep loop
    float dt_sub = dt / substeps;

    // Track statistics
    int max_newton_iterations = 0;
    int max_line_search_iterations = 0;

    spdlog::debug(
        "[ReducedNeoHookean] Starting simulation with {} reduced DOF",
        storage.num_basis);
    spdlog::debug(
        "[ReducedNeoHookean] dt={:.4f}, substeps={}, gravity={:.2f}",
        dt,
        substeps,
        gravity);

    for (int substep = 0; substep < substeps; ++substep) {
        if (substep == 0) {
            spdlog::debug(
                "[ReducedNeoHookean] Substep {}/{}, dt_sub={:.4f}",
                substep + 1,
                substeps,
                dt_sub);
        }

        // Compute q_tilde = q + dt_sub * q_dot in reduced space
        rzsim_cuda::explicit_step_reduced_gpu(
            storage.q_reduced,
            storage.q_dot_reduced,
            dt_sub,
            storage.num_basis,
            storage.q_tilde_reduced);

        // Newton's method iterations in reduced space
        storage.q_new_reduced->copy_from_device(storage.q_tilde_reduced.Get());

        bool converged = false;
        int newton_iter_count = 0;

        // Log initial energy before Newton iterations
        if (substep == 0) {
            rzsim_cuda::map_reduced_to_full_gpu(
                storage.q_new_reduced, storage.ro_data, storage.x_new_buffer);

            rzsim_cuda::map_reduced_to_full_gpu(
                storage.q_tilde_reduced,
                storage.ro_data,
                storage.next_positions_buffer);

            float initial_energy = rzsim_cuda::compute_energy_nh_gpu(
                storage.x_new_buffer,
                storage.next_positions_buffer,
                d_M_diag,
                *storage.volume_adjacency,
                storage.Dm_inv_buffer,
                storage.volumes_buffer,
                mu,
                lambda,
                density,
                gravity,
                dt_sub,
                num_particles,
                storage.num_elements,
                storage.inertial_terms_buffer,
                storage.element_energies_buffer);

            // Add barrier energy
            if (consider_bc && storage.num_bc_dofs > 0) {
                float barrier_energy = rzsim_cuda::compute_barrier_energy_gpu(
                    storage.bc_dofs_buffer,
                    storage.num_bc_dofs,
                    storage.x_new_buffer,
                    storage.ro_data.rest_positions,
                    barrier_stiffness,
                    barrier_width,
                    num_particles,
                    storage.barrier_energy_buffer);

                initial_energy += barrier_energy;
            }

            spdlog::debug(
                "[ReducedNeoHookean] Initial energy before Newton: {:.6f}",
                initial_energy);
        }

        for (int iter = 0; iter < max_iterations; iter++) {
            // Map q_new to full space positions
            rzsim_cuda::map_reduced_to_full_gpu(
                storage.q_new_reduced, storage.ro_data, storage.x_new_buffer);

            // Compute Jacobian J = dx/dq
            rzsim_cuda::compute_jacobian_gpu(
                storage.q_new_reduced, storage.ro_data, storage.jacobian);

            // Compute gradient in full space
            // For reduced order, we don't use velocities in the inertial term
            // Instead, we use q_tilde mapped to full space
            rzsim_cuda::map_reduced_to_full_gpu(
                storage.q_tilde_reduced,
                storage.ro_data,
                storage.next_positions_buffer);

            // Compute negative gradient in full space (for Newton's method)
            rzsim_cuda::compute_neg_gradient_nh_gpu(
                storage.x_new_buffer,
                storage.next_positions_buffer,
                d_M_diag,
                *storage.volume_adjacency,
                storage.Dm_inv_buffer,
                storage.volumes_buffer,
                mu,
                lambda,
                density,
                gravity,
                dt_sub,
                num_particles,
                storage.num_elements,
                d_gradients);

            // Add barrier gradient contribution (before applying BC)
            if (consider_bc && storage.num_bc_dofs > 0) {
                // Barrier gradient is added (not negated), so we need to
                // subtract it since d_gradients currently holds -∇E
                rzsim_cuda::add_barrier_gradient_gpu(
                    storage.bc_dofs_buffer,
                    storage.num_bc_dofs,
                    storage.x_new_buffer,
                    storage.ro_data.rest_positions,
                    -barrier_stiffness,  // Negate because d_gradients is -∇E
                    barrier_width,
                    num_particles,
                    d_gradients);
            }

            // Project negative gradient to reduced space: -grad_q = J^T *
            // (-grad_x) Since we already have negative gradient, just use
            // regular projection
            rzsim_cuda::compute_reduced_gradient_gpu(
                storage.cublas_handle,
                storage.jacobian,
                d_gradients,
                num_particles,
                storage.num_basis,
                storage.neg_gradient_reduced);

            // Compute gradient norm in reduced space (norm of -g equals norm of
            // g)
            int reduced_dof = storage.num_basis * 12;
            float grad_norm = rzsim_cuda::compute_vector_norm_nh_gpu(
                storage.neg_gradient_reduced, reduced_dof);

            if (!std::isfinite(grad_norm)) {
                spdlog::error(
                    "[ReducedNeoHookean] Gradient norm is not finite! "
                    "Simulation "
                    "unstable.");
                break;
            }

            grad_norm = grad_norm / reduced_dof;

            newton_iter_count = iter;

            // Log first few iterations for debugging
            if (substep == 0 && iter < 3) {
                spdlog::debug(
                    "[ReducedNeoHookean]   Newton iter {}: grad_norm={:.6e}",
                    iter,
                    grad_norm);
            }

            // Run at least one iteration
            if (iter > 0 && grad_norm < tolerance) {
                converged = true;
                if (substep == 0) {
                    spdlog::debug(
                        "[ReducedNeoHookean]   Converged at iteration {} with "
                        "grad_norm={:.6e}",
                        iter,
                        grad_norm);
                }
                break;
            }

            // Update Hessian values in full space
            rzsim_cuda::update_hessian_values_nh_gpu(
                storage.hessian_structure,
                storage.x_new_buffer,
                d_M_diag,
                *storage.volume_adjacency,
                storage.Dm_inv_buffer,
                storage.volumes_buffer,
                mu,
                lambda,
                dt_sub,
                num_particles,
                storage.num_elements,
                storage.hessian_values);

            // Add barrier Hessian contributions (before applying BC)
            if (consider_bc && storage.num_bc_dofs > 0) {
                // Zero out diagonal buffer first
                cudaMemset(
                    reinterpret_cast<void*>(
                        storage.barrier_hessian_diagonal->get_device_ptr()),
                    0,
                    num_particles * 3 * sizeof(float));

                // Compute barrier Hessian diagonal contributions
                rzsim_cuda::add_barrier_hessian_diagonal_gpu(
                    storage.bc_dofs_buffer,
                    storage.num_bc_dofs,
                    storage.x_new_buffer,
                    storage.ro_data.rest_positions,
                    barrier_stiffness,
                    barrier_width,
                    num_particles,
                    storage.barrier_hessian_diagonal);

                // Add to Hessian diagonal
                rzsim_cuda::add_to_hessian_diagonal_gpu(
                    storage.hessian_structure,
                    storage.barrier_hessian_diagonal,
                    num_particles * 3,
                    storage.hessian_values);
            }

            // Project Hessian to reduced space: H_q = J^T * H_x * J
            // Use unmodified Jacobian (BC constraints already in H_x)
            rzsim_cuda::compute_reduced_hessian_gpu(
                storage.cublas_handle,
                storage.cusparse_handle,
                storage.hessian_csr_descriptor,
                storage.jacobian_descriptor,
                storage.temp_hessian_descriptor,
                storage.cusparse_workspace,
                storage.hessian_structure,
                storage.hessian_values,
                storage.jacobian,
                num_particles,
                storage.num_basis,
                storage.temp_hessian_buffer,
                storage.hessian_reduced);

            // Solve H_q * p = -grad_q using cuSOLVER dense Cholesky
            // H_q is dense symmetric positive definite [reduced_dof x
            // reduced_dof]
            Ruzino::Solver::SolverConfig solver_config;
            solver_config.tolerance = std::max(1e-8f, grad_norm * 1e-3f);
            solver_config.verbose = false;

            auto solver_result = storage.solver->solveDenseGPU(
                reduced_dof,
                storage.hessian_reduced->get_device_ptr<float>(),
                storage.neg_gradient_reduced->get_device_ptr<float>(),
                storage.newton_direction_reduced->get_device_ptr<float>(),
                solver_config);

            if (!solver_result.converged) {
                spdlog::warn(
                    "[ReducedNeoHookean] Dense solver failed at Newton iter "
                    "{}: {}",
                    iter,
                    solver_result.error_message);
                // Fall back to gradient descent
                storage.newton_direction_reduced->copy_from_device(
                    storage.neg_gradient_reduced.Get());
            }
            else if (substep == 0 && iter < 3) {
                spdlog::debug(
                    "[ReducedNeoHookean]   Solver: {} μs",
                    solver_result.solve_time.count());
            }

            // Line search with energy descent
            rzsim_cuda::map_reduced_to_full_gpu(
                storage.q_new_reduced, storage.ro_data, storage.x_new_buffer);

            float E_current = rzsim_cuda::compute_energy_nh_gpu(
                storage.x_new_buffer,
                storage.next_positions_buffer,
                d_M_diag,
                *storage.volume_adjacency,
                storage.Dm_inv_buffer,
                storage.volumes_buffer,
                mu,
                lambda,
                density,
                gravity,
                dt_sub,
                num_particles,
                storage.num_elements,
                storage.inertial_terms_buffer,
                storage.element_energies_buffer);

            // Add barrier energy
            if (consider_bc && storage.num_bc_dofs > 0) {
                float barrier_energy = rzsim_cuda::compute_barrier_energy_gpu(
                    storage.bc_dofs_buffer,
                    storage.num_bc_dofs,
                    storage.x_new_buffer,
                    storage.ro_data.rest_positions,
                    barrier_stiffness,
                    barrier_width,
                    num_particles,
                    storage.barrier_energy_buffer);
                E_current += barrier_energy;
            }

            float E_candidate = std::numeric_limits<float>::infinity();
            float alpha = 1.0f;  // Start with full step
            int ls_iter = 0;

            while (E_candidate > E_current && ls_iter < 200) {
                // q_candidate = q_new + alpha * p
                rzsim_cuda::axpy_nh_gpu(
                    alpha,
                    storage.newton_direction_reduced,
                    storage.q_new_reduced,
                    storage.q_candidate_reduced,
                    reduced_dof);

                // Map to full space and compute energy
                rzsim_cuda::map_reduced_to_full_gpu(
                    storage.q_candidate_reduced,
                    storage.ro_data,
                    storage.x_candidate_buffer);

                E_candidate = rzsim_cuda::compute_energy_nh_gpu(
                    storage.x_candidate_buffer,
                    storage.next_positions_buffer,
                    d_M_diag,
                    *storage.volume_adjacency,
                    storage.Dm_inv_buffer,
                    storage.volumes_buffer,
                    mu,
                    lambda,
                    density,
                    gravity,
                    dt_sub,
                    num_particles,
                    storage.num_elements,
                    storage.inertial_terms_buffer,
                    storage.element_energies_buffer);

                // Add barrier energy
                if (consider_bc && storage.num_bc_dofs > 0) {
                    float barrier_energy =
                        rzsim_cuda::compute_barrier_energy_gpu(
                            storage.bc_dofs_buffer,
                            storage.num_bc_dofs,
                            storage.x_candidate_buffer,
                            storage.ro_data.rest_positions,
                            barrier_stiffness,
                            barrier_width,
                            num_particles,
                            storage.barrier_energy_buffer);
                    E_candidate += barrier_energy;
                }

                // Accept step if energy decreases OR if the increase is within
                // numerical precision
                float energy_tolerance =
                    std::max(1e-6f, std::abs(E_current) * 1e-6f);
                bool accept = (E_candidate <= E_current) ||
                              (E_candidate - E_current < energy_tolerance);
                if (accept) {
                    storage.q_new_reduced->copy_from_device(
                        storage.q_candidate_reduced.Get());
                    break;
                }

                alpha *= 0.5f;
                ls_iter++;
            }

            if (ls_iter >= 200 || alpha < 1e-6f) {
                spdlog::warn(
                    "[ReducedNeoHookean] Line search failed at iter {} "
                    "(ls_iter={}, "
                    "alpha={:.6e})",
                    iter,
                    ls_iter,
                    alpha);
                break;
            }

            // Update max line search iterations
            max_line_search_iterations =
                std::max(max_line_search_iterations, ls_iter);
        }

        // Update max Newton iterations
        max_newton_iterations =
            std::max(max_newton_iterations, newton_iter_count);

        // Check if Newton method converged
        if (!converged) {
            spdlog::warn(
                "[ReducedNeoHookean] Newton method did not converge after {} "
                "iterations",
                max_iterations);
        }

        // Update reduced velocities: q_dot = (q_new - q_old) / dt * damping
        rzsim_cuda::update_reduced_velocities_gpu(
            storage.q_new_reduced,
            storage.q_reduced,
            dt_sub,
            damping,
            storage.num_basis,
            storage.q_dot_reduced);

        // Copy final reduced coordinates back
        storage.q_reduced->copy_from_device(storage.q_new_reduced.Get());
    }

    // Map final reduced coordinates to full space positions
    rzsim_cuda::map_reduced_to_full_gpu(
        storage.q_reduced, storage.ro_data, d_positions);

    // Log simulation statistics
    spdlog::info(
        "[ReducedNeoHookean] Simulation complete - Max Newton iterations: "
        "{}, "
        "Max line search iterations: {}",
        max_newton_iterations,
        max_line_search_iterations);

    // Get final positions for geometry update
    auto final_positions = d_positions->get_host_vector<glm::vec3>();

    // Update geometry with new positions
    if (mesh_component) {
        mesh_component->set_vertices(final_positions);

        // Recalculate normals on GPU
        rzsim_cuda::compute_normals_gpu(
            storage.positions_buffer,
            storage.face_vertex_indices_buffer,
            storage.face_counts_buffer,
            flip_normal,
            storage.normals_buffer);

        auto normals = storage.normals_buffer->get_host_vector<glm::vec3>();
        mesh_component->set_normals(normals);
    }
    else {
        auto points_component = input_geom.get_component<PointsComponent>();
        points_component->set_vertices(final_positions);
    }

    params.set_output<Geometry>("Geometry", std::move(input_geom));
    return true;
}

NODE_DECLARATION_UI(reduced_order_neo_hookean_gpu);
NODE_DEF_CLOSE_SCOPE
