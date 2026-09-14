// terrain_heightfield: first node of the terrain family. Generates the base
// heightfield from the hash-seeded noise stack (NoiseCore) and packs it as a
// grid mesh for the erode nodes downstream.
//
// The legacy terrain_generate sampled glm::perlin at (p + seed*0.1), which
// correlated every octave and made reseeding a uniform field translation.
// Everything here derives from NoiseCore's integer-hash lattice instead.

#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "GCore/Components/MeshComponent.h"
#include "TerrainGen/Heightfield.h"
#include "TerrainGen/NoiseCore.h"
#include "geom_node_base.h"
#include "terrain_carry.hpp"

using namespace TerrainGen;

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(terrain_heightfield)
{
    b.add_input<int>("Resolution").min(32).max(2048).default_val(512);
    b.add_input<float>("Size").min(10.0f).max(4000.0f).default_val(200.0f);
    b.add_input<float>("Height").min(0.1f).max(500.0f).default_val(40.0f);
    b.add_input<float>("Base Level").min(-100.0f).max(100.0f).default_val(0.0f);

    b.add_input<int>("Seed").min(0).max(1073741823).default_val(0);
    b.add_input<int>("Octaves").min(1).max(12).default_val(8);
    b.add_input<float>("Frequency").min(0.05f).max(16.0f).default_val(1.5f);
    b.add_input<float>("Persistence").min(0.1f).max(0.95f).default_val(0.5f);
    b.add_input<float>("Lacunarity").min(1.2f).max(4.0f).default_val(2.0f);

    b.add_input<std::string>("Noise Type")
        .default_val("fBm");  // fBm | Ridged Multifractal | Billow | Worley
    b.add_input<float>("Ridge Blend").min(0.0f).max(1.0f).default_val(0.5f);

    b.add_input<float>("Warp Strength").min(0.0f).max(2.0f).default_val(0.4f);
    b.add_input<float>("Warp Frequency").min(0.1f).max(8.0f).default_val(1.0f);

    b.add_input<bool>("Island Falloff").default_val(false);
    b.add_input<float>("Island Radius").min(0.1f).max(1.0f).default_val(0.7f);
    b.add_input<float>("Island Steepness")
        .min(0.5f)
        .max(8.0f)
        .default_val(2.0f);

    b.add_output<Geometry>("Height Field");
}

NODE_EXECUTION_FUNCTION(terrain_heightfield)
{
    Heightfield hf;
    const int res = params.get_input<int>("Resolution");
    hf.alloc(res, params.get_input<float>("Size"));

    const float height = params.get_input<float>("Height");
    const float base_level = params.get_input<float>("Base Level");
    const uint32_t seed = static_cast<uint32_t>(params.get_input<int>("Seed"));
    const int octaves = params.get_input<int>("Octaves");
    const float frequency = params.get_input<float>("Frequency");
    const float persistence = params.get_input<float>("Persistence");
    const float lacunarity = params.get_input<float>("Lacunarity");

    const std::string noise_type = params.get_input<std::string>("Noise Type");
    const float ridge_blend = params.get_input<float>("Ridge Blend");

    const float warp_strength = params.get_input<float>("Warp Strength");
    const float warp_frequency = params.get_input<float>("Warp Frequency");

    const bool island = params.get_input<bool>("Island Falloff");
    const float island_radius = params.get_input<float>("Island Radius");
    const float island_steepness = params.get_input<float>("Island Steepness");

    const int n = res;

    const auto eval_noise = [&](float x, float y, uint32_t s) {
        // Normalized to [0, 1].
        if (noise_type == "Ridged Multifractal")
            return NoiseCore::ridged_multifractal(
                x, y, s, octaves, frequency, persistence, lacunarity);
        if (noise_type == "Billow")
            return NoiseCore::billow(
                x, y, s, octaves, frequency, persistence, lacunarity);
        if (noise_type == "Worley")
            return NoiseCore::worley_f1(x * frequency, y * frequency, s);
        // fBm
        return (NoiseCore::fbm(
                    x, y, s, octaves, frequency, persistence, lacunarity) +
                1.0f) *
               0.5f;
    };

    // One seed per octave is handled inside NoiseCore; the ridge layer gets
    // a fully independent field.
    const uint32_t ridge_seed = NoiseCore::octave_seed(seed, 977);

    const auto fill_rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float ny = static_cast<float>(y) / static_cast<float>(n - 1);
            for (int x = 0; x < n; ++x) {
                const float nx =
                    static_cast<float>(x) / static_cast<float>(n - 1);

                float sx = nx, sy = ny;
                if (warp_strength > 0.0f) {
                    const glm::vec2 warped = NoiseCore::domain_warp(
                        nx, ny, seed, warp_frequency, warp_strength);
                    sx = warped.x;
                    sy = warped.y;
                }

                float h = eval_noise(sx, sy, seed);
                if (ridge_blend > 0.0f) {
                    const float ridge = NoiseCore::ridged_multifractal(
                        sx,
                        sy,
                        ridge_seed,
                        octaves,
                        frequency,
                        persistence,
                        lacunarity);
                    h = h * (1.0f - ridge_blend) + ridge * ridge_blend;
                }

                if (island) {
                    const float dx = nx - 0.5f;
                    const float dy = ny - 0.5f;
                    const float dist = std::sqrt(dx * dx + dy * dy) /
                                       std::max(island_radius, 1e-3f);
                    const float falloff = std::clamp(dist, 0.0f, 1.0f);
                    h *= 1.0f - std::pow(falloff, island_steepness);
                }

                hf.at(x, y) = base_level + height * h;
            }
        }
    };

    int thread_count = static_cast<int>(std::thread::hardware_concurrency());
    thread_count = std::max(1, std::min(thread_count, n));
    const int chunk = (n + thread_count - 1) / thread_count;
    std::vector<std::thread> workers;
    workers.reserve(thread_count - 1);
    for (int t = 1; t < thread_count; ++t) {
        const int y0 = t * chunk;
        if (y0 >= n)
            break;
        workers.emplace_back(fill_rows, y0, std::min(y0 + chunk, n));
    }
    fill_rows(0, std::min(chunk, n));
    for (auto& w : workers)
        w.join();

    params.set_output("Height Field", terrain_carry::mesh_from_heightfield(hf));
    return true;
}

NODE_DEF_CLOSE_SCOPE
