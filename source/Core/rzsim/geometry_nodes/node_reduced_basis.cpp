#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>

#include "GCore/Components/MeshComponent.h"
#include "GCore/geom_payload.hpp"
#include "nodes/core/def/node_def.hpp"
#include "nodes/core/io/json.hpp"
#include "rzsim/reduced_order_basis.h"

using namespace Ruzino;

NODE_DEF_OPEN_SCOPE

// Storage for caching the reduced basis computation
struct ReducedBasisStorage {
    constexpr static bool has_storage = false;

    std::shared_ptr<ReducedOrderedBasis> cached_basis;
    std::string cached_geometry_hash;
    int cached_num_modes = 0;
    bool cached_use_libigl = false;
    bool initialized = false;
};

NODE_DECLARATION_FUNCTION(reduced_basis)
{
    b.add_input<Geometry>("Geometry");
    b.add_input<int>("Dimension").default_val(2).min(2).max(3);
    b.add_input<int>("Num Modes").default_val(10).min(1).max(100);
    b.add_input<int>("Mode Index").default_val(0).min(0).max(99);
    b.add_input<bool>("Use libigl").default_val(false);

    b.add_input<std::string>("Attribute Name").default_val("mode");
    b.add_input<bool>("Consider Boundary Condition").default_val(false);
    b.add_input<bool>("Export to Files").default_val(false);
    b.add_input<std::string>("Export Directory")
        .default_val("./reduced_basis_export");

    b.add_output<Geometry>("Geometry");
    b.add_output<std::shared_ptr<ReducedOrderedBasis>>("Reduced Basis");
}

// Helper function to save reduced basis data to files
static void save_reduced_basis_data(
    const std::vector<glm::vec3>& vertices,
    const std::shared_ptr<ReducedOrderedBasis>& basis,
    const std::string& base_directory)
{
    namespace fs = std::filesystem;

    try {
        spdlog::info("[ReducedBasis] Starting export process...");
        spdlog::info("[ReducedBasis] Base directory: '{}'", base_directory);

        // Create timestamped directory
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);

        std::stringstream timestamp_ss;
        timestamp_ss << std::put_time(
            std::localtime(&time_t_now), "%Y%m%d_%H%M%S");
        std::string timestamp = timestamp_ss.str();

        fs::path export_path = fs::path(base_directory) / timestamp;

        spdlog::info(
            "[ReducedBasis] Creating export directory: {}",
            export_path.string());
        fs::create_directories(export_path);

        if (!fs::exists(export_path)) {
            spdlog::error(
                "[ReducedBasis] Failed to create directory: {}",
                export_path.string());
            return;
        }

        spdlog::info(
            "[ReducedBasis] Exporting data to: {}", export_path.string());

        // Save vertices (N x 3)
        {
            fs::path vertex_file = export_path / "vertices.txt";
            spdlog::info(
                "[ReducedBasis] Saving vertices to: {}", vertex_file.string());

            std::ofstream ofs(vertex_file);
            if (!ofs.is_open()) {
                spdlog::error(
                    "[ReducedBasis] Failed to open file: {}",
                    vertex_file.string());
                return;
            }

            ofs << std::scientific << std::setprecision(16);

            for (const auto& v : vertices) {
                ofs << v.x << " " << v.y << " " << v.z << "\n";
            }
            ofs.close();
            spdlog::info(
                "[ReducedBasis] Saved {} vertices to vertices.txt",
                vertices.size());
        }

        // Save eigenvalues
        {
            fs::path eigenvalue_file = export_path / "eigenvalues.txt";
            std::ofstream ofs(eigenvalue_file);
            if (!ofs.is_open()) {
                spdlog::error(
                    "[ReducedBasis] Failed to open file: {}",
                    eigenvalue_file.string());
                return;
            }

            ofs << std::scientific << std::setprecision(16);

            for (size_t i = 0; i < basis->eigenvalues.size(); ++i) {
                ofs << basis->eigenvalues[i] << "\n";
            }
            ofs.close();
            spdlog::info(
                "[ReducedBasis] Saved {} eigenvalues to eigenvalues.txt",
                basis->eigenvalues.size());
        }

        // Save each mode (N-dimensional arrays)
        spdlog::info("[ReducedBasis] Saving {} modes...", basis->basis.size());
        for (size_t mode_idx = 0; mode_idx < basis->basis.size(); ++mode_idx) {
            std::stringstream mode_filename;
            mode_filename << "mode_" << std::setw(3) << std::setfill('0')
                          << mode_idx << ".txt";

            fs::path mode_file = export_path / mode_filename.str();
            std::ofstream ofs(mode_file);
            if (!ofs.is_open()) {
                spdlog::error(
                    "[ReducedBasis] Failed to open file: {}",
                    mode_file.string());
                continue;
            }

            ofs << std::scientific << std::setprecision(16);

            const auto& mode = basis->basis[mode_idx];
            for (int i = 0; i < mode.size(); ++i) {
                ofs << mode(i) << "\n";
            }
            ofs.close();
        }

        spdlog::info(
            "[ReducedBasis] Saved {} modes to mode_*.txt", basis->basis.size());
        spdlog::info("[ReducedBasis] ========================================");
        spdlog::info("[ReducedBasis] Export complete!");
        spdlog::info(
            "[ReducedBasis] All files saved to: {}", export_path.string());
        spdlog::info("[ReducedBasis] ========================================");
    }
    catch (const std::exception& e) {
        spdlog::error("[ReducedBasis] Failed to export data: {}", e.what());
    }
}

