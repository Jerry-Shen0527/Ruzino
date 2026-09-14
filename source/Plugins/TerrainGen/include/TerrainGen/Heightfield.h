#pragma once

#include <glm/glm.hpp>
#include <vector>

#include "TerrainGen/api.h"

namespace TerrainGen {

// A square heightfield with optional solver layers, the data carrier of the
// terrain node family.
//
// How it travels between nodes: the geometry-nodes layer packs this into a
// regular grid MESH (vertices lifted to the height, layers attached as named
// vertex scalar quantities "water" / "sediment" / "wear"), so every existing
// node — transform, merge, write_usd, the viewport — consumes it unchanged.
// The erode nodes unpack the mesh back into a Heightfield, solve, and pack a
// fresh mesh.
struct Heightfield {
    int res = 0;                // res x res vertices
    float world_size = 100.0f;  // XY extent in world units

    std::vector<float> height;    // world units
    std::vector<float> water;     // water depth (virtual-pipes state output)
    std::vector<float> sediment;  // suspended sediment (same)
    std::vector<float> wear;      // |cumulative erosion| — the rock/exposure
                                  // mask for downstream texturing
    bool has_water = false;
    bool has_sediment = false;
    bool has_wear = false;

    TERRAINGEN_API void alloc(int resolution, float size);

    [[nodiscard]] float cell_size() const
    {
        return world_size / static_cast<float>(res - 1);
    }

    float& at(int x, int y)
    {
        return height[static_cast<size_t>(y) * res + x];
    }
    [[nodiscard]] float at(int x, int y) const
    {
        return height[static_cast<size_t>(y) * res + x];
    }

    // Clamped bilinear sample in vertex coordinates ([0, res-1]).
    [[nodiscard]] TERRAINGEN_API float sample(float x, float y) const;

    // Central-difference normal in world space, Y-up.
    [[nodiscard]] TERRAINGEN_API glm::vec3 normal(int x, int y) const;

    [[nodiscard]] TERRAINGEN_API float min_height() const;
    [[nodiscard]] TERRAINGEN_API float max_height() const;
    [[nodiscard]] TERRAINGEN_API double mean_height() const;
};

}  // namespace TerrainGen
