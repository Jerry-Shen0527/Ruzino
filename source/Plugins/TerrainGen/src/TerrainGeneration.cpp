#include "TerrainGen/TerrainGeneration.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/noise.hpp>
#include <random>

namespace TerrainGen {

TerrainGenerator::TerrainGenerator()
{
}

TerrainGenerator::~TerrainGenerator()
{
}

std::shared_ptr<TerrainStructure> TerrainGenerator::generate(
    const TerrainParameters& params)
{
    auto terrain = std::make_shared<TerrainStructure>(
        params.grid_resolution, params.grid_size);
    terrain->generation_seed = params.random_seed;

    // Step 1: Generate base heightmap using noise
    generate_base_heightmap(*terrain, params);

    // Step 2: Add multi-scale features
    if (params.enable_multi_scale) {
        apply_multi_scale_features(*terrain, params);
    }

    // Step 3: Apply hydraulic erosion
    if (params.enable_erosion) {
        apply_hydraulic_erosion(*terrain, params);
        terrain->has_erosion = true;
    }

    // Step 4: Apply thermal erosion
    if (params.enable_thermal_erosion) {
        apply_thermal_erosion(*terrain, params);
    }

    // Step 5: Generate climate maps if biomes enabled
    if (params.enable_biomes) {
        generate_climate_maps(*terrain, params);
        apply_biomes(*terrain, params);
        terrain->has_biomes = true;
    }

    // Step 6: Post-processing (smoothing, curves, etc.)
    apply_post_processing(*terrain, params);

    // Update statistics
    terrain->update_statistics();

    return terrain;
}

void TerrainGenerator::generate_base_heightmap(
    TerrainStructure& terrain,
    const TerrainParameters& params)
{
    auto& field = *terrain.height_field;

    for (int y = 0; y < field.height; ++y) {
        for (int x = 0; x < field.width; ++x) {
            // Normalize coordinates to [0, 1]
            float nx = static_cast<float>(x) / (field.width - 1);
            float ny = static_cast<float>(y) / (field.height - 1);

            // Apply domain warping if enabled
            float sample_x = nx;
            float sample_y = ny;

            if (params.enable_domain_warp) {
                glm::vec2 warped = domain_warp(
                    nx,
                    ny,
                    params.domain_warp_strength,
                    params.domain_warp_frequency,
                    params.random_seed);
                sample_x = warped.x;
                sample_y = warped.y;
            }

            // Generate base noise
            float height =
                fbm(sample_x,
                    sample_y,
                    params.octaves,
                    params.frequency,
                    params.persistence,
                    params.lacunarity,
                    params.random_seed,
                    params.noise_type);

            // Normalize to [0, 1]
            height = (height + 1.0f) * 0.5f;

            // Apply island mode if enabled
            if (params.island_mode) {
                float dx = nx - params.island_center.x;
                float dy = ny - params.island_center.y;
                float distance =
                    glm::sqrt(dx * dx + dy * dy) * 2.0f;  // Scale to [0, 2]
                float falloff = glm::pow(
                    glm::clamp(distance, 0.0f, 1.0f), params.island_falloff);
                height = height * (1.0f - falloff);
            }

            // Scale to final height range
            height = params.min_height +
                     height * (params.max_height - params.min_height);
            height *= params.height_scale;

            field.at(x, y) = height;
        }
    }
}

void TerrainGenerator::apply_multi_scale_features(
    TerrainStructure& terrain,
    const TerrainParameters& params)
{
    auto& field = *terrain.height_field;

    for (int y = 0; y < field.height; ++y) {
        for (int x = 0; x < field.width; ++x) {
            float nx = static_cast<float>(x) / (field.width - 1);
            float ny = static_cast<float>(y) / (field.height - 1);

            float current_height = field.at(x, y);

            // Mountain layer (large-scale, sharp peaks)
            float mountain_noise = ridged_noise(
                nx * params.mountain_scale,
                ny * params.mountain_scale,
                params.random_seed + 1000);
            mountain_noise = glm::pow(
                (mountain_noise + 1.0f) * 0.5f, params.mountain_sharpness);
            current_height += mountain_noise * params.mountain_amplitude;

            // Valley layer (medium-scale, negative)
            float valley_noise = perlin_noise(
                nx * params.valley_scale,
                ny * params.valley_scale,
                params.random_seed + 2000);
            valley_noise = glm::smoothstep(
                params.valley_width, 1.0f, (valley_noise + 1.0f) * 0.5f);
            current_height -= (1.0f - valley_noise) * params.valley_depth;

            // Hill layer (medium-scale)
            float hill_noise = perlin_noise(
                nx * params.hill_scale,
                ny * params.hill_scale,
                params.random_seed + 3000);
            current_height +=
                ((hill_noise + 1.0f) * 0.5f) * params.hill_amplitude;

            // Detail layer (small-scale)
            float detail_noise = perlin_noise(
                nx * params.detail_scale,
                ny * params.detail_scale,
                params.random_seed + 4000);
            current_height +=
                ((detail_noise + 1.0f) * 0.5f) * params.detail_amplitude;

            field.at(x, y) = current_height;
        }
    }
}

void TerrainGenerator::apply_hydraulic_erosion(
    TerrainStructure& terrain,
    const TerrainParameters& params)
{
    for (int i = 0; i < params.erosion_iterations; ++i) {
        simulate_hydraulic_erosion_droplet(
            *terrain.height_field, params, params.random_seed + i);
    }
    terrain.erosion_iterations_done = params.erosion_iterations;
}

void TerrainGenerator::simulate_hydraulic_erosion_droplet(
    HeightField& field,
    const TerrainParameters& params,
    int seed)
{
    // Initialize random droplet position
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    float pos_x = dist(rng) * (field.width - 1);
    float pos_y = dist(rng) * (field.height - 1);

    glm::vec2 dir(0.0f);
    float speed = 1.0f;
    float water = params.initial_water_volume;
    float sediment = 0.0f;

    for (int lifetime = 0; lifetime < params.max_droplet_lifetime; ++lifetime) {
        int node_x = static_cast<int>(pos_x);
        int node_y = static_cast<int>(pos_y);

        // Check bounds
        if (node_x < 0 || node_x >= field.width - 1 || node_y < 0 ||
            node_y >= field.height - 1) {
            break;
        }

        // Calculate droplet's offset inside the cell
        float cell_offset_x = pos_x - node_x;
        float cell_offset_y = pos_y - node_y;

        // Calculate height and gradient
        float height_nw = field.at(node_x, node_y);
        float height_ne = field.at(node_x + 1, node_y);
        float height_sw = field.at(node_x, node_y + 1);
        float height_se = field.at(node_x + 1, node_y + 1);

        // Bilinear interpolation for height
        float height = height_nw * (1 - cell_offset_x) * (1 - cell_offset_y) +
                       height_ne * cell_offset_x * (1 - cell_offset_y) +
                       height_sw * (1 - cell_offset_x) * cell_offset_y +
                       height_se * cell_offset_x * cell_offset_y;

        // Calculate gradient
        float gradient_x = (height_ne - height_nw) * (1 - cell_offset_y) +
                           (height_se - height_sw) * cell_offset_y;
        float gradient_y = (height_sw - height_nw) * (1 - cell_offset_x) +
                           (height_se - height_ne) * cell_offset_x;

        // Update direction and speed with inertia
        dir = dir * params.water_inertia -
              glm::vec2(gradient_x, gradient_y) * (1.0f - params.water_inertia);
        if (glm::length(dir) != 0.0f) {
            dir = glm::normalize(dir);
        }

        float new_pos_x = pos_x + dir.x;
        float new_pos_y = pos_y + dir.y;

        // Sample new height
        float new_height = field.sample(new_pos_x, new_pos_y);

        // Calculate height difference
        float delta_height = new_height - height;

        // Calculate sediment capacity
        float capacity = glm::max(-delta_height, params.min_sediment_capacity) *
                         speed * water * params.erosion_strength;

        // Erode or deposit sediment
        if (sediment > capacity || delta_height > 0) {
            // Deposit
            float amount_to_deposit =
                (delta_height > 0)
                    ? glm::min(delta_height, sediment)
                    : (sediment - capacity) * params.deposition_strength;

            sediment -= amount_to_deposit;

            // Deposit in a radius around current position
            float deposit_per_cell = amount_to_deposit / 4.0f;
            field.at(node_x, node_y) +=
                deposit_per_cell * (1 - cell_offset_x) * (1 - cell_offset_y);
            field.at(node_x + 1, node_y) +=
                deposit_per_cell * cell_offset_x * (1 - cell_offset_y);
            field.at(node_x, node_y + 1) +=
                deposit_per_cell * (1 - cell_offset_x) * cell_offset_y;
            field.at(node_x + 1, node_y + 1) +=
                deposit_per_cell * cell_offset_x * cell_offset_y;
        }
        else {
            // Erode
            float amount_to_erode = glm::min(
                (capacity - sediment) * params.erosion_strength, -delta_height);

            // Erode from cells in brush radius
            for (int brush_y = -params.erosion_brush_radius;
                 brush_y <= params.erosion_brush_radius;
                 ++brush_y) {
                for (int brush_x = -params.erosion_brush_radius;
                     brush_x <= params.erosion_brush_radius;
                     ++brush_x) {
                    int erode_x = node_x + brush_x;
                    int erode_y = node_y + brush_y;

                    if (erode_x >= 0 && erode_x < field.width && erode_y >= 0 &&
                        erode_y < field.height) {
                        float dist = glm::sqrt(
                            static_cast<float>(
                                brush_x * brush_x + brush_y * brush_y));
                        float weight = glm::max(
                            0.0f, 1.0f - dist / params.erosion_brush_radius);
                        float weighted_erode = amount_to_erode * weight;

                        field.at(erode_x, erode_y) -= weighted_erode;
                        sediment += weighted_erode;
                    }
                }
            }
        }

        // Update position and physics
        speed = glm::sqrt(speed * speed + delta_height * params.gravity);
        water *= (1.0f - params.evaporation_rate);
        pos_x = new_pos_x;
        pos_y = new_pos_y;
    }
}

void TerrainGenerator::apply_thermal_erosion(
    TerrainStructure& terrain,
    const TerrainParameters& params)
{
    for (int iter = 0; iter < params.thermal_iterations; ++iter) {
        apply_thermal_erosion_step(
            *terrain.height_field, params.talus_angle, params.thermal_strength);
    }
}

void TerrainGenerator::apply_thermal_erosion_step(
    HeightField& field,
    float talus_angle,
    float strength)
{
    std::vector<float> new_heights = field.heights;

    float max_diff =
        glm::tan(talus_angle) * (field.world_width / (field.width - 1));

    for (int y = 1; y < field.height - 1; ++y) {
        for (int x = 1; x < field.width - 1; ++x) {
            float center_height = field.at(x, y);
            float total_diff = 0.0f;
            float moved_material = 0.0f;

            // Check all 8 neighbors
            const int dx[] = { -1, 0, 1, -1, 1, -1, 0, 1 };
            const int dy[] = { -1, -1, -1, 0, 0, 1, 1, 1 };

            for (int i = 0; i < 8; ++i) {
                int nx = x + dx[i];
                int ny = y + dy[i];

                float neighbor_height = field.at(nx, ny);
                float diff = center_height - neighbor_height;

                if (diff > max_diff) {
                    float excess = (diff - max_diff) * strength;
                    total_diff += excess;
                    new_heights[ny * field.width + nx] += excess / 8.0f;
                    moved_material += excess / 8.0f;
                }
            }

            new_heights[y * field.width + x] -= moved_material;
        }
    }

    field.heights = new_heights;
}

void TerrainGenerator::generate_climate_maps(
    TerrainStructure& terrain,
    const TerrainParameters& params)
{
    auto& field = *terrain.height_field;
    auto& climate = *terrain.climate_map;

    for (int y = 0; y < field.height; ++y) {
        for (int x = 0; x < field.width; ++x) {
            float nx = static_cast<float>(x) / (field.width - 1);
            float ny = static_cast<float>(y) / (field.height - 1);

            // Generate temperature
            float temp_noise = perlin_noise(
                nx * params.temperature_frequency,
                ny * params.temperature_frequency,
                params.random_seed + 5000);
            float temperature =
                params.base_temperature +
                ((temp_noise + 1.0f) * 0.5f) * params.temperature_range;

            // Adjust for altitude
            float altitude = field.at(x, y);
            temperature +=
                altitude * params.altitude_temperature_factor / 1000.0f;

            climate.temp_at(x, y) = temperature;

            // Generate moisture
            float moisture_noise = perlin_noise(
                nx * params.moisture_frequency,
                ny * params.moisture_frequency,
                params.random_seed + 6000);
            float moisture =
                params.base_moisture +
                ((moisture_noise + 1.0f) * 0.5f) * params.moisture_range;
            moisture = glm::clamp(moisture, 0.0f, 1.0f);

            climate.moisture_at(x, y) = moisture;
        }
    }
}

void TerrainGenerator::apply_biomes(
    TerrainStructure& terrain,
    const TerrainParameters& params)
{
    auto& field = *terrain.height_field;
    auto& climate = *terrain.climate_map;

    for (int y = 0; y < field.height; ++y) {
        for (int x = 0; x < field.width; ++x) {
            float temperature = climate.temp_at(x, y);
            float moisture = climate.moisture_at(x, y);
            float altitude = field.at(x, y);

            BiomeType biome = classify_biome(temperature, moisture, altitude);

            // Apply biome-specific height modifications
            float modifier = 1.0f;

            switch (biome) {
                case BiomeType::Mountain:
                    modifier = 1.2f;  // Higher peaks
                    break;
                case BiomeType::Desert:
                    modifier = 0.8f;  // Flatter
                    break;
                case BiomeType::Ocean:
                    modifier = 0.3f;  // Lower
                    break;
                default: modifier = 1.0f;
            }

            // Blend with original height
            float modified_height =
                glm::mix(altitude, altitude * modifier, params.biome_influence);
            field.at(x, y) = modified_height;
        }
    }
}

BiomeType TerrainGenerator::classify_biome(
    float temperature,
    float moisture,
    float altitude) const
{
    // Ocean (below sea level)
    if (altitude < 5.0f) {
        return BiomeType::Ocean;
    }

    // Beach (low altitude)
    if (altitude < 8.0f) {
        return BiomeType::Beach;
    }

    // Mountain (high altitude)
    if (altitude > 40.0f) {
        if (temperature < 0.0f) {
            return BiomeType::Snow;
        }
        return BiomeType::Mountain;
    }

    // Tundra (cold)
    if (temperature < -10.0f) {
        return BiomeType::Tundra;
    }

    // Taiga (cold forest)
    if (temperature < 5.0f && moisture > 0.4f) {
        return BiomeType::Taiga;
    }

    // Desert (hot and dry)
    if (moisture < 0.2f && temperature > 20.0f) {
        return BiomeType::Desert;
    }

    // Savanna (warm and semi-dry)
    if (moisture < 0.4f && temperature > 15.0f) {
        return BiomeType::Savanna;
    }

    // Rainforest (hot and wet)
    if (moisture > 0.7f && temperature > 20.0f) {
        return BiomeType::Rainforest;
    }

    // Forest (moderate, wet)
    if (moisture > 0.5f) {
        return BiomeType::Forest;
    }

    // Default: Grassland
    return BiomeType::Grassland;
}

void TerrainGenerator::apply_post_processing(
    TerrainStructure& terrain,
    const TerrainParameters& params)
{
    auto& field = *terrain.height_field;

    // Apply height curve
    if (params.height_curve != 1.0f) {
        float min_h = field.heights[0];
        float max_h = field.heights[0];
        for (float h : field.heights) {
            min_h = glm::min(min_h, h);
            max_h = glm::max(max_h, h);
        }

        for (int i = 0; i < field.heights.size(); ++i) {
            float normalized = (field.heights[i] - min_h) / (max_h - min_h);
            normalized = glm::pow(normalized, params.height_curve);
            field.heights[i] = min_h + normalized * (max_h - min_h);
        }
    }

    // Apply terracing
    if (params.enable_terracing && params.terrace_levels > 1) {
        float min_h = field.heights[0];
        float max_h = field.heights[0];
        for (float h : field.heights) {
            min_h = glm::min(min_h, h);
            max_h = glm::max(max_h, h);
        }

        float range = max_h - min_h;
        float level_height = range / params.terrace_levels;

        for (int i = 0; i < field.heights.size(); ++i) {
            float height = field.heights[i];
            float normalized = (height - min_h) / range;

            float level = glm::floor(normalized * params.terrace_levels);
            float next_level = level + 1.0f;

            float level_start = level * level_height + min_h;
            float level_end = next_level * level_height + min_h;

            float t = (height - level_start) / level_height;
            t = glm::smoothstep(0.0f, params.terrace_smoothness, t);

            field.heights[i] = glm::mix(level_start, level_end, t);
        }
    }

    // Apply smoothing
    if (params.smoothing > 0.0f) {
        std::vector<float> smoothed = field.heights;

        for (int y = 1; y < field.height - 1; ++y) {
            for (int x = 1; x < field.width - 1; ++x) {
                float sum = 0.0f;
                float weight = 0.0f;

                // 3x3 kernel
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        float w = (dx == 0 && dy == 0) ? 4.0f : 1.0f;
                        sum += field.at(x + dx, y + dy) * w;
                        weight += w;
                    }
                }

                float smoothed_height = sum / weight;
                smoothed[y * field.width + x] =
                    glm::mix(field.at(x, y), smoothed_height, params.smoothing);
            }
        }

