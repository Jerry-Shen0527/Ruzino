#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace TerrainGen {

// Height field structure
struct HeightField {
    int width = 0;
    int height = 0;
    std::vector<float> heights;  // Row-major storage

    // World space bounds
    float world_width = 100.0f;
    float world_height = 100.0f;
    float min_altitude = 0.0f;
    float max_altitude = 50.0f;

    // Constructor
    HeightField(int w, int h) : width(w), height(h)
    {
        heights.resize(w * h, 0.0f);
    }

    // Access height at (x, y)
    float& at(int x, int y)
    {
        return heights[y * width + x];
    }

    const float& at(int x, int y) const
    {
        return heights[y * width + x];
    }

    // Bilinear interpolation for smooth height lookup
    float sample(float x, float y) const
    {
        // Clamp to valid range
        x = glm::clamp(x, 0.0f, static_cast<float>(width - 1));
        y = glm::clamp(y, 0.0f, static_cast<float>(height - 1));

        int x0 = static_cast<int>(x);
        int y0 = static_cast<int>(y);
        int x1 = glm::min(x0 + 1, width - 1);
        int y1 = glm::min(y0 + 1, height - 1);

        float fx = x - x0;
        float fy = y - y0;

        float h00 = at(x0, y0);
        float h10 = at(x1, y0);
        float h01 = at(x0, y1);
        float h11 = at(x1, y1);

        float h0 = glm::mix(h00, h10, fx);
        float h1 = glm::mix(h01, h11, fx);

        return glm::mix(h0, h1, fy);
    }

    // Get normal at grid position
    glm::vec3 get_normal(int x, int y) const
    {
        float cell_size_x = world_width / (width - 1);
        float cell_size_y = world_height / (height - 1);

        // Get heights of neighbors
        float h_left = (x > 0) ? at(x - 1, y) : at(x, y);
        float h_right = (x < width - 1) ? at(x + 1, y) : at(x, y);
        float h_down = (y > 0) ? at(x, y - 1) : at(x, y);
        float h_up = (y < height - 1) ? at(x, y + 1) : at(x, y);

        // Calculate tangent vectors
        glm::vec3 tangent_x(cell_size_x * 2.0f, h_right - h_left, 0.0f);
        glm::vec3 tangent_y(0.0f, h_up - h_down, cell_size_y * 2.0f);

        // Normal is cross product
        glm::vec3 normal = glm::normalize(glm::cross(tangent_y, tangent_x));
        return normal;
    }

    // Get slope at position (in radians)
    float get_slope(int x, int y) const
    {
        glm::vec3 normal = get_normal(x, y);
        return glm::acos(glm::clamp(normal.y, 0.0f, 1.0f));
    }
};

// Water/sediment map for erosion simulation
struct WaterMap {
    int width = 0;
    int height = 0;
    std::vector<float> water;     // Water amount at each point
    std::vector<float> sediment;  // Sediment amount at each point
    std::vector<glm::vec2> flow;  // Flow velocity at each point

    WaterMap(int w, int h) : width(w), height(h)
    {
        water.resize(w * h, 0.0f);
        sediment.resize(w * h, 0.0f);
        flow.resize(w * h, glm::vec2(0.0f));
    }

    float& water_at(int x, int y)
    {
        return water[y * width + x];
    }

    float& sediment_at(int x, int y)
    {
        return sediment[y * width + x];
    }

    glm::vec2& flow_at(int x, int y)
    {
        return flow[y * width + x];
    }
};

// Temperature/moisture map for biome generation
struct ClimateMap {
    int width = 0;
    int height = 0;
    std::vector<float> temperature;  // Temperature in celsius
    std::vector<float> moisture;     // Moisture 0-1

    ClimateMap(int w, int h) : width(w), height(h)
    {
        temperature.resize(w * h, 15.0f);
        moisture.resize(w * h, 0.5f);
    }

    float& temp_at(int x, int y)
    {
        return temperature[y * width + x];
    }

    float& moisture_at(int x, int y)
    {
        return moisture[y * width + x];
    }
};

// Complete terrain structure
struct TerrainStructure {
    std::shared_ptr<HeightField> height_field;
    std::shared_ptr<WaterMap> water_map;
    std::shared_ptr<ClimateMap> climate_map;

    // Statistics
    float min_height = 0.0f;
    float max_height = 0.0f;
    float avg_height = 0.0f;

    // Generation metadata
    int generation_seed = 0;
    int erosion_iterations_done = 0;
    bool has_erosion = false;
    bool has_biomes = false;

    // Constructor
    TerrainStructure(int resolution, float world_size)
    {
        height_field = std::make_shared<HeightField>(resolution, resolution);
        height_field->world_width = world_size;
        height_field->world_height = world_size;

        water_map = std::make_shared<WaterMap>(resolution, resolution);
        climate_map = std::make_shared<ClimateMap>(resolution, resolution);
    }

    // Update statistics
    void update_statistics()
    {
        if (!height_field || height_field->heights.empty())
            return;

        min_height = height_field->heights[0];
        max_height = height_field->heights[0];
        float sum = 0.0f;

        for (float h : height_field->heights) {
            min_height = glm::min(min_height, h);
            max_height = glm::max(max_height, h);
            sum += h;
        }

        avg_height = sum / height_field->heights.size();
    }
};

}  // namespace TerrainGen