NODE_EXECUTION_FUNCTION(reduced_basis)
{
    auto& storage = params.get_storage<ReducedBasisStorage&>();

    // Get inputs
    auto input_geom = params.get_input<Geometry>("Geometry");
    input_geom.apply_transform();

    int dimension = params.get_input<int>("Dimension");
    int num_modes = params.get_input<int>("Num Modes");
    int mode_index = params.get_input<int>("Mode Index");
    bool use_libigl = params.get_input<bool>("Use libigl");
    std::string attr_name = params.get_input<std::string>("Attribute Name");

    // Get mesh component
    auto mesh_component = input_geom.get_component<MeshComponent>();
    if (!mesh_component) {
        params.set_output<Geometry>("Geometry", std::move(input_geom));
        return true;
    }

    std::vector<glm::vec3> vertices = mesh_component->get_vertices();
    if (vertices.empty()) {
        params.set_output<Geometry>("Geometry", std::move(input_geom));
        return true;
    }

    // Get boundary condition vertices if requested
    bool consider_bc = params.get_input<bool>("Consider Boundary Condition");
    std::vector<int> bc_vertices;

    if (consider_bc) {
        // Try to get dirichlet face quantity
        std::vector<float> dirichlet_face_values =
            mesh_component->get_face_scalar_quantity("dirichlet");

        if (!dirichlet_face_values.empty()) {
            std::vector<int> face_vertex_indices =
                mesh_component->get_face_vertex_indices();
            std::vector<int> face_counts =
                mesh_component->get_face_vertex_counts();

            if (dirichlet_face_values.size() == face_counts.size()) {
                std::set<int> bc_vertex_set;
                int vertex_offset = 0;

                for (size_t face = 0; face < face_counts.size(); ++face) {
                    // If this face is marked as dirichlet (non-zero value)
                    if (dirichlet_face_values[face] > 0.5f) {
                        int num_verts = face_counts[face];
                        for (int v = 0; v < num_verts; ++v) {
                            int vert_idx =
                                face_vertex_indices[vertex_offset + v];
                            bc_vertex_set.insert(vert_idx);
                        }
                    }
                    vertex_offset += face_counts[face];
                }

                bc_vertices.assign(bc_vertex_set.begin(), bc_vertex_set.end());
                spdlog::info(
                    "[ReducedBasis] Found {} vertices with Dirichlet BC",
                    bc_vertices.size());
            }
        }
    }

    // Validate mode index
    if (mode_index >= num_modes) {
        mode_index = num_modes - 1;
    }
    if (mode_index < 0) {
        mode_index = 0;
    }

    // Compute or use cached reduced order basis
    if (!storage.initialized || storage.cached_num_modes != num_modes ||
        storage.cached_use_libigl != use_libigl || !storage.cached_basis) {
        try {
            storage.cached_basis = std::make_shared<ReducedOrderedBasis>(
                input_geom, num_modes, dimension, use_libigl, bc_vertices);
            storage.cached_num_modes = num_modes;
            storage.cached_use_libigl = use_libigl;
            storage.initialized = true;

            // Export to files if requested
            bool export_files = params.get_input<bool>("Export to Files");
            spdlog::info("[ReducedBasis] Export to Files = {}", export_files);

            if (export_files) {
                std::string export_dir =
                    params.get_input<std::string>("Export Directory");
                spdlog::info(
                    "[ReducedBasis] Export Directory = '{}'", export_dir);
                save_reduced_basis_data(
                    vertices, storage.cached_basis, export_dir);
            }
            else {
                spdlog::info(
                    "[ReducedBasis] File export is disabled. Enable 'Export to "
                    "Files' to save data.");
            }
        }
        catch (const std::exception& e) {
            spdlog::error(
                "Failed to compute reduced order basis: {}", e.what());
            params.set_output<Geometry>("Geometry", std::move(input_geom));
            return false;
        }
    }

    // Extract the scalar values from the selected mode
    if (mode_index >= static_cast<int>(storage.cached_basis->basis.size())) {
        spdlog::warn(
            "Mode index {} out of range, clamping to {}",
            mode_index,
            storage.cached_basis->basis.size() - 1);
        mode_index = storage.cached_basis->basis.size() - 1;
    }

    const auto& selected_mode = storage.cached_basis->basis[mode_index];
    float eigenvalue = storage.cached_basis->eigenvalues[mode_index];

    // Convert eigenvector to scalar quantities (per vertex)
    std::vector<float> mode_values;
    mode_values.reserve(selected_mode.size());

    for (int i = 0; i < selected_mode.size(); ++i) {
        mode_values.push_back(selected_mode(i));
    }

    // Add the mode as a vertex scalar quantity using the custom attribute name
    mesh_component->add_vertex_scalar_quantity(attr_name, mode_values);

    params.set_output<Geometry>("Geometry", std::move(input_geom));
    params.set_output<std::shared_ptr<ReducedOrderedBasis>>(
        "Reduced Basis",
        std::shared_ptr<ReducedOrderedBasis>(storage.cached_basis));
    return true;
}

NODE_DECLARATION_UI(reduced_basis);

NODE_DEF_CLOSE_SCOPE
