#include "TerrainGen/Heightfield.h"

#include <algorithm>
#include <cmath>

namespace TerrainGen {

void Heightfield::alloc(int resolution, float size)
{
    res = resolution;
    world_size = size;
    const size_t n = static_cast<size_t>(resolution) * resolution;
    height.assign(n, 0.0f);
    water.clear();
    sediment.clear();
    wear.clear();
    has_water = has_sediment = has_wear = false;
}

float Heightfield::sample(float x, float y) const
{
    x = std::clamp(x, 0.0f, static_cast<float>(res - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(res - 1));

    const int x0 = std::min(static_cast<int>(x), res - 2);
    const int y0 = std::min(static_cast<int>(y), res - 2);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);

    const float h00 = at(x0, y0);
    const float h10 = at(x0 + 1, y0);
    const float h01 = at(x0, y0 + 1);
    const float h11 = at(x0 + 1, y0 + 1);

    return h00 * (1 - fx) * (1 - fy) + h10 * fx * (1 - fy) +
           h01 * (1 - fx) * fy + h11 * fx * fy;
}

glm::vec3 Heightfield::normal(int x, int y) const
{
    // Central differences on the clamped neighbourhood, scaled to world
    // spacing: n = normalize(-dh/dx, 2, -dh/dy) for a Y-up field.
    const int x0 = std::max(x - 1, 0);
    const int x1 = std::min(x + 1, res - 1);
    const int y0 = std::max(y - 1, 0);
    const int y1 = std::min(y + 1, res - 1);

    const float dx =
        (at(x1, y) - at(x0, y)) / (static_cast<float>(x1 - x0) * cell_size());
    const float dy =
        (at(x, y1) - at(x, y0)) / (static_cast<float>(y1 - y0) * cell_size());

    return glm::normalize(glm::vec3(-dx, 1.0f, -dy));
}

float Heightfield::min_height() const
{
    return *std::min_element(height.begin(), height.end());
}

float Heightfield::max_height() const
{
    return *std::max_element(height.begin(), height.end());
}

double Heightfield::mean_height() const
{
    double sum = 0.0;
    for (float h : height)
        sum += h;
    return height.empty() ? 0.0 : sum / static_cast<double>(height.size());
}

}  // namespace TerrainGen
