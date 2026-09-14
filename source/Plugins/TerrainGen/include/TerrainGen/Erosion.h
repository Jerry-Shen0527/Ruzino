#pragma once

#include <cstdint>

#include "TerrainGen/Heightfield.h"
#include "TerrainGen/api.h"

namespace TerrainGen {

// Particle (droplet) hydraulic erosion (Hans Theobald Beyer) — the CPU
// fallback of the terrain erode node for machines without an RHI device.
// The primary path is the virtual-pipes grid model on the GPU (slang
// compute, see geometry_nodes/TerrainGen/shaders +
// node_terrain_erode_hydraulic.cpp).
//
// Fixed relative to the legacy TerrainGenerator droplet loop: per-droplet
// hash-seeded RNG (the old path fed consecutive seeds into ONE mt19937
// stream), normalized erosion brush weights, and wear accumulation.
struct DropletErosionParams {
    int droplet_count = 60000;
    int max_lifetime = 30;
    float inertia = 0.05f;
    float sediment_capacity = 4.0f;
    float erosion_rate = 0.3f;
    float deposition_rate = 0.3f;
    float evaporation = 0.01f;
    float gravity = 4.0f;
    int brush_radius = 3;
    float min_slope = 0.01f;  // capacity floor in height units
    uint32_t seed = 0;
    int thread_count = 0;  // droplets are split into per-thread batches
};

TERRAINGEN_API void hydraulic_droplets(
    Heightfield& field,
    const DropletErosionParams& params);

// Slope-limited (talus) relaxation: material above the stable angle slides
// to its lower neighbours. Smooths rock faces into scree without touching
// the drainage network. CPU fallback; the erode node runs the same math as
// a GPU dispatch loop when a device is available.
struct ThermalErosionParams {
    int iterations = 30;
    float talus_angle_deg = 35.0f;
    float strength = 0.5f;
    int thread_count = 0;
};

TERRAINGEN_API void thermal_erosion(
    Heightfield& field,
    const ThermalErosionParams& params);

}  // namespace TerrainGen