        field.heights = smoothed;
    }
}

// Noise functions
float TerrainGenerator::perlin_noise(float x, float y, int seed) const
{
    return glm::perlin(glm::vec2(x + seed * 0.1f, y + seed * 0.1f));
}

float TerrainGenerator::simplex_noise(float x, float y, int seed) const
{
    return glm::simplex(glm::vec2(x + seed * 0.1f, y + seed * 0.1f));
}

float TerrainGenerator::worley_noise(float x, float y, int seed) const
{
    // Simple Worley/Voronoi noise implementation
    glm::vec2 p(x + seed * 0.1f, y + seed * 0.1f);
    glm::vec2 i = glm::floor(p);
    glm::vec2 f = p - i;  // Fractional part

    float min_dist = 10.0f;

    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            glm::vec2 neighbor =
                glm::vec2(static_cast<float>(dx), static_cast<float>(dy));
            glm::vec2 neighbor_i = i + neighbor;
            float dot_result = glm::dot(neighbor_i, glm::vec2(127.1f, 311.7f));
            float sin_result = glm::sin(dot_result) * 43758.5453f;
            glm::vec2 point = glm::vec2(
                sin_result - glm::floor(sin_result),
                glm::sin(dot_result + 1.0f) * 43758.5453f -
                    glm::floor(glm::sin(dot_result + 1.0f) * 43758.5453f));

            float dist = glm::length(neighbor + point - f);
            min_dist = glm::min(min_dist, dist);
        }
    }

    return min_dist * 2.0f - 1.0f;  // Normalize to [-1, 1]
}

