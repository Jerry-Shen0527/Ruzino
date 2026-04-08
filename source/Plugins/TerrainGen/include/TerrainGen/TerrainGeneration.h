#pragma once

#include <memory>

#include "TerrainParameters.h"
#include "TerrainStructure.h"
#include "api.h"

namespace TerrainGen {

// Main terrain generation class
class TERRAINGEN_API TerrainGenerator {
   public:
    TerrainGenerator();
    ~TerrainGenerator();

    // Generate terrain from parameters
    std::shared_ptr<TerrainStructure> generate(const TerrainParameters& params);

    // Individual generation steps (for incremental generation)
    void generate_base_heightmap(
        TerrainStructure& terrain,
        const TerrainParameters& params);
    void apply_multi_scale_features(
        TerrainStructure& terrain,
        const TerrainParameters& params);
    void apply_hydraulic_erosion(
        TerrainStructure& terrain,
        const TerrainParameters& params);
    void apply_thermal_erosion(
        TerrainStructure& terrain,
        const TerrainParameters& params);
    void generate_climate_maps(
        TerrainStructure& terrain,
        const TerrainParameters& params);
    void apply_biomes(
        TerrainStructure& terrain,
        const TerrainParameters& params);
    void apply_post_processing(
        TerrainStructure& terrain,
        const TerrainParameters& params);

   private:
    // Noise generation functions
    float perlin_noise(float x, float y, int seed) const;
    float simplex_noise(float x, float y, int seed) const;
    float worley_noise(float x, float y, int seed) const;
    float ridged_noise(float x, float y, int seed) const;
    float billow_noise(float x, float y, int seed) const;

    // Multi-octave noise (FBM - Fractional Brownian Motion)
    float fbm(
        float x,
        float y,
        int octaves,
        float frequency,
        float persistence,
        float lacunarity,
        int seed,
        TerrainParameters::NoiseType type) const;

    // Domain warping
    glm::vec2 domain_warp(
        float x,
        float y,
        float strength,
        float frequency,
        int seed) const;

    // Erosion helpers
    void simulate_hydraulic_erosion_droplet(
        HeightField& field,
        const TerrainParameters& params,
        int seed);
    void apply_thermal_erosion_step(
        HeightField& field,
        float talus_angle,
        float strength);

    // Biome classification
    BiomeType classify_biome(float temperature, float moisture, float altitude)
        const;

    // Utility functions
    float smoothstep(float edge0, float edge1, float x) const;
    float remap(
        float value,
        float in_min,
        float in_max,
        float out_min,
        float out_max) const;

    // Random number generator
    float random_float(int& seed) const;
    glm::vec2 random_vec2(int& seed) const;
};

}  // namespace TerrainGen
