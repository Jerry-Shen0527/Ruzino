// terrain_scatter_points: scatter points ON the terrain surface for
// instancing (trees, rocks, grass ...).
//
// Stays generic and rides existing types only: it unpacks the input
// heightfield (dense grid or adaptive mesh — both via terrain_carry) and
// dart-throws candidates filtered by slope / height band / an acceptance
// window on a named vertex scalar quantity ("Mask Field", e.g. "biome"
// from terrain_texture_bake, or "water" to keep points off lakes). The
// output is a plain Points geometry whose fields carry everything a
// downstream instance_on_points needs:
//   - vertices : surface points (Y = terrain height, XZ inside the field)
//   - normals  : +Y upright, or the surface normal when Align to Normal
//   - width    : per-point scale factor (uniform in [Scale Min, Scale Max])
// width doubles as the instance scale (instance_on_points "Use Width as
// Scale"), so WHERE something grows and HOW BIG it is are both decided
// here, next to the terrain data.
//
// The minimum-distance rejection uses a uniform hash grid over accepted
// points, so "Count" is a target: dense-enough terrain reaches it, tight
// spacing or heavy filtering yields fewer (logged).

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "GCore/Components/MeshComponent.h"
#include "GCore/Components/PointsComponent.h"
#include "GCore/GOP.h"
#include "TerrainGen/Heightfield.h"
#include "geom_node_base.h"
#include "spdlog/spdlog.h"
#include "terrain_carry.hpp"

using namespace TerrainGen;

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(terrain_scatter_points)
{
    b.add_input<Geometry>("Height Field");
    b.add_input<int>("Count").min(1).max(100000).default_val(500);
    b.add_input<float>("Min Distance").min(0.0f).max(500.0f).default_val(8.0f);
    b.add_input<int>("Seed").min(0).max(1073741823).default_val(0);
    // Degrees from horizontal; steeper ground rejects the candidate.
    b.add_input<float>("Slope Max").min(0.0f).max(90.0f).default_val(40.0f);
    // Height band normalized to the field's [min, max] — same convention
    // as the bake node's snow/grass lines (0.65 keeps trees off the peaks).
    b.add_input<float>("Height Min").min(0.0f).max(1.0f).default_val(0.0f);
    b.add_input<float>("Height Max").min(0.0f).max(1.0f).default_val(1.0f);
    // Acceptance window on a named vertex scalar quantity riding the input
    // mesh. Dense grids sample it bilinearly; adaptive meshes have no grid
    // to interpolate on, so the mask is skipped there (with a warning).
    // Examples: Mask Field "biome", Min 2, Max 2 = grass only; Mask Field
    // "water", Max 0.01 = keep points out of lakes.
    b.add_input<std::string>("Mask Field").default_val("");
    b.add_input<float>("Mask Min").default_val(0.0f);
    b.add_input<float>("Mask Max").default_val(1000000.0f);
    // Instances stay upright (+Y) by default — trees grow against gravity,
    // not perpendicular to the slope. Align to Normal tilts them onto the
    // surface instead (useful for rocks, not for trees).
    b.add_input<bool>("Align to Normal").default_val(false);
    b.add_input<float>("Scale Min").min(0.05f).max(10.0f).default_val(0.8f);
    b.add_input<float>("Scale Max").min(0.05f).max(10.0f).default_val(1.25f);

    b.add_output<Geometry>("Points");
}

