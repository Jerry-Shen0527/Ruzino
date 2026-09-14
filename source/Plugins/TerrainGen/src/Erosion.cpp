#include "TerrainGen/Erosion.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

#include "TerrainGen/NoiseCore.h"

namespace TerrainGen {

namespace {

    // Split [0, rows) into contiguous per-thread chunks. Callers only read
    // previous-state buffers and write cells they own, so results are
    // bit-identical for any thread count.
    template<typename Fn>
    void parallel_rows(int rows, int thread_count, Fn&& fn)
    {
        if (thread_count <= 0)
            thread_count =
                static_cast<int>(std::thread::hardware_concurrency());
        thread_count = std::max(1, std::min(thread_count, rows));

        const int chunk = (rows + thread_count - 1) / thread_count;
        std::vector<std::thread> workers;
        workers.reserve(thread_count - 1);
        for (int t = 1; t < thread_count; ++t) {
            const int y0 = t * chunk;
            const int y1 = std::min(y0 + chunk, rows);
            if (y0 >= y1)
                break;
            workers.emplace_back([&fn, y0, y1] { fn(y0, y1); });
        }
        fn(0, std::min(chunk, rows));
        for (auto& w : workers)
            w.join();
    }

    float bilinear(const std::vector<float>& f, int res, float x, float y)
    {
        x = std::clamp(x, 0.0f, static_cast<float>(res - 1));
        y = std::clamp(y, 0.0f, static_cast<float>(res - 1));
        const int x0 = std::min(static_cast<int>(x), res - 2);
        const int y0 = std::min(static_cast<int>(y), res - 2);
        const float fx = x - static_cast<float>(x0);
        const float fy = y - static_cast<float>(y0);
        const size_t i00 = static_cast<size_t>(y0) * res + x0;
        return f[i00] * (1 - fx) * (1 - fy) + f[i00 + 1] * fx * (1 - fy) +
               f[i00 + res] * (1 - fx) * fy + f[i00 + res + 1] * fx * fy;
    }

}  // namespace

void hydraulic_droplets(Heightfield& field, const DropletErosionParams& params)
{
    const int res = field.res;
    if (res < 2)
        return;

    // Beyer's droplet parameters are dimensionless and assume heights around
    // [0, 1] (Houdini's HeightField tools do the same normalization). Solve
    // on the normalized field, then restore the original scale.
    const float h_min = field.min_height();
    const float h_max = field.max_height();
    const float scale = std::max(h_max - h_min, 1e-6f);
    for (float& h : field.height)
        h = (h - h_min) / scale;

    int thread_count = params.thread_count;
    if (thread_count <= 0)
        thread_count = static_cast<int>(std::thread::hardware_concurrency());
    thread_count = std::max(1, std::min(thread_count, params.droplet_count));

    // Each thread erodes its own copy over a droplet batch, then the deltas
    // are merged — the same trade-off as the standard GPU droplet
    // implementations (batches act on slightly stale heights), but fully
    // deterministic.
    const int base_batch = params.droplet_count / thread_count;
    const int remainder = params.droplet_count % thread_count;

    std::vector<std::vector<float>> local_height(thread_count);
    std::vector<std::vector<float>> local_wear(thread_count);
    std::vector<std::thread> workers(thread_count);

    const auto simulate_batch = [&](int t, int count) {
        local_height[t] = field.height;
        local_wear[t].assign(static_cast<size_t>(res) * res, 0.0f);
        std::vector<float>& h = local_height[t];
        std::vector<float>& wear = local_wear[t];

        for (int drop = 0; drop < count; ++drop) {
            // Hash-seeded per-droplet RNG (xorshift32).
            uint32_t rng = NoiseCore::hash_u32(
                params.seed + 0x9e3779b9u * static_cast<uint32_t>(drop + 1));
            auto next_uniform = [&rng]() {
                rng ^= rng << 13;
                rng ^= rng >> 17;
                rng ^= rng << 5;
                return static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
            };

            float pos_x = next_uniform() * static_cast<float>(res - 1);
            float pos_y = next_uniform() * static_cast<float>(res - 1);
            float dir_x = 0.0f, dir_y = 0.0f;
            float speed = 1.0f;
            float water = 1.0f;
            float sediment = 0.0f;

            for (int life = 0; life < params.max_lifetime; ++life) {
                const int nx0 = static_cast<int>(pos_x);
                const int ny0 = static_cast<int>(pos_y);
                if (nx0 < 0 || nx0 >= res - 1 || ny0 < 0 || ny0 >= res - 1)
                    break;

                const float fx = pos_x - static_cast<float>(nx0);
                const float fy = pos_y - static_cast<float>(ny0);

                const float h00 = h[static_cast<size_t>(ny0) * res + nx0];
                const float h10 = h[static_cast<size_t>(ny0) * res + nx0 + 1];
                const float h01 = h[static_cast<size_t>(ny0 + 1) * res + nx0];
                const float h11 =
                    h[static_cast<size_t>(ny0 + 1) * res + nx0 + 1];
                const float height_c = h00 * (1 - fx) * (1 - fy) +
                                       h10 * fx * (1 - fy) +
                                       h01 * (1 - fx) * fy + h11 * fx * fy;

                const float grad_x = (h10 - h00) * (1 - fy) + (h11 - h01) * fy;
                const float grad_y = (h01 - h00) * (1 - fx) + (h11 - h10) * fx;

                dir_x = dir_x * params.inertia - grad_x * (1 - params.inertia);
                dir_y = dir_y * params.inertia - grad_y * (1 - params.inertia);
                const float dir_len = std::sqrt(dir_x * dir_x + dir_y * dir_y);
                if (dir_len > 1e-6f) {
                    dir_x /= dir_len;
                    dir_y /= dir_len;
                }
                else {
                    // Nowhere to flow: drop what is held and stop.
                    h[static_cast<size_t>(ny0) * res + nx0] += sediment;
                    sediment = 0.0f;
                    break;
                }

                const float new_x = pos_x + dir_x;
                const float new_y = pos_y + dir_y;
                const float new_height = bilinear(h, res, new_x, new_y);
                const float delta_h = new_height - height_c;

                const float capacity = std::max(-delta_h, params.min_slope) *
                                       params.sediment_capacity * speed * water;

                if (sediment > capacity || delta_h > 0) {
                    const float amount =
                        (delta_h > 0)
                            ? std::min(delta_h, sediment)
                            : (sediment - capacity) * params.deposition_rate;
                    sediment -= amount;

                    const float quarter = amount * 0.25f;
                    h[static_cast<size_t>(ny0) * res + nx0] +=
                        quarter * (1 - fx) * (1 - fy);
                    h[static_cast<size_t>(ny0) * res + nx0 + 1] +=
                        quarter * fx * (1 - fy);
                    h[static_cast<size_t>(ny0 + 1) * res + nx0] +=
                        quarter * (1 - fx) * fy;
                    h[static_cast<size_t>(ny0 + 1) * res + nx0 + 1] +=
                        quarter * fx * fy;
                }
                else {
                    const float amount = std::min(
                        (capacity - sediment) * params.erosion_rate, -delta_h);
                    // Per-step erodibility cap: at most 1% of the field's
                    // relief per droplet step (mirrors the pipes solver's
                    // max_delta; keeps valley-bottom convergence from
                    // digging runaway slots).
                    static constexpr float MAX_STEP = 0.01f;
                    const float capped = std::min(amount, MAX_STEP);

                    // Normalize the brush kernel so the full amount leaves
                    // the surface (the legacy loop created/destroyed mass
                    // with raw weights).
                    float weight_sum = 0.0f;
                    const int r = params.brush_radius;
                    const float rf = static_cast<float>(r);
                    for (int by = -r; by <= r; ++by) {
                        for (int bx = -r; bx <= r; ++bx) {
                            const float dist = std::sqrt(
                                static_cast<float>(bx * bx + by * by));
                            if (dist <= rf)
                                weight_sum += 1.0f - dist / rf;
                        }
                    }

                    for (int by = -r; by <= r; ++by) {
                        for (int bx = -r; bx <= r; ++bx) {
                            const int ex = nx0 + bx;
                            const int ey = ny0 + by;
                            if (ex < 0 || ex >= res || ey < 0 || ey >= res)
                                continue;
                            const float dist = std::sqrt(
                                static_cast<float>(bx * bx + by * by));
                            if (dist > rf)
                                continue;
                            float weight = 1.0f - dist / rf;
                            if (weight_sum > 0.0f)
                                weight /= weight_sum;
                            const float eroded = capped * weight;
                            const size_t idx =
                                static_cast<size_t>(ey) * res + ex;
                            h[idx] -= eroded;
                            wear[idx] += eroded;
                            sediment += eroded;
                        }
                    }
                }

                // Guard against a negative argument under the sqrt when the
                // droplet falls further than speed^2/gravity in one step.
                speed = std::sqrt(
                    std::max(0.0f, speed * speed + delta_h * params.gravity));
                water *= (1.0f - params.evaporation);
                pos_x = new_x;
                pos_y = new_y;
            }
        }
    };

    for (int t = 0; t < thread_count; ++t) {
        const int count = base_batch + (t < remainder ? 1 : 0);
        workers[t] = std::thread(simulate_batch, t, count);
    }
    for (auto& w : workers)
        w.join();

    // Merge: field.height += Σ_t (local_t − base). Every local copy started
    // as a copy of the base, so this recovers base + Σ(deltas) without ever
    // aliasing the base during the sum.
    const size_t n = field.height.size();
    std::vector<float> merged(n, 0.0f);
    for (int t = 0; t < thread_count; ++t) {
        for (size_t i = 0; i < n; ++i)
            merged[i] += local_height[t][i];
    }
    for (size_t i = 0; i < n; ++i) {
        field.height[i] +=
            merged[i] - static_cast<float>(thread_count) * field.height[i];
    }

    field.wear.assign(n, 0.0f);
    for (int t = 0; t < thread_count; ++t) {
        for (size_t i = 0; i < field.wear.size(); ++i)
            field.wear[i] += local_wear[t][i];
    }
    // Restore the original height scale; wear/depths scale with it.
    for (size_t i = 0; i < n; ++i) {
        field.height[i] = field.height[i] * scale + h_min;
        field.wear[i] *= scale;
    }
    field.has_water = false;
    field.has_sediment = false;
    field.has_wear = true;
}

void thermal_erosion(Heightfield& field, const ThermalErosionParams& params)
{
    const int res = field.res;
    if (res < 3)
        return;
    const float talus =
        std::tan(params.talus_angle_deg * 3.14159265358979f / 180.0f) *
        field.cell_size();

    const size_t n = static_cast<size_t>(res) * res;
    std::vector<float> cur = field.height;
    std::vector<float> next(n, 0.0f);

    // Each cell's net change follows only from the OLD heights of its 3x3
    // neighbourhood: gains from higher neighbours sliding onto it minus its
    // own slides to lower neighbours. Pure read of `cur`, so rows are
    // race-free for any partitioning.
    for (int iter = 0; iter < params.iterations; ++iter) {
        parallel_rows(res, params.thread_count, [&](int y0, int y1) {
            for (int y = std::max(y0, 1); y < std::min(y1, res - 1); ++y) {
                for (int x = 1; x < res - 1; ++x) {
                    const size_t idx = static_cast<size_t>(y) * res + x;
                    const float center = cur[idx];

                    float change = 0.0f;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (dx == 0 && dy == 0)
                                continue;
                            const float nh =
                                cur[static_cast<size_t>(y + dy) * res + x + dx];
                            const float diff = nh - center;
                            const float signed_excess =
                                (diff > talus) ? (diff - talus)
                                               : (center - nh > talus
                                                      ? -(center - nh - talus)
                                                      : 0.0f);
                            change += signed_excess * params.strength * 0.125f;
                        }
                    }
                    next[idx] = center + change;
                }
            }
        });
        // Border rows/cols are clamped: carry them over unchanged.
        const int y0 = 0;
        for (int x = 0; x < res; ++x) {
            next[static_cast<size_t>(y0) * res + x] =
                cur[static_cast<size_t>(y0) * res + x];
            next[static_cast<size_t>(res - 1) * res + x] =
                cur[static_cast<size_t>(res - 1) * res + x];
        }
        for (int y = 0; y < res; ++y) {
            next[static_cast<size_t>(y) * res] =
                cur[static_cast<size_t>(y) * res];
            next[static_cast<size_t>(y) * res + res - 1] =
                cur[static_cast<size_t>(y) * res + res - 1];
        }
        cur.swap(next);
    }
    field.height = std::move(cur);
}

}  // namespace TerrainGen
