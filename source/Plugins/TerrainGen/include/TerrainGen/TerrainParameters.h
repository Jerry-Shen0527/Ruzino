#pragma once

#include <glm/glm.hpp>

namespace TerrainGen {

// Procedural terrain generation parameters
// Supports multi-scale noise, erosion simulation, and biome-based generation
struct TerrainParameters {
    // ========== Grid Parameters ==========

    // Grid resolution (number of vertices per side)
    int grid_resolution = 256;

    // Grid size (world units)
    float grid_size = 100.0f;

    // ========== Height Field Parameters ==========

    // Base height range
    float min_height = 0.0f;
    float max_height = 50.0f;

    // Global height scale
    float height_scale = 1.0f;

    // ========== Noise Parameters - Base Terrain ==========

    // Primary noise octaves (more = more detail)
    int octaves = 6;

    // Frequency (higher = more compressed features)
    float frequency = 1.0f;

    // Persistence (how much each octave contributes)
    // Higher values = rougher terrain
    float persistence = 0.5f;

    // Lacunarity (frequency multiplier between octaves)
    // Higher values = more detail variation
    float lacunarity = 2.0f;

    // Noise type
    enum class NoiseType {
        Perlin,
        Simplex,
        Worley,  // Cellular/Voronoi
        Ridged,  // Ridge noise for mountains
        Billow   // Cloud-like formations
    } noise_type = NoiseType::Perlin;

    // ========== Multi-Scale Features ==========

    // Enable multi-scale generation
    bool enable_multi_scale = true;

    // Mountain layer parameters
    float mountain_scale = 0.5f;       // Scale in world units
    float mountain_amplitude = 30.0f;  // Height contribution
    float mountain_sharpness = 2.0f;   // Power function for peaks

    // Valley layer parameters
    float valley_scale = 1.0f;
    float valley_depth = 10.0f;
    float valley_width = 0.5f;

    // Hill layer parameters
    float hill_scale = 2.0f;
    float hill_amplitude = 5.0f;

    // Detail layer (small-scale features)
    float detail_scale = 10.0f;
    float detail_amplitude = 0.5f;

    // ========== Hydraulic Erosion Parameters ==========

    // Enable erosion simulation
    bool enable_erosion = true;

    // Number of erosion iterations
    int erosion_iterations = 50000;

    // Erosion strength (how much material is removed)
    float erosion_strength = 0.3f;

    // Deposition strength (how much sediment is deposited)
    float deposition_strength = 0.3f;

    // Evaporation rate (water loss per step)
    float evaporation_rate = 0.01f;

    // Minimum sediment capacity
    float min_sediment_capacity = 0.01f;

    // Water inertia (momentum)
    float water_inertia = 0.3f;

    // Gravity for water flow
    float gravity = 4.0f;

    // Initial water volume for each droplet
    float initial_water_volume = 1.0f;

    // Maximum droplet lifetime (steps)
    int max_droplet_lifetime = 30;

    // Erosion brush radius (in grid cells)
    int erosion_brush_radius = 3;

    // ========== Thermal Erosion Parameters ==========

    // Enable thermal erosion (slope-based material sliding)
    bool enable_thermal_erosion = true;

    // Thermal erosion iterations
    int thermal_iterations = 5;

    // Talus angle (maximum stable slope in radians)
    float talus_angle = 0.7f;  // ~40 degrees

    // Thermal erosion rate
    float thermal_strength = 0.5f;

    // ========== Biome Parameters ==========

    // Enable biome-based generation
    bool enable_biomes = false;

    // Temperature map parameters
    float temperature_frequency = 0.3f;
    float base_temperature = 15.0f;   // Base temperature in celsius
    float temperature_range = 30.0f;  // Temperature variation
    float altitude_temperature_factor =
        -6.5f;  // Temperature drop per km altitude

    // Moisture map parameters
    float moisture_frequency = 0.4f;
    float base_moisture = 0.5f;  // 0=dry, 1=wet
    float moisture_range = 0.5f;

    // Biome influence on terrain
    float biome_influence = 0.5f;

    // ========== Terracing/Plateaus ==========

    // Enable height terracing (creates stepped plateaus)
    bool enable_terracing = false;

    // Number of terrace levels
    int terrace_levels = 5;

    // Terrace smoothness (0=sharp steps, 1=smooth)
    float terrace_smoothness = 0.1f;

    // ========== Domain Warping ==========

    // Enable domain warping (distorts the noise space)
    bool enable_domain_warp = false;

    // Domain warp strength
    float domain_warp_strength = 0.5f;

    // Domain warp frequency
    float domain_warp_frequency = 0.5f;

    // ========== Post-processing ==========

    // Smooth factor (0=no smoothing, higher=more)
    float smoothing = 0.0f;

    // Height curve (power function applied to normalized height)
    // < 1.0 = flatter valleys, > 1.0 = sharper peaks
    float height_curve = 1.0f;

    // ========== Island Mode ==========

    // Create island (height decreases toward edges)
    bool island_mode = false;

    // Island falloff strength
    float island_falloff = 3.0f;

    // Island center offset
    glm::vec2 island_center = glm::vec2(0.5f, 0.5f);  // Normalized coordinates

    // ========== Simulation Parameters ==========

    // Random seed
    int random_seed = 42;

    // UV scale (for texture coordinate generation)
    float uv_scale = 1.0f;
};

// Biome types for terrain generation
enum class BiomeType {
    Ocean,
    Beach,
    Desert,
    Grassland,
    Forest,
    Rainforest,
    Savanna,
    Tundra,
    Taiga,
    Mountain,
    Snow
};

// Biome definition
struct Biome {
    BiomeType type;
    float min_temperature;
    float max_temperature;
    float min_moisture;
    float max_moisture;
    float min_altitude;
    float max_altitude;

    // Terrain modifiers for this biome
    float height_multiplier = 1.0f;
    float roughness_multiplier = 1.0f;
};

}  // namespace TerrainGen