NODE_EXECUTION_FUNCTION(terrain_scatter_points)
{
    auto input = params.get_input<Geometry>("Height Field");

    // Dense grid first (exact bilinear sampling), adaptive rasterization
    // otherwise (height/slope still valid; the mask's size check below
    // rejects the heuristic adaptive resolution and disables it).
    Heightfield hf;
    std::string err;
    if (!terrain_carry::heightfield_from_mesh_auto(input, hf, err)) {
        params.set_error(("terrain_scatter_points: " + err).c_str());
        return false;
    }

    const int want = std::clamp(params.get_input<int>("Count"), 1, 100000);
    const float min_dist =
        std::max(params.get_input<float>("Min Distance"), 0.0f);
    const int seed = params.get_input<int>("Seed");
    const float slope_max =
        std::clamp(params.get_input<float>("Slope Max"), 0.0f, 90.0f);
    const float height_min =
        std::clamp(params.get_input<float>("Height Min"), 0.0f, 1.0f);
    const float height_max =
        std::clamp(params.get_input<float>("Height Max"), 0.0f, 1.0f);
    const std::string mask_name = params.get_input<std::string>("Mask Field");
    const float mask_min = params.get_input<float>("Mask Min");
    const float mask_max = params.get_input<float>("Mask Max");
    const bool align_normal = params.get_input<bool>("Align to Normal");
    float scale_min = params.get_input<float>("Scale Min");
    float scale_max = params.get_input<float>("Scale Max");
    if (scale_min > scale_max)
        std::swap(scale_min, scale_max);

    const int res = hf.res;
    const float half = hf.world_size * 0.5f;
    const float cell = hf.cell_size();
    const float h_min = hf.min_height();
    const float relief = std::max(hf.max_height() - h_min, 1e-6f);

    // Named-quantity mask, bilinear on the dense grid.
    const float* mask_q = nullptr;
    if (!mask_name.empty()) {
        auto mesh = input.get_component<MeshComponent>();
        const auto& q = mesh->get_vertex_scalar_quantity(mask_name);
        if (q.size() == static_cast<size_t>(res) * res) {
            mask_q = q.data();
        }
        else {
            spdlog::warn(
                "[terrain] scatter: mask field \"{}\" not found on the "
                "input mesh ({} values, need {} for a {}x{} grid) — "
                "masking disabled",
                mask_name,
                q.size(),
                static_cast<size_t>(res) * res,
                res,
                res);
        }
    }
    const auto mask_at = [&](float gx, float gy) {
        const int x0 = std::min(static_cast<int>(gx), res - 2);
        const int y0 = std::min(static_cast<int>(gy), res - 2);
        const float fx = gx - static_cast<float>(x0);
        const float fy = gy - static_cast<float>(y0);
        const float v00 = mask_q[static_cast<size_t>(y0) * res + x0];
        const float v10 = mask_q[static_cast<size_t>(y0) * res + x0 + 1];
        const float v01 = mask_q[static_cast<size_t>(y0 + 1) * res + x0];
        const float v11 = mask_q[static_cast<size_t>(y0 + 1) * res + x0 + 1];
        return v00 * (1 - fx) * (1 - fy) + v10 * fx * (1 - fy) +
               v01 * (1 - fx) * fy + v11 * fx * fy;
    };

    // World-space gradient one vertex step to each side (the bake node's
    // convention): returns the slope in degrees and the Y-up surface normal.
    const auto slope_at = [&](float gx, float gy, glm::vec3& nrm) {
        const float x_lo = std::max(gx - 1.0f, 0.0f);
        const float x_hi = std::min(gx + 1.0f, static_cast<float>(res - 1));
        const float y_lo = std::max(gy - 1.0f, 0.0f);
        const float y_hi = std::min(gy + 1.0f, static_cast<float>(res - 1));
        const float step_x = (x_hi - x_lo) * cell;
        const float step_y = (y_hi - y_lo) * cell;
        const float dhdx =
            (step_x > 0.0f)
                ? (hf.sample(x_hi, gy) - hf.sample(x_lo, gy)) / step_x
                : 0.0f;
        const float dhdz =
            (step_y > 0.0f)
                ? (hf.sample(gx, y_hi) - hf.sample(gx, y_lo)) / step_y
                : 0.0f;
        nrm = glm::normalize(glm::vec3(-dhdx, 1.0f, -dhdz));
        return std::atan(std::sqrt(dhdx * dhdx + dhdz * dhdz)) * 57.2957795f;
    };

    // Min-distance rejection over accepted points via a uniform hash grid
    // (cell edge = min distance, check the 3x3 neighborhood).
    const bool spaced = min_dist > 1e-4f && min_dist < hf.world_size;
    const float min_dist2 = min_dist * min_dist;
    std::unordered_map<long long, std::vector<glm::vec2>> buckets;
    const auto bucket_key = [&](float wx, float wz) {
        const long long bx = static_cast<long long>(std::floor(wx / min_dist));
        const long long bz = static_cast<long long>(std::floor(wz / min_dist));
        return bx * 12582917LL + bz;
    };
    const auto too_close = [&](float wx, float wz) {
        if (!spaced)
            return false;
        // Re-derive the bucket coords for the 3x3 walk.
        const long long bx = static_cast<long long>(std::floor(wx / min_dist));
        const long long bz = static_cast<long long>(std::floor(wz / min_dist));
        for (long long dz = -1; dz <= 1; ++dz) {
            for (long long dx = -1; dx <= 1; ++dx) {
                const auto it =
                    buckets.find((bx + dx) * 12582917LL + (bz + dz));
                if (it == buckets.end())
                    continue;
                for (const auto& p : it->second) {
                    const float ddx = p.x - wx, ddz = p.y - wz;
                    if (ddx * ddx + ddz * ddz < min_dist2)
                        return true;
                }
            }
        }
        return false;
    };

    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<float> widths;
    positions.reserve(want);
    normals.reserve(want);
    widths.reserve(want);

    std::mt19937 rng(static_cast<unsigned>(seed));
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);

    const long long max_attempts =
        1000LL * static_cast<long long>(want) + 100000LL;
    long long attempts = 0;
    int starve = 0;  // consecutive spacing rejections: area is packing out
    while (static_cast<int>(positions.size()) < want &&
           attempts < max_attempts && starve < 20000) {
        ++attempts;

        const float wx = -half + unit(rng) * hf.world_size;
        const float wz = -half + unit(rng) * hf.world_size;
        const float gx = (wx + half) / cell;
        const float gy = (wz + half) / cell;

        // Height band (normalized like the bake node's snow/grass lines).
        const float h = hf.sample(gx, gy);
        const float t = (h - h_min) / relief;
        if (t < height_min || t > height_max)
            continue;

        glm::vec3 nrm;
        if (slope_at(gx, gy, nrm) > slope_max)
            continue;

        if (mask_q) {
            const float v = mask_at(gx, gy);
            if (v < mask_min || v > mask_max)
                continue;
        }

        if (too_close(wx, wz)) {
            ++starve;
            continue;
        }
        starve = 0;

        positions.emplace_back(wx, h, wz);
        normals.push_back(align_normal ? nrm : glm::vec3(0.0f, 1.0f, 0.0f));
        widths.push_back(scale_min + unit(rng) * (scale_max - scale_min));

        if (spaced)
            buckets[bucket_key(wx, wz)].emplace_back(wx, wz);
    }

    if (static_cast<int>(positions.size()) < want) {
        spdlog::warn(
            "[terrain] scatter: placed {} of {} points ({} attempts) — "
            "spacing {:.2f} and the filters leave too little eligible area; "
            "lower Min Distance / open the filters, or accept the count",
            positions.size(),
            want,
            attempts,
            min_dist);
    }
    else {
        spdlog::info(
            "[terrain] scatter: {} points in {} attempts",
            positions.size(),
            attempts);
    }

    Geometry points_geom = Geometry::CreatePoints();
    auto points = points_geom.get_component<PointsComponent>();
    points->set_vertices(positions);
    points->set_normals(normals);
    points->set_width(widths);

    params.set_output("Points", points_geom);
    return true;
}

NODE_DEF_CLOSE_SCOPE
