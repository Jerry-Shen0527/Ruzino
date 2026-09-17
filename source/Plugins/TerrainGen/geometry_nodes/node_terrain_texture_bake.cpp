// terrain_texture_bake: bakes the heightfield's layers (height, slope, water,
// sediment, wear) into a terrain albedo texture AND a tangent-space normal
// map PNG, ready for the create_material node (diffuseColor + normal inputs).
//
// Albedo: rule-based biome model over the vertex fields —
//   rock      stratified (height-band brightness + grey/tan fbm tint) on
//             slopes beyond "Rock Slope Angle"
//   grass     lowlands plus fbm patches climbing moderate slopes (a flat
//             single-tone rock base is what made earlier bakes read as mud)
//   snow      above a noise-dithered "Snow Line", gated to slopes below
//             ~50 deg (an undithered line turned every ridgeline into a
//             white vein)
//   sediment  sandy tint along the wear map (cumulative erosion), remapped
//             by a 97th-percentile reference so channel strength does not
//             depend on the single hottest cell
//   wetness   the water field darkens channel bottoms
//   valley AO height-concavity darkening breaks the flat look
//
// Normal map: tangent-space (OpenGL +Y-up convention, matching the Usd
// PreviewSurface normal input) from the heightfield gradient, PLUS optional
// detail octaves — procedural micro-relief evaluated at the texture's
// Nyquist, above the mesh resolution (off by default; see below).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "GCore/Components/MeshComponent.h"
#include "GCore/GOP.h"
#include "TerrainGen/Heightfield.h"
#include "TerrainGen/NoiseCore.h"
#include "geom_node_base.h"
#include "stb_image_write.h"
#include "terrain_carry.hpp"

using namespace TerrainGen;

