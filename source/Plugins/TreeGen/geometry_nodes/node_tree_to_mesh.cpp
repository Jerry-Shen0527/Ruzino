#include <cmath>
#include <glm/gtx/rotate_vector.hpp>

#include "GCore/Components/CurveComponent.h"
#include "GCore/Components/MeshComponent.h"
#include "geom_node_base.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(tree_to_mesh)
{
    b.add_input<Geometry>("Tree Branches");
    b.add_input<Geometry>("Leaves");
    b.add_input<int>("Radial Segments").min(3).max(24).default_val(10);

    b.add_output<Geometry>("Branch Mesh");
    b.add_output<Geometry>("Leaf Mesh");
}

NODE_EXECUTION_FUNCTION(tree_to_mesh)
{
    auto tree_branches = params.get_input<Geometry>("Tree Branches");
    auto leaves_geom = params.get_input<Geometry>("Leaves");
    int radial_segments = params.get_input<int>("Radial Segments");
    radial_segments = std::max(3, std::min(32, radial_segments));

    tree_branches.apply_transform();
    auto curve = tree_branches.get_component<CurveComponent>();
    if (!curve) {
        // If no branches, output empty geometries
        params.set_output("Branch Mesh", Geometry::CreateMesh());
        params.set_output("Leaf Mesh", Geometry::CreateMesh());
        return true;
    }

    // Sweep each input polyline (one per unbranched shoot chain, with one
    // width per point) into a continuous tube: consecutive segments SHARE a
    // ring, so the surface has no steps at internode joints. Frames are
    // parallel-transported along the chain to avoid twisting. Each chain is
    // closed with a flat cap at its base and a pointed cone cap at its tip,
    // making every chain a closed (watertight) shell; forks are sealed by
    // the cone from the parent radius that each child chain starts with.
    Geometry branch_mesh_geom = Geometry::CreateMesh();
    auto branch_mesh = branch_mesh_geom.get_component<MeshComponent>();

    std::vector<glm::vec3> vertices;
    std::vector<int> face_vertex_counts;
    std::vector<int> face_vertex_indices;
    std::vector<glm::vec3> normals;

    auto curve_verts = curve->get_vertices();
    auto curve_counts = curve->get_curve_counts();
    auto curve_widths = curve->get_widths();

    size_t offset = 0;
    for (int point_count : curve_counts) {
        if (point_count < 2 || offset + point_count > curve_verts.size()) {
            offset += std::max(0, point_count);
            continue;
        }

        const int S = radial_segments;
        const int ring_vert_start = static_cast<int>(vertices.size());

        // Parallel-transport frame along the chain
        glm::vec3 dir_prev = glm::normalize(curve_verts[offset + 1] -
                                            curve_verts[offset]);
        glm::vec3 first_dir = dir_prev;
        glm::vec3 perp = glm::normalize(glm::cross(
            dir_prev,
            (std::abs(dir_prev.y) < 0.9f) ? glm::vec3(0.f, 1.f, 0.f)
                                          : glm::vec3(1.f, 0.f, 0.f)));

        for (int i = 0; i < point_count; ++i) {
            glm::vec3 center = curve_verts[offset + i];
            float width = (i < static_cast<int>(curve_widths.size()))
                              ? std::max(curve_widths[offset + i], 0.001f)
                              : 0.001f;

            glm::vec3 dir = dir_prev;
            if (i < point_count - 1) {
                glm::vec3 seg = curve_verts[offset + i + 1] - center;
                if (glm::length(seg) > 1e-7f)
                    dir = glm::normalize(seg);
            }

            // Rotate the frame from the previous segment to this one
            glm::vec3 axis = glm::cross(dir_prev, dir);
            float sin_angle = glm::length(axis);
            if (sin_angle > 1e-7f) {
                float cos_angle =
                    glm::clamp(glm::dot(dir_prev, dir), -1.0f, 1.0f);
                perp = glm::rotate(perp, std::atan2(sin_angle, cos_angle),
                                   glm::normalize(axis));
            } else if (glm::dot(dir_prev, dir) < 0.0f) {
                perp = -perp;  // 180-degree turn: flip around the tangent
            }
            perp = glm::normalize(perp - dir * glm::dot(perp, dir));
            glm::vec3 perp2 = glm::normalize(glm::cross(dir, perp));

            for (int s = 0; s < S; ++s) {
                float angle = (2.0f * 3.14159265f * s) / S;
                glm::vec3 radial = std::cos(angle) * perp +
                                   std::sin(angle) * perp2;
                vertices.push_back(center + radial * width);
                normals.push_back(radial);
            }

            dir_prev = dir;
        }

        // Tube surface: quads between consecutive (shared) rings
        for (int i = 0; i < point_count - 1; ++i) {
            int base_a = ring_vert_start + i * S;
            int base_b = base_a + S;
            for (int s = 0; s < S; ++s) {
                int s2 = (s + 1) % S;
                face_vertex_indices.push_back(base_a + s);
                face_vertex_indices.push_back(base_a + s2);
                face_vertex_indices.push_back(base_b + s2);
                face_vertex_indices.push_back(base_b + s);
                face_vertex_counts.push_back(4);
            }
        }

        // Base cap: flat fan closing the first ring
        {
            int center_idx = static_cast<int>(vertices.size());
            vertices.push_back(curve_verts[offset]);
            normals.push_back(-first_dir);
            for (int s = 0; s < S; ++s) {
                face_vertex_indices.push_back(center_idx);
                face_vertex_indices.push_back(ring_vert_start + s);
                face_vertex_indices.push_back(ring_vert_start +
                                              (s + 1) % S);
                face_vertex_counts.push_back(3);
            }
        }

        // Tip cap: cone to a point slightly beyond the last ring
        {
            int last_ring = ring_vert_start + (point_count - 1) * S;
            float tip_width =
                (static_cast<int>(curve_widths.size()) > offset + point_count - 1)
                    ? std::max(curve_widths[offset + point_count - 1], 0.001f)
                    : 0.001f;
            int center_idx = static_cast<int>(vertices.size());
            glm::vec3 tip = curve_verts[offset + point_count - 1] +
                            dir_prev * tip_width * 1.2f;
            vertices.push_back(tip);
            normals.push_back(dir_prev);
            for (int s = 0; s < S; ++s) {
                face_vertex_indices.push_back(center_idx);
                face_vertex_indices.push_back(last_ring + (s + 1) % S);
                face_vertex_indices.push_back(last_ring + s);
                face_vertex_counts.push_back(3);
            }
        }

        offset += point_count;
    }

    branch_mesh->set_vertices(vertices);
    branch_mesh->set_face_vertex_counts(face_vertex_counts);
    branch_mesh->set_face_vertex_indices(face_vertex_indices);
    branch_mesh->set_normals(normals);

    params.set_output("Branch Mesh", branch_mesh_geom);

    // Pass through leaf geometry unchanged
    params.set_output("Leaf Mesh", leaves_geom);

    return true;
}

NODE_DECLARATION_UI(tree_to_mesh);

NODE_DEF_CLOSE_SCOPE
