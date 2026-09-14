#pragma once

#include <cstdint>
#include <glm/glm.hpp>

#include "TerrainGen/api.h"

namespace TerrainGen {

// Integer-hash-seeded 2D gradient noise family.
//
// The legacy TerrainGenerator sampled glm::perlin at (p + seed * 0.1f), which
// correlates every octave of an fBm (they all live in one continuous noise
// field, offset by 0.1) and makes "new seed" a uniform translation of the
// whole field. Everything here instead derives each evaluation's gradient
// lattice from hash(cell, seed), so distinct seeds give fully decorrelated
// fields and fBm octaves are independent by construction.

class NoiseCore {
   public:
    // Simplex gradient noise, range ~[-1, 1].
    TERRAINGEN_API static float noise2(float x, float y, uint32_t seed);

    // Plain fractal Brownian motion, normalized to ~[-1, 1].
    TERRAINGEN_API static float fbm(
        float x,
        float y,
        uint32_t seed,
        int octaves,
        float frequency,
        float persistence,
        float lacunarity);

    // Musgue ridged multifractal, [0, 1]. Successive octaves are weighted by
    // the previous ridge height, which sharpens crests and fills valleys.
    TERRAINGEN_API static float ridged_multifractal(
        float x,
        float y,
        uint32_t seed,
        int octaves,
        float frequency,
        float persistence,
        float lacunarity);

    // |noise| doubled, [0, 1]. Rounded, cloud-like bumps.
    TERRAINGEN_API static float billow(
        float x,
        float y,
        uint32_t seed,
        int octaves,
        float frequency,
        float persistence,
        float lacunarity);

    // Worley (cellular) F1 distance, [0, 1]. Per-cell feature points come
    // from the integer hash (the legacy GLSL sin-hash port is unusable in
    // float32 on the CPU).
    TERRAINGEN_API static float worley_f1(float x, float y, uint32_t seed);

    // Two-pass progressive domain warp (iq): returns warped sample
    // coordinates for (x, y). Frequency is in the same units as the caller's
    // noise sampling; strength is in noise-space units.
    TERRAINGEN_API static glm::vec2 domain_warp(
        float x,
        float y,
        uint32_t seed,
        float frequency,
        float strength);

    // One seed per octave: hash-mixed so octaves cannot correlate through
    // the seed, unlike seed + i.
    TERRAINGEN_API static uint32_t octave_seed(uint32_t seed, int octave);

    // 32-bit finalizer used by all of the above.
    TERRAINGEN_API static uint32_t hash_u32(uint32_t x);
};

}  // namespace TerrainGen