namespace {

float smoothstep(float e0, float e1, float x)
{
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

std::string resolve_exe_relative(const std::string& path)
{
    std::filesystem::path p(path);
    if (p.is_absolute())
        return p.lexically_normal().string();
#ifdef _WIN32
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    p = std::filesystem::path(exe).parent_path() / p;
#else
    char exe[PATH_MAX];
    const ssize_t len = readlink("/proc/self/exe", exe, PATH_MAX);
    if (len != -1) {
        p = std::filesystem::path(std::string(exe, len)).parent_path() / p;
    }
#endif
    return p.lexically_normal().string();
}

// Percentile of a strided subsample — a max-referenced normalization puts
// the whole dynamic range into the few hottest cells (wear is heavy-tailed
// after long erosion runs).
float field_percentile(const std::vector<float>& field, int res, float fraction)
{
    if (field.empty())
        return 0.0f;
    std::vector<float> sample;
    const int stride = std::max(1, res / 257);
    for (int y = 0; y < res; y += stride)
        for (int x = 0; x < res; x += stride)
            sample.push_back(field[static_cast<size_t>(y) * res + x]);
    std::sort(sample.begin(), sample.end());
    const size_t idx = std::min(
        sample.size() - 1,
        static_cast<size_t>(fraction * static_cast<float>(sample.size())));
    return std::max(sample[idx], 1e-6f);
}

}  // namespace

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(terrain_texture_bake)
{
    b.add_input<Geometry>("Height Field");

    b.add_input<int>("Texture Resolution").min(64).max(16384).default_val(1024);
    // Slope beyond this angle (degrees) reads as bare rock.
    b.add_input<float>("Rock Slope Angle")
        .min(0.0f)
        .max(90.0f)
        .default_val(35.0f);
    // Fractions of the height relief.
    b.add_input<float>("Snow Line").min(0.0f).max(1.0f).default_val(0.65f);
    b.add_input<float>("Grass Line").min(0.0f).max(1.0f).default_val(0.35f);
    // How strongly the wear map drives the sandy sediment color.
    b.add_input<float>("Sediment Strength")
        .min(0.0f)
        .max(1.0f)
        .default_val(0.6f);

    // Normal-map shaping: strength exaggerates the slope shading; the detail
    // octaves are procedural micro-relief evaluated per texel (above the
    // mesh Nyquist), so shading carries detail the mesh cannot.
    b.add_input<float>("Normal Strength").min(0.0f).max(4.0f).default_val(1.0f);
    // Default OFF: detail octaves only make sense well below the MESH
    // Nyquist; at any useful texture scale they double-count slopes the
    // mesh already carries and turn the shading into speckle (verified
    // 2026-09-07: even a wavelength of ~12 mesh cells destroyed the render).
    b.add_input<float>("Detail Strength").min(0.0f).max(2.0f).default_val(0.0f);
    b.add_input<float>("Detail Frequency")
        .min(0.25f)
        .max(4.0f)
        .default_val(1.0f);

    // Relative paths resolve against the executable directory, matching
    // set_material's convention.
    b.add_input<std::string>("Output Path")
        .default_val("test_output/terrain_albedo.png");
    b.add_input<std::string>("Normal Output Path")
        .default_val("test_output/terrain_normal.png");

    b.add_output<Geometry>("Height Field");
    b.add_output<std::string>("Texture Path");
    b.add_output<std::string>("Normal Path");
}

NODE_EXECUTION_FUNCTION(terrain_texture_bake)
{
    if (!params.has_input("Height Field")) {
        spdlog::warn("terrain_texture_bake: no Height Field input");
        return false;
    }
    Geometry input = params.get_input<Geometry>("Height Field");

    Heightfield hf;
    std::string err;
    // Accepts both the dense grid (erode output) and the adaptive mesh
    // (terrain_adaptive_mesh output, triangle rasterization).
    if (!terrain_carry::heightfield_from_mesh_auto(input, hf, err)) {
        spdlog::warn("terrain_texture_bake: {}", err);
        params.set_output("Height Field", input);
        params.set_output("Texture Path", std::string());
        params.set_output("Normal Path", std::string());
        return true;
    }

    const int tex_res = params.get_input<int>("Texture Resolution");
    const float rock_slope_deg = params.get_input<float>("Rock Slope Angle");
    const float snow_line = params.get_input<float>("Snow Line");
    const float grass_line = params.get_input<float>("Grass Line");
    const float sediment_strength =
        params.get_input<float>("Sediment Strength");
    const float normal_strength = params.get_input<float>("Normal Strength");
    const float detail_strength = params.get_input<float>("Detail Strength");
    const float detail_freq = params.get_input<float>("Detail Frequency");
    const std::string out_path =
        resolve_exe_relative(params.get_input<std::string>("Output Path"));
    const std::string normal_path = resolve_exe_relative(
        params.get_input<std::string>("Normal Output Path"));

    const int res = hf.res;
    const float h_min = hf.min_height();
    const float h_max = std::max(hf.max_height(), h_min + 1e-6f);
    const float relief = h_max - h_min;

    const float wear_ref =
        hf.has_wear ? field_percentile(hf.wear, res, 0.97f) : 1e-6f;
    const float water_ref =
        hf.has_water ? field_percentile(hf.water, res, 0.98f) : 1e-6f;

    // Detail-octave amplitude: detail_strength * 0.1% of the world size is
    // visible micro-relief without looking like noise.
    const float detail_amp = detail_strength * hf.world_size * 0.001f;
    // Base detail wavelength ~24 texels, 2 octaves (top ~11 texels): the
    // 1-texel central difference then spans <0.2 cycles of the top octave
    // and stays a valid gradient estimate. Fixed absolute frequencies
    // alias at other resolutions (the per-texel noise of the first spike).
    const float detail_base_freq =
        static_cast<float>(tex_res) / 24.0f * detail_freq;
    const uint32_t detail_seed = 0x1c3f5a9du;

    std::vector<unsigned char> albedo_pixels(
        static_cast<size_t>(tex_res) * tex_res * 3, 0);
    std::vector<unsigned char> normal_pixels(
        static_cast<size_t>(tex_res) * tex_res * 3, 0);

    // Biome palette. Rock splits into grey / tan variants mixed by fbm,
    // slope, and stratification bands; grass comes in lush and dry.
    const float grass_lush[3] = { 0.20f, 0.33f, 0.10f };
    const float grass_dry[3] = { 0.44f, 0.42f, 0.19f };
    const float rock_grey[3] = { 0.36f, 0.35f, 0.335f };
    const float rock_tan[3] = { 0.56f, 0.46f, 0.34f };
    const float snow_col[3] = { 0.88f, 0.91f, 0.95f };
    const float sediment_col[3] = { 0.58f, 0.51f, 0.37f };

    const float rock_lo = rock_slope_deg - 6.0f;
    const float rock_hi = rock_slope_deg + 6.0f;

    // Coarse height pyramid (128x128 box-filtered): biome REGIONS must come
    // from a low-passed field. The eroded heightfield's dense gully network
    // otherwise stripes every mask into marble; the fine detail belongs in
    // the normal map, not the albedo regions.
    const int coarse_n = 128;
    const int block = std::max(1, res / coarse_n);
    std::vector<float> coarse_h(static_cast<size_t>(coarse_n) * coarse_n, 0.0f);
    for (int cy = 0; cy < coarse_n; ++cy) {
        for (int cx = 0; cx < coarse_n; ++cx) {
            const int x0 = cx * block;
            const int y0 = cy * block;
            const int x1 = std::min(x0 + block, res);
            const int y1 = std::min(y0 + block, res);
            float acc = 0.0f;
            int n = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    acc += hf.height[static_cast<size_t>(y) * res + x];
                    ++n;
                }
            }
            coarse_h[static_cast<size_t>(cy) * coarse_n + cx] =
                (n > 0) ? acc / static_cast<float>(n) : 0.0f;
        }
    }
    const float coarse_du = 1.0f / static_cast<float>(coarse_n - 1);
    const auto sample_coarse = [&](float u, float v) {
        const float gx =
            std::clamp(u, 0.0f, 1.0f) * static_cast<float>(coarse_n - 1);
        const float gy =
            std::clamp(v, 0.0f, 1.0f) * static_cast<float>(coarse_n - 1);
        const int x0 = std::min(static_cast<int>(gx), coarse_n - 2);
        const int y0 = std::min(static_cast<int>(gy), coarse_n - 2);
        const float fx = gx - static_cast<float>(x0);
        const float fy = gy - static_cast<float>(y0);
        const size_t i00 = static_cast<size_t>(y0) * coarse_n + x0;
        const float h00 = coarse_h[i00];
        const float h10 = coarse_h[i00 + 1];
        const float h01 = coarse_h[i00 + coarse_n];
        const float h11 = coarse_h[i00 + coarse_n + 1];
        return h00 * (1 - fx) * (1 - fy) + h10 * fx * (1 - fy) +
               h01 * (1 - fx) * fy + h11 * fx * fy;
    };

    // Region-scale biome classification, shared by the texture texels and
    // the per-vertex "biome" scatter quantity — ONE truth for where things
    // grow, so the two consumers cannot drift apart. sed_w is split into
    // biome_sed_w because the texel loop computes wear only after the
    // normal encode.
    struct BiomeRegion {
        float region_t;   // coarse height normalized to [0, 1]
        float slope_deg;  // coarse (low-passed) slope in degrees
        float dither;     // snow-line dither noise
        float snow_w;
        float rock_w;
        float patch;  // meadow patch noise (also modulates grass tint)
        float grass_w;
    };
    const auto biome_region = [&](float u, float v) {
        BiomeRegion b;
        const float region_h = sample_coarse(u, v);
        b.region_t = std::clamp((region_h - h_min) / relief, 0.0f, 1.0f);
        const float rh_x1 = sample_coarse(u + coarse_du, v);
        const float rh_x0 = sample_coarse(u - coarse_du, v);
        const float rh_y1 = sample_coarse(u, v + coarse_du);
        const float rh_y0 = sample_coarse(u, v - coarse_du);
        const float rgx = (rh_x1 - rh_x0) / (2.0f * coarse_du * hf.world_size);
        const float rgy = (rh_y1 - rh_y0) / (2.0f * coarse_du * hf.world_size);
        b.slope_deg = std::atan(std::sqrt(rgx * rgx + rgy * rgy)) * 57.29578f;

        b.dither =
            NoiseCore::fbm(u * 48.0f, v * 48.0f, 0x51ed, 3, 1.0f, 0.5f, 2.2f);
        const float snow_line_local = snow_line + 0.16f * (b.dither - 0.5f);
        b.snow_w =
            smoothstep(snow_line_local, snow_line_local + 0.06f, b.region_t) *
            (1.0f - smoothstep(30.0f, 55.0f, b.slope_deg));

        b.rock_w = smoothstep(rock_lo, rock_hi, b.slope_deg);

        // Grass: fbm meadow patches, favored in lowlands, that still climb
        // moderate rock (alpine turf on scree slopes). Low-octave patch
        // noise: meadows must be LARGE; the 4-octave version scattered
        // 1-texel grass speckle.
        b.patch =
            NoiseCore::fbm(u * 10.0f, v * 10.0f, 0x2f9d, 2, 1.0f, 0.5f, 2.0f);
        const float altitude_grass =
            1.0f - smoothstep(grass_line, grass_line + 0.35f, b.region_t);
        b.grass_w = std::clamp(
            b.patch * 0.65f + altitude_grass * 0.55f - 0.15f, 0.0f, 1.0f);
        b.grass_w *= (1.0f - 0.55f * b.rock_w) * (1.0f - b.snow_w);
        return b;
    };
    // Sediment: sandy deposits along the STRONGEST eroded channels only
    // (the wear field is near-uniform after long runs -- an unthresholded
    // wash painted 2/3 of the map).
    const auto biome_sed_w = [&](float wear_n, float snow_w, float grass_w) {
        return smoothstep(0.55f, 0.95f, std::pow(wear_n, 1.5f)) *
               sediment_strength * (1.0f - snow_w) * (1.0f - grass_w * 0.6f);
    };

    // Dominant-biome pixel counters (diagnostics for parameter tuning).
    std::atomic<int> rock_px{ 0 }, grass_px{ 0 }, snow_px{ 0 }, sed_px{ 0 },
        gentle_px{ 0 };

    const auto bake_rows = [&](int ty0, int ty1) {
        for (int ty = ty0; ty < ty1; ++ty) {
            // PNG rows are written GL-style (row 0 = field v = 1): the
            // renderer's texture load is Hio flipped=true, i.e. v=0
            // samples the LAST file row. Writing v top-down instead
            // mirrored both maps on the mesh (snow in valleys,
            // 2026-09-07 UV alignment probe).
            const float v = 1.0f - (static_cast<float>(ty) + 0.5f) /
                                       static_cast<float>(tex_res);
            for (int tx = 0; tx < tex_res; ++tx) {
                const float u = (static_cast<float>(tx) + 0.5f) /
                                static_cast<float>(tex_res);

                // Bilinear sample of the vertex fields.
                const float gx = u * static_cast<float>(res - 1);
                const float gy = v * static_cast<float>(res - 1);
                const float h = hf.sample(gx, gy);

                // Slope from the height gradient (world units), one vertex
                // step to each side of the sample.
                const float e = 1.0f;
                const float x_lo = std::max(gx - e, 0.0f);
                const float x_hi =
                    std::min(gx + e, static_cast<float>(res - 1));
                const float y_lo = std::max(gy - e, 0.0f);
                const float y_hi =
                    std::min(gy + e, static_cast<float>(res - 1));
                const float hx1 = hf.sample(x_hi, gy);
                const float hx0 = hf.sample(x_lo, gy);
                const float hy1 = hf.sample(gx, y_hi);
                const float hy0 = hf.sample(gx, y_lo);
                const float step_x = (x_hi - x_lo) * hf.cell_size();
                const float step_y = (y_hi - y_lo) * hf.cell_size();
                const float gx_w =
                    (step_x > 0.0f) ? (hx1 - hx0) / step_x : 0.0f;
                const float gy_w =
                    (step_y > 0.0f) ? (hy1 - hy0) / step_y : 0.0f;

                // Valley AO: mean height of a 3x3 neighborhood at ~2.5 cell
                // radius; being below the neighborhood mean reads as
                // concavity and gets darkened.
                float nb_mean = h;
                {
                    const float r = 2.5f;
                    float acc = 0.0f;
                    int n = 0;
                    for (int oy = -1; oy <= 1; ++oy) {
                        for (int ox = -1; ox <= 1; ++ox) {
                            if (ox == 0 && oy == 0)
                                continue;
                            acc += hf.sample(
                                std::clamp(
                                    gx + ox * r,
                                    0.0f,
                                    static_cast<float>(res - 1)),
                                std::clamp(
                                    gy + oy * r,
                                    0.0f,
                                    static_cast<float>(res - 1)));
                            ++n;
                        }
                    }
                    nb_mean = acc / static_cast<float>(n);
                }
                const float occ = std::clamp(
                    (nb_mean - h) / std::max(relief, 1e-6f) * 6.0f, 0.0f, 1.0f);

                // Region (low-passed) fields drive biome REGIONS; fine
                // slope/height only modulate within them (classification
                // shared with the per-vertex biome quantity below). Snow is
                // needed before the normal encode: a smooth snowfield must
                // not shade with gully detail (renders as crumpled foil).
                const BiomeRegion bio = biome_region(u, v);

                // Detail octaves: procedural micro-relief gradient at the
                // texture's scale, finite-differenced one texel apart.
                float dgx = 0.0f, dgy = 0.0f;
                if (detail_strength > 0.0f) {
                    const float du = 1.0f / static_cast<float>(tex_res);
                    const float fx = u * detail_base_freq;
                    const float fy = v * detail_base_freq;
                    const float dx1 = NoiseCore::fbm(
                        fx + du * detail_base_freq,
                        fy,
                        detail_seed,
                        2,
                        1.0f,
                        0.5f,
                        2.2f);
                    const float dx0 = NoiseCore::fbm(
                        fx - du * detail_base_freq,
                        fy,
                        detail_seed,
                        2,
                        1.0f,
                        0.5f,
                        2.2f);
                    const float dy1 = NoiseCore::fbm(
                        fx,
                        fy + du * detail_base_freq,
                        detail_seed,
                        2,
                        1.0f,
                        0.5f,
                        2.2f);
                    const float dy0 = NoiseCore::fbm(
                        fx,
                        fy - du * detail_base_freq,
                        detail_seed,
                        2,
                        1.0f,
                        0.5f,
                        2.2f);
                    const float dworld = 2.0f * du * hf.world_size;
                    if (dworld > 0.0f) {
                        dgx = (dx1 - dx0) * detail_amp / dworld;
                        dgy = (dy1 - dy0) * detail_amp / dworld;
                    }
                }

                // Tangent-space normal (T = +u = world +X, B = +v = world
                // +Z, N = world +Y). OpenGL +Y-up encoding for
                // UsdPreviewSurface. Clamp: this terrain's fine-scale
                // gradients reach 2-4, and unclamped strength-scaled
                // components saturate the [-1,1] encode into noise (the
                // all-black spike of 2026-09-07). Strength is damped under
                // snow: snow smooths and buries the fine gully relief.
                const float ns_local =
                    normal_strength * (1.0f - 0.75f * bio.snow_w);
                const float n_x =
                    std::clamp(-gx_w * ns_local - dgx, -2.5f, 2.5f);
                const float n_y =
                    std::clamp(-gy_w * ns_local - dgy, -2.5f, 2.5f);
                const float inv_len =
                    1.0f / std::sqrt(n_x * n_x + n_y * n_y + 1.0f);

                const float t_height = (h - h_min) / relief;

                // Field remaps.
                const auto sample_field = [&](const std::vector<float>& f) {
                    const int x0 = std::min(static_cast<int>(gx), res - 2);
                    const int y0 = std::min(static_cast<int>(gy), res - 2);
                    const float fx = gx - static_cast<float>(x0);
                    const float fy = gy - static_cast<float>(y0);
                    const float v00 = f[static_cast<size_t>(y0) * res + x0];
                    const float v10 = f[static_cast<size_t>(y0) * res + x0 + 1];
                    const float v01 = f[static_cast<size_t>(y0 + 1) * res + x0];
                    const float v11 =
                        f[static_cast<size_t>(y0 + 1) * res + x0 + 1];
                    return v00 * (1 - fx) * (1 - fy) + v10 * fx * (1 - fy) +
                           v01 * (1 - fx) * fy + v11 * fx * fy;
                };
                const float wear =
                    hf.has_wear
                        ? std::clamp(
                              sample_field(hf.wear) / wear_ref, 0.0f, 1.0f)
                        : 0.0f;
                const float wet =
                    hf.has_water
                        ? std::clamp(
                              sample_field(hf.water) / water_ref, 0.0f, 1.0f)
                        : 0.0f;

                // Biome weights (region fields via bio; fine slope + tint
                // stay local to the color blend).
                const float slope_deg =
                    std::atan(std::sqrt(gx_w * gx_w + gy_w * gy_w)) * 57.29578f;
                const float tint = NoiseCore::fbm(
                    u * 5.5f, v * 5.5f, 0x9e37, 4, 1.0f, 0.5f, 2.0f);
                const float sed_w = biome_sed_w(wear, bio.snow_w, bio.grass_w);

                float color[3];
                for (int c = 0; c < 3; ++c) {
                    // Stratified rock: dense height bands warped by fbm.
                    // Steepest faces expose fresh grey rock; moderate
                    // slopes lean warm tan (weathered soil).
                    const float fresh = smoothstep(
                        rock_slope_deg, rock_slope_deg + 25.0f, slope_deg);
                    const float band = std::sin(
                        (bio.region_t * 18.0f + 4.0f * tint) * 3.14159f);
                    const float mix_grey =
                        std::clamp(0.5f * tint + fresh, 0.0f, 1.0f);
                    float rock = rock_tan[c] * (1.0f - mix_grey) +
                                 rock_grey[c] * mix_grey;
                    rock *= 1.0f + 0.16f * band;

                    // Grass: lush base, dry upslope, fbm value variation.
                    float g = grass_lush[c] * (1.0f - bio.region_t * 0.5f) +
                              grass_dry[c] * bio.region_t * 0.5f;
                    g *= 0.9f + 0.25f * bio.patch;

                    float out = rock * (1.0f - bio.grass_w) + g * bio.grass_w;
                    out = out * (1.0f - sed_w) + sediment_col[c] * sed_w;
                    // Wetness darkens only STRONG channel water (residual
                    // film after drainage would otherwise dim the whole
                    // map).
                    out *= 1.0f - 0.28f * smoothstep(0.45f, 1.0f, wet) *
                                      (1.0f - bio.rock_w) * (1.0f - bio.snow_w);
                    // Valley AO.
                    out *= 1.0f - 0.20f * occ;
                    out = out * (1.0f - bio.snow_w) + snow_col[c] * bio.snow_w;
                    // Fine dither breaks banding.
                    out *= 1.0f + 0.05f * bio.dither;
                    color[c] = std::clamp(out, 0.0f, 1.0f);
                }

                // Dominant-biome tally for parameter tuning.
                if (bio.snow_w > 0.5f)
                    snow_px.fetch_add(1);
                else if (bio.rock_w > 0.5f)
                    rock_px.fetch_add(1);
                else if (bio.grass_w > 0.4f)
                    grass_px.fetch_add(1);
                if (sed_w > 0.25f)
                    sed_px.fetch_add(1);
                if (slope_deg < 25.0f)
                    gentle_px.fetch_add(1);

                const size_t idx = (static_cast<size_t>(ty) * tex_res + tx) * 3;
                albedo_pixels[idx + 0] =
                    static_cast<unsigned char>(color[0] * 255.0f + 0.5f);
                albedo_pixels[idx + 1] =
                    static_cast<unsigned char>(color[1] * 255.0f + 0.5f);
                albedo_pixels[idx + 2] =
                    static_cast<unsigned char>(color[2] * 255.0f + 0.5f);

                normal_pixels[idx + 0] = static_cast<unsigned char>(
                    std::clamp(n_x * inv_len * 0.5f + 0.5f, 0.0f, 1.0f) *
                        255.0f +
                    0.5f);
                normal_pixels[idx + 1] = static_cast<unsigned char>(
                    std::clamp(n_y * inv_len * 0.5f + 0.5f, 0.0f, 1.0f) *
                        255.0f +
                    0.5f);
                normal_pixels[idx + 2] = static_cast<unsigned char>(
                    std::clamp(inv_len * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f +
                    0.5f);
            }
        }
    };

    int thread_count = static_cast<int>(std::thread::hardware_concurrency());
    thread_count = std::max(1, std::min(thread_count, tex_res));
    const int chunk = (tex_res + thread_count - 1) / thread_count;
    std::vector<std::thread> workers;
    workers.reserve(thread_count - 1);
    for (int t = 1; t < thread_count; ++t) {
        const int ty0 = t * chunk;
        if (ty0 >= tex_res)
            break;
        workers.emplace_back(bake_rows, ty0, std::min(ty0 + chunk, tex_res));
    }
    bake_rows(0, std::min(chunk, tex_res));
    for (auto& w : workers)
        w.join();

    const int total_px = tex_res * tex_res;
    const auto pct = [&](int n) {
        return 100.0f * static_cast<float>(n) / static_cast<float>(total_px);
    };
    spdlog::info(
        "[terrain] biome coverage: rock {:.1f}% grass {:.1f}% snow {:.1f}% "
        "sediment>0.25 {:.1f}% gentle(<25deg) {:.1f}%",
        pct(rock_px.load()),
        pct(grass_px.load()),
        pct(snow_px.load()),
        pct(sed_px.load()),
        pct(gentle_px.load()));

    // Per-vertex dominant-biome id on the passthrough mesh (dense inputs
    // only — an adaptive mesh's vertices are not the hf grid). Uses the
    // SAME biome_region/biome_sed_w classifier as the texture texels, so a
    // downstream scatter filters on ONE truth with the texture: "biome"
    // 0=snow, 1=rock/bare, 2=grass (tree-suitable), 3=sediment channel
    // (see terrain_carry.hpp).
    {
        auto mesh = input.get_component<MeshComponent>();
        if (mesh && mesh->get_vertices().size() ==
                        static_cast<size_t>(res) * static_cast<size_t>(res)) {
            std::vector<float> biome(static_cast<size_t>(res) * res, 1.0f);
            int snow_v = 0, rock_v = 0, grass_v = 0, sed_v = 0;
            for (int y = 0; y < res; ++y) {
                const float v =
                    static_cast<float>(y) / static_cast<float>(res - 1);
                for (int x = 0; x < res; ++x) {
                    const float u =
                        static_cast<float>(x) / static_cast<float>(res - 1);

                    const size_t idx =
                        static_cast<size_t>(y) * static_cast<size_t>(res) + x;
                    const float wear_n =
                        hf.has_wear
                            ? std::clamp(hf.wear[idx] / wear_ref, 0.0f, 1.0f)
                            : 0.0f;
                    const BiomeRegion bio = biome_region(u, v);
                    const float sed_w =
                        biome_sed_w(wear_n, bio.snow_w, bio.grass_w);

                    // Dominant id: sediment channels win first (they cut
                    // through meadows), then the surface biomes. Grass
                    // admits at a lower weight than the others (tuned so
                    // meadows keep enough scatterable area).
                    float id = 1.0f;  // bare / unclassified rock ground
                    if (sed_w > 0.5f) {
                        id = 3.0f;
                        ++sed_v;
                    }
                    else if (bio.snow_w > 0.5f) {
                        id = 0.0f;
                        ++snow_v;
                    }
                    else if (bio.rock_w > 0.5f) {
                        id = 1.0f;
                        ++rock_v;
                    }
                    else if (bio.grass_w > 0.4f) {
                        id = 2.0f;
                        ++grass_v;
                    }
                    biome[idx] = id;
                }
            }
            mesh->add_vertex_scalar_quantity(terrain_carry::Q_BIOME, biome);
            const float vtotal = static_cast<float>(res) * res;
            spdlog::info(
                "[terrain] biome vertices: snow {:.1f}% rock {:.1f}% grass "
                "{:.1f}% sediment {:.1f}%",
                100.0f * snow_v / vtotal,
                100.0f * rock_v / vtotal,
                100.0f * grass_v / vtotal,
                100.0f * sed_v / vtotal);
        }
    }

    std::filesystem::path parent =
        std::filesystem::path(out_path).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent);

    const bool albedo_ok = stbi_write_png(
        out_path.c_str(),
        tex_res,
        tex_res,
        3,
        albedo_pixels.data(),
        tex_res * 3);
    const bool normal_ok = stbi_write_png(
        normal_path.c_str(),
        tex_res,
        tex_res,
        3,
        normal_pixels.data(),
        tex_res * 3);
    if (!albedo_ok || !normal_ok) {
        spdlog::warn(
            "terrain_texture_bake: failed to write {} / {}",
            out_path,
            normal_path);
        params.set_output("Height Field", input);
        params.set_output("Texture Path", std::string());
        params.set_output("Normal Path", std::string());
        return true;
    }

    spdlog::info(
        "[terrain] texture bake: {} + {} ({}x{}, wear_p97 {:.4f})",
        out_path,
        normal_path,
        tex_res,
        tex_res,
        wear_ref);

    params.set_output("Height Field", input);
    params.set_output("Texture Path", out_path);
    params.set_output("Normal Path", normal_path);
    return true;
}

NODE_DEF_CLOSE_SCOPE
