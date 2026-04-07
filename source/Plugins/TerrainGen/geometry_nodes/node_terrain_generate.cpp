#include "GCore/Components/MeshComponent.h"
#include "TerrainGen/TerrainGeneration.h"
#include "TerrainGen/TerrainParameters.h"
#include "TerrainGen/TerrainStructure.h"
#include "geom_node_base.h"

using namespace TerrainGen;

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(terrain_generate)
{
    // Grid parameters
    b.add_input<int>("Grid Resolution").min(32).max(512).default_val(128);
    b.add_input<float>("Grid Size").min(10.0f).max(1000.0f).default_val(100.0f);
    b.add_input<float>("Max Height").min(1.0f).max(200.0f).default_val(50.0f);

    // Noise parameters
    b.add_input<int>("Octaves").min(1).max(10).default_val(6);
    b.add_input<float>("Frequency").min(0.1f).max(10.0f).default_val(1.0f);
    b.add_input<float>("Persistence").min(0.1f).max(0.9f).default_val(0.5f);
    b.add_input<float>("Lacunarity").min(1.0f).max(4.0f).default_val(2.0f);

    // Multi-scale features
    b.add_input<bool>("Enable Multi Scale").default_val(true);
    b.add_input<float>("Mountain Amplitude")
        .min(0.0f)
        .max(100.0f)
        .default_val(30.0f);
    b.add_input<float>("Valley Depth").min(0.0f).max(50.0f).default_val(10.0f);
    b.add_input<float>("Hill Amplitude").min(0.0f).max(20.0f).default_val(5.0f);

    // Erosion parameters
    b.add_input<bool>("Enable Erosion").default_val(false);
    b.add_input<int>("Erosion Iterations")
        .min(0)
        .max(100000)
        .default_val(10000);
    b.add_input<float>("Erosion Strength")
        .min(0.0f)
        .max(1.0f)
        .default_val(0.3f);
    b.add_input<float>("Deposition Strength")
        .min(0.0f)
        .max(1.0f)
        .default_val(0.3f);

    // Thermal erosion
    b.add_input<bool>("Enable Thermal Erosion").default_val(false);
    b.add_input<int>("Thermal Iterations").min(0).max(20).default_val(5);

    // Other parameters
    b.add_input<int>("Random Seed").min(0).max(10000).default_val(42);
    b.add_input<bool>("Island Mode").default_val(false);

    // Output
    b.add_output<Geometry>("Height Field");
}

NODE_EXECUTION_FUNCTION(terrain_generate)
{
    // Get parameters from inputs
    TerrainParameters terrain_params;

    terrain_params.grid_resolution = params.get_input<int>("Grid Resolution");
    terrain_params.grid_size = params.get_input<float>("Grid Size");
    terrain_params.max_height = params.get_input<float>("Max Height");

    terrain_params.octaves = params.get_input<int>("Octaves");
    terrain_params.frequency = params.get_input<float>("Frequency");
    terrain_params.persistence = params.get_input<float>("Persistence");
    terrain_params.lacunarity = params.get_input<float>("Lacunarity");

    terrain_params.enable_multi_scale =
        params.get_input<bool>("Enable Multi Scale");
    terrain_params.mountain_amplitude =
        params.get_input<float>("Mountain Amplitude");
    terrain_params.valley_depth = params.get_input<float>("Valley Depth");
    terrain_params.hill_amplitude = params.get_input<float>("Hill Amplitude");

    terrain_params.enable_erosion = params.get_input<bool>("Enable Erosion");
    terrain_params.erosion_iterations =
        params.get_input<int>("Erosion Iterations");
    terrain_params.erosion_strength =
        params.get_input<float>("Erosion Strength");
    terrain_params.deposition_strength =
        params.get_input<float>("Deposition Strength");

    terrain_params.enable_thermal_erosion =
        params.get_input<bool>("Enable Thermal Erosion");
    terrain_params.thermal_iterations =
        params.get_input<int>("Thermal Iterations");

    terrain_params.random_seed = params.get_input<int>("Random Seed");
    terrain_params.island_mode = params.get_input<bool>("Island Mode");

    // Create terrain generator
    TerrainGenerator generator;

    // Generate terrain
    auto terrain = generator.generate(terrain_params);

    // Convert terrain height field to curve geometry (for visualization)
    // We'll store the height field as attributes on a mesh
    Geometry heightfield_geom = Geometry::CreateMesh();
    auto mesh = heightfield_geom.get_component<MeshComponent>();

    auto& field = *terrain->height_field;

    // Create a simple grid mesh to hold height data
    std::vector<glm::vec3> vertices;
    std::vector<int> face_vertex_counts;
    std::vector<int> face_vertex_indices;
    std::vector<float> height_attribute;

    // Generate grid vertices
    for (int y = 0; y < field.height; ++y) {
        for (int x = 0; x < field.width; ++x) {
            float nx = static_cast<float>(x) / (field.width - 1);
            float ny = static_cast<float>(y) / (field.height - 1);

            float world_x = (nx - 0.5f) * field.world_width;
            float world_z = (ny - 0.5f) * field.world_height;
            float height = field.at(x, y);

            vertices.push_back(glm::vec3(world_x, height, world_z));
            height_attribute.push_back(height);
        }
    }

    // Generate faces (triangles)
    for (int y = 0; y < field.height - 1; ++y) {
        for (int x = 0; x < field.width - 1; ++x) {
            int i0 = y * field.width + x;
            int i1 = y * field.width + (x + 1);
            int i2 = (y + 1) * field.width + x;
            int i3 = (y + 1) * field.width + (x + 1);

            // Triangle 1
            face_vertex_counts.push_back(3);
            face_vertex_indices.push_back(i0);
            face_vertex_indices.push_back(i2);
            face_vertex_indices.push_back(i1);

            // Triangle 2
            face_vertex_counts.push_back(3);
            face_vertex_indices.push_back(i1);
            face_vertex_indices.push_back(i2);
            face_vertex_indices.push_back(i3);
        }
    }

    // Set mesh data
    mesh->set_vertices(vertices);
    mesh->set_face_vertex_counts(face_vertex_counts);
    mesh->set_face_vertex_indices(face_vertex_indices);

    // Calculate and set normals
    std::vector<glm::vec3> normals;
    for (int y = 0; y < field.height; ++y) {
        for (int x = 0; x < field.width; ++x) {
            glm::vec3 normal = field.get_normal(x, y);
            normals.push_back(normal);
        }
    }
    mesh->set_normals(normals);

    // Set UV coordinates (using XZ positions)
    std::vector<glm::vec2> uv_coords;
    uv_coords.reserve(vertices.size());
    for (const auto& v : vertices) {
        uv_coords.push_back(glm::vec2(v.x, v.z));
    }
    mesh->set_texcoords_array(uv_coords);

    // Set height as vertex color for visualization
    std::vector<glm::vec3> colors;
    colors.reserve(vertices.size());
    for (const auto& v : vertices) {
        float normalized_height = (v.y + 1.0f) * 0.5f;  // Normalize to [0,1]
        colors.push_back(
            glm::vec3(normalized_height, normalized_height, normalized_height));
    }
    mesh->set_display_color(colors);

    params.set_output("Height Field", heightfield_geom);

    return true;
}

NODE_DEF_CLOSE_SCOPE
