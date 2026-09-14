// terrain_adaptive_mesh: GPU-compute adaptive quadtree meshing of the
// heightfield (scheme B). Replaces the uniform res x res grid with a
// balanced tile-level mesh: C x C-cell tiles pick a subdivision level from
// their relief/slope error, levels are 2:1 balanced against neighbors, and
// finer-neighbor edges stitch (doubled edge points + clean-apex fan ring)
// so the assembled surface is watertight -- a crack would be a light leak
// in the path tracer. The reference mesher lives in TerrainGen/AdaMesh.hpp
// and is pinned by terrain_ada_stitch_test; the slang passes in
// TerrainGen/shaders/terrain_ada_*.slang mirror it.
//
// Emission is fully parallel on the GPU; the CPU only does tiny per-tile
// count readbacks + prefix sums to schedule exact-size output buffers.
// Vertex layout out of the shader: [pos3, normal3, uv2, water, sediment,
// wear] (11 floats).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "GCore/Components/MeshComponent.h"
#include "GCore/GOP.h"
#include "TerrainGen/AdaMesh.hpp"
#include "TerrainGen/Heightfield.h"
#include "geom_node_base.h"
#include "terrain_carry.hpp"
#include "terrain_gpu_common.hpp"

using namespace TerrainGen;

namespace Ruzino {
namespace {
    using namespace terrain_gpu;

    // Mirror of AdaConstants in terrain_ada_common.slangh (scalar-only).
    struct AdaConstantsCB {
        int res;
        int tile_cells;
        int tiles;
        int max_level;

        float detail_threshold;
        float slope_weight;
        float world_half;
        float cell_size;

        float inv_relief;
        int max_slots;
        float sag_tolerance;
        int pad2;
    };

    struct AdaBuffers {
        std::vector<nvrhi::BufferHandle> leased;
        ResourceAllocator* rc = nullptr;

        ~AdaBuffers()
        {
            for (auto& h : leased)
                if (h)
                    rc->destroy(h);
        }
    };

    nvrhi::BufferHandle
    lease_field(AdaBuffers& bufs, size_t n, const char* name)
    {
        nvrhi::BufferHandle h =
            terrain_gpu::create_field_buffer(*bufs.rc, n, name);
        bufs.leased.push_back(h);
        return h;
    }
}  // namespace
}  // namespace Ruzino

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(terrain_adaptive_mesh)
{
    b.add_input<Geometry>("Height Field");

    // Drives the error threshold: level = floor(log2(err / threshold)).
    // Higher detail -> lower threshold -> finer mesh.
    b.add_input<float>("Detail").min(0.0f).max(1.0f).default_val(0.5f);
    // How strongly slope (rise/run) feeds the error next to relief.
    b.add_input<float>("Slope Weight").min(0.0f).max(4.0f).default_val(1.0f);
    // Finest allowed level; stride = tile_cells >> level. Level 4 on a
    // 16-cell tile means 1-cell quads = full field resolution.
    b.add_input<int>("Max Subdiv").min(0).max(4).default_val(4);
    // Curvature pass: coarse-quad sag budget in world (height) units. A
    // tile refines until its flat quads deviate from the true surface by
    // less than this (sag = second difference * stride^2 / 8, quartering
    // per level). 0 disables: relief/slope alone cannot see smooth bending
    // surfaces (quadric test 2026-09-08: a 38-deg dome stayed at level 0
    // because planar tilt is not error, only changing slope is).
    b.add_input<float>("Sag Tolerance").min(0.0f).max(100.0f).default_val(0.0f);

    b.add_output<Geometry>("Height Field");
}