float TerrainGenerator::ridged_noise(float x, float y, int seed) const
{
    float n = perlin_noise(x, y, seed);
    return 1.0f - glm::abs(n);  // Invert and fold
}

float TerrainGenerator::billow_noise(float x, float y, int seed) const
{
    float n = perlin_noise(x, y, seed);
    return glm::abs(n) * 2.0f - 1.0f;  // Absolute value, normalize
}

float TerrainGenerator::fbm(
    float x,
    float y,
    int octaves,
    float frequency,
    float persistence,
    float lacunarity,
    int seed,
    TerrainParameters::NoiseType type) const
{
    float total = 0.0f;
    float amplitude = 1.0f;
    float max_value = 0.0f;
    float freq = frequency;

    for (int i = 0; i < octaves; ++i) {
        float noise_value = 0.0f;

        switch (type) {
            case TerrainParameters::NoiseType::Perlin:
                noise_value = perlin_noise(x * freq, y * freq, seed + i);
                break;
            case TerrainParameters::NoiseType::Simplex:
                noise_value = simplex_noise(x * freq, y * freq, seed + i);
                break;
            case TerrainParameters::NoiseType::Worley:
                noise_value = worley_noise(x * freq, y * freq, seed + i);
                break;
            case TerrainParameters::NoiseType::Ridged:
                noise_value = ridged_noise(x * freq, y * freq, seed + i);
                break;
            case TerrainParameters::NoiseType::Billow:
                noise_value = billow_noise(x * freq, y * freq, seed + i);
                break;
        }

        total += noise_value * amplitude;
        max_value += amplitude;

        amplitude *= persistence;
        freq *= lacunarity;
    }

    return total / max_value;
}

glm::vec2 TerrainGenerator::domain_warp(
    float x,
    float y,
    float strength,
    float frequency,
    int seed) const
{
    float offset_x =
        perlin_noise(x * frequency, y * frequency, seed + 7000) * strength;
    float offset_y =
        perlin_noise(x * frequency, y * frequency, seed + 8000) * strength;

    return glm::vec2(x + offset_x, y + offset_y);
}

float TerrainGenerator::smoothstep(float edge0, float edge1, float x) const
{
    float t = glm::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float TerrainGenerator::remap(
    float value,
    float in_min,
    float in_max,
    float out_min,
    float out_max) const
{
    return out_min + (value - in_min) * (out_max - out_min) / (in_max - in_min);
}

float TerrainGenerator::random_float(int& seed) const
{
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    return static_cast<float>(seed) / static_cast<float>(0x7fffffff);
}

glm::vec2 TerrainGenerator::random_vec2(int& seed) const
{
    return glm::vec2(random_float(seed), random_float(seed));
}

}  // namespace TerrainGen
