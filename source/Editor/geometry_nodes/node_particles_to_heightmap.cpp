#include "GCore/Components/PointsComponent.h"
#include "GCore/GOP.h"
#include "GCore/Texture/Texture.h"
#include "geom_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(particles_to_heightmap)
{
    b.add_input<Geometry>("Particles");
    b.add_input<int>("Resolution").default_val(512).min(64).max(4096);
    b.add_input<float>("Paper Size").default_val(1.0f).min(0.1f).max(100.0f);
    b.add_input<float>("Splat Radius").default_val(0.01f).min(0.001f).max(0.1f);
    b.add_input<float>("Height Scale").default_val(1.0f).min(0.0f).max(10.0f);

    b.add_output<TextureHandle>("Height Map");
}

NODE_EXECUTION_FUNCTION(particles_to_heightmap)
{
    auto particles = params.get_input<Geometry>("Particles");
    int resolution = params.get_input<int>("Resolution");
    float paper_size = params.get_input<float>("Paper Size");
    float splat_radius = params.get_input<float>("Splat Radius");
    float height_scale = params.get_input<float>("Height Scale");

    auto points = particles.get_component<PointsComponent>();

    // Create height map texture
    auto height_map = std::make_shared<DataTexture2D>();
    height_map->resize(resolution, resolution);
    height_map->set_wrap_mode(DataTexture2D::WrapMode::Clamp);

    if (!points) {
        params.set_output("Height Map", height_map);
        return true;
    }

    auto vertices = points->get_vertices();
    float half_size = paper_size * 0.5f;

    // Splat particles onto the grid
    for (const auto& v : vertices) {
        float u = (v.x + half_size) / paper_size;
        float t = (v.y + half_size) / paper_size;

        int cx = static_cast<int>(u * resolution);
        int cy = static_cast<int>(t * resolution);

        int spread = static_cast<int>(
            std::ceil(splat_radius / paper_size * resolution));
        spread = std::max(spread, 1);
        for (int dy = -spread; dy <= spread; dy++) {
            for (int dx = -spread; dx <= spread; dx++) {
                int px = cx + dx;
                int py = cy + dy;
                if (px < 0 || px >= resolution || py < 0 || py >= resolution)
                    continue;

                float dist_sq =
                    static_cast<float>(dx * dx + dy * dy) /
                    (spread * spread);
                if (dist_sq > 1.0f) continue;

                float weight = std::exp(-dist_sq * 3.0f);
                glm::vec4 current = height_map->get_pixel(px, py);
                current.r += weight * height_scale;
                height_map->set_pixel(px, py, current);
            }
        }
    }

    spdlog::info(
        "particles_to_heightmap: {} particles -> {}x{} height map",
        vertices.size(),
        resolution,
        resolution);

    params.set_output("Height Map", height_map);
    return true;
}

NODE_DECLARATION_UI(particles_to_heightmap);

NODE_DEF_CLOSE_SCOPE
