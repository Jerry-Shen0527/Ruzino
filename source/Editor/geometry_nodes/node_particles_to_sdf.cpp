#include "GCore/Components/PointsComponent.h"
#include "GCore/GOP.h"
#include "GCore/Texture/Texture.h"
#include "geom_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "spdlog/spdlog.h"

#include <cmath>
#include <limits>

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(particles_to_sdf)
{
    b.add_input<Geometry>("Particles");
    b.add_input<int>("Resolution").default_val(256).min(64).max(2048);
    b.add_input<float>("Paper Size").default_val(1.0f).min(0.1f).max(100.0f);
    b.add_input<float>("Particle Radius")
        .default_val(0.005f)
        .min(0.001f)
        .max(0.1f);

    b.add_output<TextureHandle>("SDF");
}

NODE_EXECUTION_FUNCTION(particles_to_sdf)
{
    auto particles = params.get_input<Geometry>("Particles");
    int resolution = params.get_input<int>("Resolution");
    float paper_size = params.get_input<float>("Paper Size");
    float particle_radius = params.get_input<float>("Particle Radius");

    auto sdf = std::make_shared<DataTexture2D>();
    sdf->resize(resolution, resolution);
    sdf->set_wrap_mode(DataTexture2D::WrapMode::Clamp);

    auto points = particles.get_component<PointsComponent>();
    if (!points) {
        params.set_output("SDF", sdf);
        return true;
    }

    auto vertices = points->get_vertices();
    float half_size = paper_size * 0.5f;
    float cell_size = paper_size / resolution;

    // Step 1: Binary occupancy grid
    std::vector<bool> occupied(resolution * resolution, false);
    for (const auto& v : vertices) {
        int gx = static_cast<int>((v.x + half_size) / paper_size * resolution);
        int gy = static_cast<int>((v.y + half_size) / paper_size * resolution);
        if (gx >= 0 && gx < resolution && gy >= 0 && gy < resolution) {
            occupied[gy * resolution + gx] = true;
            int spread = std::max(
                static_cast<int>(particle_radius / cell_size), 1);
            for (int dy = -spread; dy <= spread; dy++) {
                for (int dx = -spread; dx <= spread; dx++) {
                    int px = gx + dx;
                    int py = gy + dy;
                    if (px >= 0 && px < resolution && py >= 0 &&
                        py < resolution) {
                        float dist = std::sqrt(
                            static_cast<float>(dx * dx + dy * dy)) *
                            cell_size;
                        if (dist <= particle_radius) {
                            occupied[py * resolution + px] = true;
                        }
                    }
                }
            }
        }
    }

    // Step 2: Brute-force distance transform
    // TODO: Replace with jump flooding algorithm for better performance
    int search_radius = std::min(resolution / 4, 64);
    for (int y = 0; y < resolution; y++) {
        for (int x = 0; x < resolution; x++) {
            float min_dist = std::numeric_limits<float>::max();
            for (int dy = -search_radius; dy <= search_radius; dy++) {
                for (int dx = -search_radius; dx <= search_radius; dx++) {
                    int px = x + dx;
                    int py = y + dy;
                    if (px >= 0 && px < resolution && py >= 0 &&
                        py < resolution && occupied[py * resolution + px]) {
                        float dist = std::sqrt(
                            static_cast<float>(dx * dx + dy * dy)) *
                            cell_size;
                        min_dist = std::min(min_dist, dist);
                    }
                }
            }

            float sdf_value;
            if (occupied[y * resolution + x]) {
                sdf_value = -min_dist;
            }
            else if (min_dist < std::numeric_limits<float>::max()) {
                sdf_value = min_dist;
            }
            else {
                sdf_value = paper_size;
            }

            sdf->set_pixel(x, y, glm::vec4(sdf_value, 0.0f, 0.0f, 1.0f));
        }
    }

    spdlog::info(
        "particles_to_sdf: {} particles -> {}x{} SDF",
        vertices.size(),
        resolution,
        resolution);

    params.set_output("SDF", sdf);
    return true;
}

NODE_DECLARATION_UI(particles_to_sdf);

NODE_DEF_CLOSE_SCOPE
