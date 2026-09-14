#include "TerrainGen/NoiseCore.h"

#include <algorithm>
#include <cmath>

namespace TerrainGen {

namespace {

    // Map a 32-bit hash to [0, 1). Uses the top 24 bits so a float cast loses
    // nothing.
    float hash_to_unit(uint32_t h)
    {
        return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
    }

    glm::vec2 cell_feature_point(int cx, int cy, uint32_t seed)
    {
        const uint32_t h1 = NoiseCore::hash_u32(
            static_cast<uint32_t>(cx) * 0x8da6b343u ^
            static_cast<uint32_t>(cy) * 0xd8163841u ^ seed);
        const uint32_t h2 = NoiseCore::hash_u32(
            static_cast<uint32_t>(cx) * 0xcb1ab31fu ^
            static_cast<uint32_t>(cy) * 0x165667b1u ^ (seed ^ 0x68bc21ebu));
        return glm::vec2(hash_to_unit(h1), hash_to_unit(h2));
    }

    // 8 gradient directions; index comes from the low bits of the cell hash.
    float corner_contribution(
        float dx,
        float dy,
        int gi,
        int gj,
        uint32_t seed,
        float t)
    {
        if (t <= 0.0f)
            return 0.0f;
        const uint32_t h = NoiseCore::hash_u32(
            static_cast<uint32_t>(gi) * 0x8da6b343u ^
            static_cast<uint32_t>(gj) * 0xd8163841u ^ seed);
        static const float dirs[8][2] = {
            { 1.f, 0.f },          { -1.f, 0.f },
            { 0.f, 1.f },          { 0.f, -1.f },
            { 0.7071f, 0.7071f },  { -0.7071f, 0.7071f },
            { 0.7071f, -0.7071f }, { -0.7071f, -0.7071f },
        };
        const float* d = dirs[h & 7];
        const float t2 = t * t;
        return t2 * t2 * (d[0] * dx + d[1] * dy);
    }

}  // namespace

uint32_t NoiseCore::hash_u32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint32_t NoiseCore::octave_seed(uint32_t seed, int octave)
{
    return hash_u32(seed + static_cast<uint32_t>(octave) * 0x9e3779b9u);
}

float NoiseCore::noise2(float x, float y, uint32_t seed)
{
    // 2D simplex (Gustavson skewing), gradient lattice from coord_hash.
    const float f2 = 0.3660254037844386f;   // 0.5 * (sqrt(3) - 1)
    const float g2 = 0.21132486540518713f;  // (3 - sqrt(3)) / 6

    const float s = (x + y) * f2;
    const int i = static_cast<int>(std::floor(x + s));
    const int j = static_cast<int>(std::floor(y + s));
    const float t = static_cast<float>(i + j) * g2;

    const float x0 = x - (static_cast<float>(i) - t);
    const float y0 = y - (static_cast<float>(j) - t);

    const int i1 = (x0 > y0) ? 1 : 0;
    const int j1 = (x0 > y0) ? 0 : 1;

    const float x1 = x0 - static_cast<float>(i1) + g2;
    const float y1 = y0 - static_cast<float>(j1) + g2;
    const float x2 = x0 - 1.0f + 2.0f * g2;
    const float y2 = y0 - 1.0f + 2.0f * g2;

    const uint32_t s1 = static_cast<uint32_t>(i1);
    const uint32_t s2 = 1u;

    float n = 0.0f;
    n += corner_contribution(x0, y0, i, j, seed, 0.5f - x0 * x0 - y0 * y0);
    n += corner_contribution(
        x1,
        y1,
        i + static_cast<int>(s1),
        j + static_cast<int>(j1),
        seed,
        0.5f - x1 * x1 - y1 * y1);
    n += corner_contribution(
        x2, y2, i + 1, j + 1, seed, 0.5f - x2 * x2 - y2 * y2);

    // ~70 scales the [-1, 1] bounding of the kernel to full range.
    return 70.0f * n;
}

float NoiseCore::fbm(
    float x,
    float y,
    uint32_t seed,
    int octaves,
    float frequency,
    float persistence,
    float lacunarity)
{
    float sum = 0.0f;
    float amplitude = 1.0f;
    float norm = 0.0f;
    float freq = frequency;

    for (int i = 0; i < octaves; ++i) {
        sum += noise2(x * freq, y * freq, octave_seed(seed, i)) * amplitude;
        norm += amplitude;
        amplitude *= persistence;
        freq *= lacunarity;
    }
    return (norm > 0.0f) ? sum / norm : 0.0f;
}

float NoiseCore::ridged_multifractal(
    float x,
    float y,
    uint32_t seed,
    int octaves,
    float frequency,
    float persistence,
    float lacunarity)
{
    float sum = 0.0f;
    float amplitude = 1.0f;
    float norm = 0.0f;
    float freq = frequency;
    float weight = 1.0f;

    for (int i = 0; i < octaves; ++i) {
        float signal =
            1.0f - std::abs(noise2(x * freq, y * freq, octave_seed(seed, i)));
        signal *= signal;
        signal *= weight;
        weight = std::clamp(signal * 2.0f, 0.0f, 1.0f);

        sum += signal * amplitude;
        norm += amplitude;
        amplitude *= persistence;
        freq *= lacunarity;
    }
    return (norm > 0.0f) ? std::clamp(sum / norm, 0.0f, 1.0f) : 0.0f;
}

float NoiseCore::billow(
    float x,
    float y,
    uint32_t seed,
    int octaves,
    float frequency,
    float persistence,
    float lacunarity)
{
    float sum = 0.0f;
    float amplitude = 1.0f;
    float norm = 0.0f;
    float freq = frequency;

    for (int i = 0; i < octaves; ++i) {
        float signal =
            std::abs(noise2(x * freq, y * freq, octave_seed(seed, i)));
        signal = signal * 2.0f - 0.6f;  // recentre around ~[0, 1]
        sum += signal * amplitude;
        norm += amplitude;
        amplitude *= persistence;
        freq *= lacunarity;
    }
    return (norm > 0.0f) ? std::clamp(sum / norm, 0.0f, 1.0f) : 0.0f;
}

float NoiseCore::worley_f1(float x, float y, uint32_t seed)
{
    const int cx = static_cast<int>(std::floor(x));
    const int cy = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(cx);
    const float fy = y - static_cast<float>(cy);

    float best = 10.0f;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const glm::vec2 p = cell_feature_point(cx + dx, cy + dy, seed);
            const float ex = static_cast<float>(dx) + p.x - fx;
            const float ey = static_cast<float>(dy) + p.y - fy;
            best = std::min(best, std::sqrt(ex * ex + ey * ey));
        }
    }
    return std::clamp(best, 0.0f, 1.0f);
}

glm::vec2 NoiseCore::domain_warp(
    float x,
    float y,
    uint32_t seed,
    float frequency,
    float strength)
{
    // Two progressive passes: the second warps the output of the first,
    // which produces the flowing, gully-like distortion single-pass warp
    // cannot (iq, "Domain Warping").
    glm::vec2 p(x, y);

    for (int pass = 0; pass < 2; ++pass) {
        const uint32_t sx = octave_seed(seed, 40 + pass * 2);
        const uint32_t sy = octave_seed(seed, 41 + pass * 2);
        const float wx =
            fbm(p.x * frequency, p.y * frequency, sx, 3, 1.f, 0.5f, 2.f);
        const float wy =
            fbm(p.x * frequency + 31.416f,
                p.y * frequency + 47.853f,
                sy,
                3,
                1.f,
                0.5f,
                2.f);
        p += glm::vec2(wx, wy) * strength * (pass == 0 ? 1.0f : 0.5f);
    }
    return p;
}

}  // namespace TerrainGen