NODE_EXECUTION_FUNCTION(terrain_adaptive_mesh)
{
    if (!params.has_input("Height Field")) {
        spdlog::warn("terrain_adaptive_mesh: no Height Field input");
        return false;
    }
    Geometry input = params.get_input<Geometry>("Height Field");

    Heightfield hf;
    std::string err;
    if (!terrain_carry::heightfield_from_mesh(input, hf, err)) {
        spdlog::warn("terrain_adaptive_mesh: {}", err);
        params.set_output("Height Field", input);
        return true;
    }

    const float detail = params.get_input<float>("Detail");
    const float slope_weight = params.get_input<float>("Slope Weight");
    const int max_subdiv = params.get_input<int>("Max Subdiv");
    const float sag_tolerance = params.get_input<float>("Sag Tolerance");

    const int res = hf.res;
    const int C = 16;  // tile_cells: (res - 1) must be divisible by C
    if (max_subdiv < 0 || max_subdiv > 4 || (res - 1) % C != 0 || res < 2 * C) {
        spdlog::warn(
            "terrain_adaptive_mesh: res {} incompatible with tile size {} "
            "(needs (res-1) % {} == 0, res >= {}); pass the dense input",
            res,
            C,
            C,
            2 * C + 1);
        params.set_output("Height Field", input);
        return true;
    }
    const int T = (res - 1) / C;
    const int max_level = std::min(max_subdiv, 4);

    const size_t n = static_cast<size_t>(res) * res;
    const size_t nt = static_cast<size_t>(T) * T;
    const size_t max_slots =
        static_cast<size_t>(C + 1) * (C + 1) + 4ull * C;  // s = 1 worst case

    std::vector<float> zero(n, 0.0f);
    const std::vector<float>& water = hf.has_water ? hf.water : zero;
    const std::vector<float>& sediment = hf.has_sediment ? hf.sediment : zero;
    const std::vector<float>& wear = hf.has_wear ? hf.wear : zero;

    const auto t0 = std::chrono::steady_clock::now();

    Ruzino::init_gpu_geometry_algorithms();
    if (!Ruzino::is_gpu_alive() || !RHI::get_device()) {
        spdlog::warn(
            "terrain_adaptive_mesh: GPU unavailable, passing dense input");
        params.set_output("Height Field", input);
        return true;
    }
    nvrhi::IDevice* device = RHI::get_device();
    auto& rc = Ruzino::get_resource_allocator();

    AdaBuffers bufs;
    bufs.rc = &rc;

    // Fields + tile grids.
    auto b_height = lease_field(bufs, n, "ada_height");
    auto b_water = lease_field(bufs, n, "ada_water");
    auto b_sed = lease_field(bufs, n, "ada_sed");
    auto b_wear = lease_field(bufs, n, "ada_wear");
    auto b_level_a = lease_field(bufs, nt, "ada_lvl_a");
    auto b_level_b = lease_field(bufs, nt, "ada_lvl_b");
    auto b_vtx_count = lease_field(bufs, nt, "ada_vtx_count");
    auto b_idx_count = lease_field(bufs, nt, "ada_idx_count");
    auto b_vtx_base = lease_field(bufs, nt, "ada_vtx_base");
    auto b_idx_base = lease_field(bufs, nt, "ada_idx_base");

    {
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        cmd->writeBuffer(b_height, hf.height.data(), n * sizeof(float));
        cmd->writeBuffer(b_water, water.data(), n * sizeof(float));
        cmd->writeBuffer(b_sed, sediment.data(), n * sizeof(float));
        cmd->writeBuffer(b_wear, wear.data(), n * sizeof(float));
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        rc.destroy(cmd);
    }

    AdaConstantsCB cb{};
    cb.res = res;
    cb.tile_cells = C;
    cb.tiles = T;
    cb.max_level = max_level;
    // Detail 0 -> threshold 2.0 (coarse), 1 -> 0.125 (fine).
    cb.detail_threshold =
        std::exp2(1.0f - 4.0f * std::clamp(detail, 0.0f, 1.0f));
    cb.slope_weight = slope_weight;
    cb.world_half = hf.world_size * 0.5f;
    cb.cell_size = hf.cell_size();
    cb.inv_relief = 1.0f / std::max(hf.max_height() - hf.min_height(), 1e-6f);
    cb.max_slots = static_cast<int>(max_slots);
    cb.sag_tolerance = sag_tolerance;

    nvrhi::BufferHandle cb_buf = rc.create(
        nvrhi::BufferDesc{}
            .setByteSize(sizeof(AdaConstantsCB))
            .setIsConstantBuffer(true)
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true)
            .setDebugName("ada_cb"));
    bufs.leased.push_back(cb_buf);
    terrain_gpu::upload_constant_buffer(rc, device, &cb, sizeof(cb), cb_buf);

    auto prog_error =
        terrain_gpu::compile_shader(rc, "terrain_ada_error.slang");
    auto prog_balance =
        terrain_gpu::compile_shader(rc, "terrain_ada_balance.slang");
    auto prog_count =
        terrain_gpu::compile_shader(rc, "terrain_ada_count.slang");
    auto prog_vtx =
        terrain_gpu::compile_shader(rc, "terrain_ada_emit_vtx.slang");
    auto prog_idx =
        terrain_gpu::compile_shader(rc, "terrain_ada_emit_idx.slang");
    if (!prog_error || !prog_balance || !prog_count || !prog_vtx || !prog_idx) {
        spdlog::warn("terrain_adaptive_mesh: shader compile failed");
        params.set_output("Height Field", input);
        return true;
    }
    struct ProgsGuard {
        ResourceAllocator& rc;
        ProgramHandle& a;
        ProgramHandle& b;
        ProgramHandle& c;
        ProgramHandle& d;
        ProgramHandle& e;
        ~ProgsGuard()
        {
            for (auto* p : { &a, &b, &c, &d, &e })
                if (*p)
                    rc.destroy(*p);
        }
    } progs_guard{
        rc, prog_error, prog_balance, prog_count, prog_vtx, prog_idx
    };

    // 1. error -> level
    terrain_gpu::dispatch_shader(
        rc,
        prog_error,
        { { "height", b_height } },
        { { "tile_level", b_level_a } },
        cb_buf,
        int(nt));

    // 2. 2:1 balance relaxation (ping-pong)
    nvrhi::BufferHandle lvl_cur = b_level_a, lvl_nxt = b_level_b;
    for (int iter = 0; iter <= max_level; ++iter) {
        terrain_gpu::dispatch_shader(
            rc,
            prog_balance,
            { { "level_in", lvl_cur } },
            { { "level_out", lvl_nxt } },
            cb_buf,
            int(nt));
        std::swap(lvl_cur, lvl_nxt);
    }

    // 3. counts
    terrain_gpu::dispatch_shader(
        rc,
        prog_count,
        { { "tile_level", lvl_cur } },
        { { "vertex_count", b_vtx_count }, { "index_count", b_idx_count } },
        cb_buf,
        int(nt));

    // 4. readback counts, prefix-sum on CPU, upload bases
    std::vector<float> vtx_count(nt), idx_count(nt);
    terrain_gpu::readback_floats(rc, device, b_vtx_count, vtx_count.data(), nt);
    terrain_gpu::readback_floats(rc, device, b_idx_count, idx_count.data(), nt);

    size_t total_vtx = 0, total_idx = 0;
    std::vector<float> vtx_base(nt), idx_base(nt);
    for (size_t i = 0; i < nt; ++i) {
        vtx_base[i] = float(total_vtx);
        idx_base[i] = float(total_idx);
        total_vtx += uint32_t(vtx_count[i] + 0.5f);
        total_idx += uint32_t(idx_count[i] + 0.5f);
    }
    if (total_vtx == 0 || total_idx == 0) {
        spdlog::warn("terrain_adaptive_mesh: empty mesh, passing input");
        params.set_output("Height Field", input);
        return true;
    }
    terrain_gpu::upload_floats(rc, device, b_vtx_base, vtx_base.data(), nt);
    terrain_gpu::upload_floats(rc, device, b_idx_base, idx_base.data(), nt);

    auto b_vertices = lease_field(bufs, total_vtx * 11, "ada_vertices");
    auto b_indices = lease_field(bufs, total_idx, "ada_indices");

    // 5. emit
    terrain_gpu::dispatch_shader(
        rc,
        prog_vtx,
        { { "tile_level", lvl_cur },
          { "vertex_base", b_vtx_base },
          { "height", b_height },
          { "water", b_water },
          { "sediment", b_sed },
          { "wear", b_wear } },
        { { "vertices", b_vertices } },
        cb_buf,
        int(nt * max_slots));
    terrain_gpu::dispatch_shader(
        rc,
        prog_idx,
        { { "tile_level", lvl_cur },
          { "vertex_base", b_vtx_base },
          { "index_base", b_idx_base } },
        { { "indices", b_indices } },
        cb_buf,
        int(nt));

    // 6. readback
    std::vector<float> vtx_data(total_vtx * 11);
    std::vector<uint32_t> idx_data(total_idx);
    terrain_gpu::readback_floats(
        rc, device, b_vertices, vtx_data.data(), total_vtx * 11);
    terrain_gpu::readback_floats(
        rc,
        device,
        b_indices,
        reinterpret_cast<float*>(idx_data.data()),
        total_idx);

    // Readback sanity: duplicate directed edges mean the emit/dispatch
    // chain corrupted the buffer (a watertight mesh has each directed
    // edge exactly once). O(n log n) over all indices plus tile-level
    // histogram readback -- opt-in via TERRAIN_ADA_DEBUG now that
    // terrain_ada_stitch_test + test_terrain_adaptive.py pin the
    // watertightness contract.
    static const bool ada_debug = std::getenv("TERRAIN_ADA_DEBUG") != nullptr;
    if (ada_debug) {
        std::vector<uint64_t> dkeys(total_idx);
        for (size_t k = 0; k < total_idx; k += 3) {
            for (int e = 0; e < 3; ++e) {
                const uint32_t p = idx_data[k + e];
                const uint32_t q = idx_data[k + (e + 1) % 3];
                dkeys[k + e] = (uint64_t(p) << 32) | uint64_t(q);
            }
        }
        std::sort(dkeys.begin(), dkeys.end());

        // Report duplicate directed edges (with tile attribution) BEFORE
        // std::unique destroys the adjacent runs.
        int printed = 0;
        size_t dup_pairs = 0;
        const auto decode_tile = [&](uint32_t vid) {
            int lo = 0, hi = T - 1;
            while (lo < hi) {
                const int mid = (lo + hi + 1) / 2;
                if (vtx_base[uint32_t(mid)] <= float(vid))
                    lo = mid;
                else
                    hi = mid - 1;
            }
            return lo;
        };
        for (size_t k = 0; k + 1 < dkeys.size(); ++k) {
            if (dkeys[k] != dkeys[k + 1])
                continue;
            ++dup_pairs;
            if (printed < 5) {
                const uint32_t p = uint32_t(dkeys[k] >> 32);
                const uint32_t q = uint32_t(dkeys[k] & 0xffffffffu);
                spdlog::info(
                    "[terrain] dup directed edge {}->{} (tiles {} and {})",
                    p,
                    q,
                    decode_tile(p),
                    decode_tile(q));
                ++printed;
            }
        }
        const size_t uniq = dkeys.size() - dup_pairs;
        spdlog::info(
            "[terrain] adaptive mesh readback: directed edges {} unique {} "
            "(duplicate pairs {})",
            total_idx,
            uniq,
            dup_pairs);
    }

    const auto ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();

    // Level histogram (tuning diagnostics).
    std::string hist_s;
    if (ada_debug) {
        std::vector<float> lvl(nt);
        terrain_gpu::readback_floats(rc, device, lvl_cur, lvl.data(), nt);
        int hist[8] = {};
        for (float l : lvl) {
            const int li = std::clamp(int(l + 0.5f), 0, 7);
            hist[li]++;
        }
        for (int l = 0; l <= max_level; ++l) {
            hist_s += " L" + std::to_string(l) + "=" + std::to_string(hist[l]);
        }
    }

    // ---- pack Geometry ----
    Geometry geom = Geometry::CreateMesh();
    auto mesh = geom.get_component<MeshComponent>();

    const float h_min = hf.min_height();
    const float h_max = std::max(hf.max_height(), h_min + 1e-6f);

    std::vector<glm::vec3> vertices(total_vtx);
    std::vector<glm::vec3> normals(total_vtx);
    std::vector<glm::vec2> uvs(total_vtx);
    std::vector<glm::vec3> colors(total_vtx);
    std::vector<float> q_water(total_vtx), q_sed(total_vtx), q_wear(total_vtx);
    for (size_t i = 0; i < total_vtx; ++i) {
        const float* v = &vtx_data[i * 11];
        vertices[i] = { v[0], v[1], v[2] };
        normals[i] = { v[3], v[4], v[5] };
        uvs[i] = { v[6], v[7] };
        q_water[i] = v[8];
        q_sed[i] = v[9];
        q_wear[i] = v[10];
        const float t =
            std::clamp((v[1] - h_min) / (h_max - h_min), 0.0f, 1.0f);
        colors[i] = { 0.15f + 0.85f * t, 0.25f + 0.75f * t, 0.35f + 0.65f * t };
    }
    std::vector<int> indices(total_idx);
    for (size_t i = 0; i < total_idx; ++i)
        indices[i] = int(idx_data[i]);
    std::vector<int> counts(total_idx / 3, 3);

    mesh->set_vertices(vertices);
    mesh->set_normals(normals);
    mesh->set_texcoords_array(uvs);
    mesh->set_display_color(colors);
    mesh->set_face_vertex_counts(counts);
    mesh->set_face_vertex_indices(indices);
    mesh->add_vertex_scalar_quantity(terrain_carry::Q_WATER, q_water);
    mesh->add_vertex_scalar_quantity(terrain_carry::Q_SEDIMENT, q_sed);
    mesh->add_vertex_scalar_quantity(terrain_carry::Q_WEAR, q_wear);

    spdlog::info(
        "[terrain] adaptive mesh: res={} tiles={}{} verts {} (dense {}) "
        "tris {} (dense {}) gpu {:.1f} ms",
        res,
        nt,
        hist_s,
        total_vtx,
        n,
        total_idx / 3,
        (res - 1) * (res - 1) * 2,
        ms);

    params.set_output("Height Field", geom);
    return true;
}

NODE_DEF_CLOSE_SCOPE
