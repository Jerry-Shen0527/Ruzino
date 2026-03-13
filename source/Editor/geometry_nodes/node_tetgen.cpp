
#include "GCore/Components/MeshComponent.h"
#include "GCore/GOP.h"
#include "GCore/algorithms/tetgen_algorithm.h"
#include "nodes/core/def/node_def.hpp"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE
NODE_DECLARATION_FUNCTION(tetgen_tetrahedralize)
{
    b.add_input<Geometry>("Surface Mesh");
    b.add_input<float>("Quality Ratio").min(1.0f).max(10.0f).default_val(2.0f);
    b.add_input<float>("Max Volume")
        .min(0.0001f)
        .max(1000.0f)
        .default_val(0.01f);
    b.add_input<bool>("Refine").default_val(true);
    b.add_input<bool>("Conforming Delaunay").default_val(true);
    b.add_input<std::string>("Preserve Attribute").default_val("");
    b.add_output<Geometry>("Tetrahedral Mesh");
}

NODE_EXECUTION_FUNCTION(tetgen_tetrahedralize)
{
    auto geometry = params.get_input<Geometry>("Surface Mesh");
    geometry.apply_transform();
    float quality_ratio = params.get_input<float>("Quality Ratio");
    float max_volume = params.get_input<float>("Max Volume");
    bool refine = params.get_input<bool>("Refine");
    bool conforming = params.get_input<bool>("Conforming Delaunay");
    std::string preserve_attr =
        params.get_input<std::string>("Preserve Attribute");

    try {
        // Use algorithm layer
        geom_algorithm::TetgenParams tet_params;
        tet_params.quality_ratio = quality_ratio;
        tet_params.max_volume = max_volume;
        tet_params.refine = refine;
        tet_params.conforming_delaunay = conforming;
        tet_params.quiet = true;
        tet_params.preserve_face_attribute = preserve_attr;

        Geometry output_geometry =
            geom_algorithm::tetrahedralize(geometry, tet_params);

        // Calculate normals for visualization
        auto output_mesh = output_geometry.get_component<MeshComponent>();
        if (output_mesh) {
            const auto& output_points = output_mesh->get_vertices();
            const auto& output_indices = output_mesh->get_face_vertex_indices();
            const auto& output_face_counts =
                output_mesh->get_face_vertex_counts();

            std::vector<glm::vec3> normals;
            normals.reserve(output_indices.size());

            for (size_t i = 0; i < output_face_counts.size(); ++i) {
                size_t idx_start = i * 3;
                int i0 = output_indices[idx_start];
                int i1 = output_indices[idx_start + 1];
                int i2 = output_indices[idx_start + 2];

                glm::vec3 v0 = output_points[i0];
                glm::vec3 v1 = output_points[i1];
                glm::vec3 v2 = output_points[i2];

                glm::vec3 edge1 = v1 - v0;
                glm::vec3 edge2 = v2 - v0;
                glm::vec3 normal = glm::cross(edge2, edge1);

                float length = glm::length(normal);
                if (length > 1e-8f) {
                    normal = normal / length;
                }
                else {
                    normal = glm::vec3(0.0f, 1.0f, 0.0f);
                }

                normals.push_back(normal);
                normals.push_back(normal);
                normals.push_back(normal);
            }

            output_mesh->set_normals(normals);
        }

        params.set_output("Tetrahedral Mesh", std::move(output_geometry));
    }
    catch (const std::exception& e) {
        params.set_error((std::string("TetGen error: ") + e.what()).c_str());
        spdlog::error("TetGen error: {}", e.what());
        return false;
    }

    return true;
}

NODE_DECLARATION_UI(tetgen_tetrahedralize);

NODE_DEF_CLOSE_SCOPE
