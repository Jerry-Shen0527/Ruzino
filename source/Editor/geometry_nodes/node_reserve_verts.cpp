#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <unordered_map>

#include "GCore/Components/MeshComponent.h"
#include "GCore/GOP.h"
#include "nodes/core/def/node_def.hpp"

using namespace Ruzino;

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(reserve_verts)
{
    b.add_input<Geometry>("Geometry");
    b.add_input<std::string>("Mask Name").default_val("mask");
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(reserve_verts)
{
    // 获取输入参数
    Geometry input_geometry = params.get_input<Geometry>("Geometry");
    std::string mask_name = params.get_input<std::string>("Mask Name");

    // 获取网格组件
    auto mesh_component = input_geometry.get_component<MeshComponent>();
    if (!mesh_component) {
        return false;
    }

    // 获取mask数据
    std::vector<float> mask =
        mesh_component->get_vertex_scalar_quantity(mask_name);
    if (mask.empty()) {
        return false;
    }

    // 获取原始数据
    std::vector<glm::vec3> vertices = mesh_component->get_vertices();
    std::vector<int> face_vertex_counts =
        mesh_component->get_face_vertex_counts();
    std::vector<int> face_vertex_indices =
        mesh_component->get_face_vertex_indices();

    if (vertices.size() != mask.size()) {
        return false;
    }

    // 创建顶点映射表 (old_index -> new_index)
    std::unordered_map<int, int> vertex_map;
    std::vector<glm::vec3> new_vertices;
    new_vertices.reserve(vertices.size());

    int new_vertex_index = 0;
    for (int i = 0; i < static_cast<int>(vertices.size()); ++i) {
        if (mask[i] > 0.5f) {  // mask为1的顶点
            vertex_map[i] = new_vertex_index++;
            new_vertices.push_back(vertices[i]);
        }
    }

    // 如果没有保留的顶点，返回空几何体
    if (new_vertices.empty()) {
        // 创建空的网格组件
        mesh_component->set_vertices({});
        mesh_component->set_face_vertex_counts({});
        mesh_component->set_face_vertex_indices({});
        params.set_output("Geometry", input_geometry);
        return true;
    }

    // 重建面数据
    std::vector<int> new_face_vertex_counts;
    std::vector<int> new_face_vertex_indices;

    int face_start = 0;
    for (int face_size : face_vertex_counts) {
        std::vector<int> face_vertices;
        face_vertices.reserve(face_size);

        // 检查面的所有顶点是否都被保留
        bool valid_face = true;
        for (int i = 0; i < face_size; ++i) {
            int old_vertex_index = face_vertex_indices[face_start + i];
            auto it = vertex_map.find(old_vertex_index);
            if (it == vertex_map.end()) {
                valid_face = false;
                break;
            }
            face_vertices.push_back(it->second);
        }

        // 如果面的所有顶点都被保留，则添加到新的面数据中
        if (valid_face && face_vertices.size() >= 3) {
            new_face_vertex_counts.push_back(
                static_cast<int>(face_vertices.size()));
            new_face_vertex_indices.insert(
                new_face_vertex_indices.end(),
                face_vertices.begin(),
                face_vertices.end());
        }

        face_start += face_size;
    }

    // 更新网格数据
    mesh_component->set_vertices(new_vertices);
    mesh_component->set_face_vertex_counts(new_face_vertex_counts);
    mesh_component->set_face_vertex_indices(new_face_vertex_indices);

    // 处理其他顶点属性（标量、颜色、向量等）
    // 获取所有顶点标量量
    auto scalar_names = mesh_component->get_vertex_scalar_quantity_names();
    for (const auto& name : scalar_names) {
        std::vector<float> old_data =
            mesh_component->get_vertex_scalar_quantity(name);
        if (old_data.size() == vertices.size()) {
            std::vector<float> new_data;
            new_data.reserve(new_vertices.size());
            for (int i = 0; i < static_cast<int>(vertices.size()); ++i) {
                if (mask[i] > 0.5f) {
                    new_data.push_back(old_data[i]);
                }
            }
            mesh_component->add_vertex_scalar_quantity(name, new_data);
        }
    }

    // 处理顶点向量量
    auto vector_names = mesh_component->get_vertex_vector_quantity_names();
    for (const auto& name : vector_names) {
        std::vector<glm::vec3> old_data =
            mesh_component->get_vertex_vector_quantity(name);
        if (old_data.size() == vertices.size()) {
            std::vector<glm::vec3> new_data;
            new_data.reserve(new_vertices.size());
            for (int i = 0; i < static_cast<int>(vertices.size()); ++i) {
                if (mask[i] > 0.5f) {
                    new_data.push_back(old_data[i]);
                }
            }
            mesh_component->add_vertex_vector_quantity(name, new_data);
        }
    }

    // 处理顶点参数化量
    auto param_names =
        mesh_component->get_vertex_parameterization_quantity_names();
    for (const auto& name : param_names) {
        std::vector<glm::vec2> old_data =
            mesh_component->get_vertex_parameterization_quantity(name);
        if (old_data.size() == vertices.size()) {
            std::vector<glm::vec2> new_data;
            new_data.reserve(new_vertices.size());
            for (int i = 0; i < static_cast<int>(vertices.size()); ++i) {
                if (mask[i] > 0.5f) {
                    new_data.push_back(old_data[i]);
                }
            }
            mesh_component->add_vertex_parameterization_quantity(
                name, new_data);
        }
    }

    // 处理显示颜色
    std::vector<glm::vec3> display_color = mesh_component->get_display_color();
    if (display_color.size() == vertices.size()) {
        std::vector<glm::vec3> new_display_color;
        new_display_color.reserve(new_vertices.size());
        for (int i = 0; i < static_cast<int>(vertices.size()); ++i) {
            if (mask[i] > 0.5f) {
                new_display_color.push_back(display_color[i]);
            }
        }
        mesh_component->set_display_color(new_display_color);
    }

    // 处理法线 - 支持 per-vertex 和 face-varying
    std::vector<glm::vec3> normals = mesh_component->get_normals();
    if (!normals.empty()) {
        if (normals.size() == vertices.size()) {
            // Per-vertex normals
            std::vector<glm::vec3> new_normals;
            new_normals.reserve(new_vertices.size());
            for (int i = 0; i < static_cast<int>(vertices.size()); ++i) {
                if (mask[i] > 0.5f) {
                    new_normals.push_back(normals[i]);
                }
            }
            mesh_component->set_normals(new_normals);
        }
        else if (normals.size() == face_vertex_indices.size()) {
            // Face-varying normals - need to rebuild based on kept faces
            std::vector<glm::vec3> new_normals;
            new_normals.reserve(new_face_vertex_indices.size());

            int old_face_start = 0;
            int old_normal_start = 0;
            for (int face_size : face_vertex_counts) {
                // Check if this face is kept
                bool face_kept = true;
                for (int i = 0; i < face_size; ++i) {
                    int old_vertex_index =
                        face_vertex_indices[old_face_start + i];
                    if (vertex_map.find(old_vertex_index) == vertex_map.end()) {
                        face_kept = false;
                        break;
                    }
                }

                // If face is kept, copy its normals
                if (face_kept && face_size >= 3) {
                    for (int i = 0; i < face_size; ++i) {
                        new_normals.push_back(normals[old_normal_start + i]);
                    }
                }

                old_face_start += face_size;
                old_normal_start += face_size;
            }
            mesh_component->set_normals(new_normals);
        }
    }

    // 处理纹理坐标 - 支持 per-vertex 和 face-varying
    std::vector<glm::vec2> texcoords = mesh_component->get_texcoords_array();
    if (!texcoords.empty()) {
        if (texcoords.size() == vertices.size()) {
            // Per-vertex texcoords
            std::vector<glm::vec2> new_texcoords;
            new_texcoords.reserve(new_vertices.size());
            for (int i = 0; i < static_cast<int>(vertices.size()); ++i) {
                if (mask[i] > 0.5f) {
                    new_texcoords.push_back(texcoords[i]);
                }
            }
            mesh_component->set_texcoords_array(new_texcoords);
        }
        else if (texcoords.size() == face_vertex_indices.size()) {
            // Face-varying texcoords - need to rebuild based on kept faces
            std::vector<glm::vec2> new_texcoords;
            new_texcoords.reserve(new_face_vertex_indices.size());

            int old_face_start = 0;
            int old_texcoord_start = 0;
            for (int face_size : face_vertex_counts) {
                // Check if this face is kept
                bool face_kept = true;
                for (int i = 0; i < face_size; ++i) {
                    int old_vertex_index =
                        face_vertex_indices[old_face_start + i];
                    if (vertex_map.find(old_vertex_index) == vertex_map.end()) {
                        face_kept = false;
                        break;
                    }
                }

                // If face is kept, copy its texcoords
                if (face_kept && face_size >= 3) {
                    for (int i = 0; i < face_size; ++i) {
                        new_texcoords.push_back(
                            texcoords[old_texcoord_start + i]);
                    }
                }

                old_face_start += face_size;
                old_texcoord_start += face_size;
            }
            mesh_component->set_texcoords_array(new_texcoords);
        }
    }

    // 输出结果
    params.set_output("Geometry", input_geometry);
    return true;
}

NODE_DECLARATION_UI(reserve_verts);

NODE_DEF_CLOSE_SCOPE