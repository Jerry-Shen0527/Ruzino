// Mock Point Emitter — streams control points one per simulation frame,
// replaying a captured stroke trajectory at the simulation's own frame rate.
//
// Pipeline role:
//   [brush_capture] --Stroke Curves--> [mock_point_emitter] --BrushPoint-->
//       [simulation_in] --> {wetbrush step} --> [simulation_out]
//
// The emitter does NOT read live mouse input. During drawing (is_simulating
// == false) it stays idle — only brush_capture accumulates. Once the user
// starts the simulation (is_simulating == true, driven by the Stage
// animation system / spacebar), the emitter advances an internal time
// cursor by payload.delta_time each frame and linearly interpolates the
// captured trajectory to produce "where the brush has reached now".
//
// Multi-stroke support: when the cursor crosses a stroke boundary the
// emitted BrushPoint carries stroke_start=true so downstream nodes can
// re-initialize (treat as a brand-new pen-down).

#include <cmath>

#include "GCore/Components/CurveComponent.h"
#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "brush_sim_common.hpp"  // BrushPoint (shared with wetbrush_step)
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

// Per-instance playback cursor. Persists across frames via Node::storage.
// Not serialized (has_storage=false): playback always restarts from the
// beginning when the graph is reloaded — the trajectory itself lives in
// brush_capture's serialized storage.
struct EmitterStorage {
    static constexpr bool has_storage = false;

    // Flattened trajectory cache, rebuilt whenever the input curve changes.
    // Stroke k occupies vertices [stroke_start[k],
    // stroke_start[k]+stroke_len[k]).
    std::vector<glm::vec3> points;
    std::vector<float> times;           // per-point stroke-local time
    std::vector<int> stroke_start_idx;  // first flat index of each stroke
    std::vector<int> stroke_len;        // vertex count per stroke
    int total_points = 0;
    int total_strokes = 0;
    uint64_t input_signature = 0;  // cheap change-detection (vertex count hash)

    // Playback cursor (in flat point index space, float for sub-point interp).
    bool playback_started = false;
    float cursor = 0.0f;         // flat index into points[], fractional
    int cur_stroke = 0;          // which stroke the cursor is in
    glm::vec3 last_pos{ 0.0f };  // last emitted pos (for done-detection)
};

// Forward declarations — helpers are defined after node_execution below.
static void advance_cursor(EmitterStorage& s, float elapsed);
static void sample_trajectory(const EmitterStorage& s, BrushPoint& out);

NODE_DECLARATION_FUNCTION(mock_point_emitter)
{
    // Optional: in a standalone (non-zone) graph this carries the captured
    // stroke curves directly. Inside a Wetbrush simulation zone the boundary
    // is single-typed (WetbrushFrame), so the curves arrive via the "Frame"
    // input below instead -- making this optional avoids MISSING_INPUT in the
    // zone path.
    b.add_input<Geometry>("Stroke Curves").optional(true);
    b.add_input<Ruzino::WetbrushFrame>("Frame").optional(true);
    b.add_input<float>("Replay Speed").default_val(1.0f).min(0.01f).max(10.0f);
    b.add_output<BrushPoint>("Current Point");
}

NODE_EXECUTION_FUNCTION(mock_point_emitter)
{
    auto& storage = params.get_storage<EmitterStorage&>();
    auto payload = params.get_global_payload<GeomPayload>();

    // Default output: an inactive point (pen up).
    BrushPoint out;
    out.active = false;

    // Refresh the trajectory cache from the input curve. Rebuild only when
    // the input signature changes (cheap: vertex count + stroke count) to
    // avoid re-parsing every frame. Prefer the WetbrushFrame.stroke_curves
    // (zone path) if wired, else the raw "Stroke Curves" socket.
    Geometry stroke_curves =
        params.has_input("Frame")
            ? params.get_input<Ruzino::WetbrushFrame>("Frame").stroke_curves
            : params.get_input<Geometry>("Stroke Curves");
    auto curve = stroke_curves.get_component<CurveComponent>();
    if (curve) {
        auto verts = curve->get_vertices();
        auto vc = curve->get_vert_count();
        uint64_t sig = static_cast<uint64_t>(verts.size()) * 1000003ULL +
                       static_cast<uint64_t>(vc.size());
        if (sig != storage.input_signature && !verts.empty()) {
            storage.input_signature = sig;
            storage.points = verts;
            storage.total_points = static_cast<int>(verts.size());

            // Timestamps (optional — stroke-local time per point).
            std::vector<float> ts;
            for (auto& name : curve->get_vertex_scalar_quantity_names()) {
                if (name == "timestamp") {
                    ts = curve->get_vertex_scalar_quantity("timestamp");
                    break;
                }
            }
            storage.times = ts;
            if (storage.times.size() != verts.size()) {
                // Fallback: synthesize uniform time spacing per stroke.
                storage.times.assign(verts.size(), 0.0f);
            }

            // Build flat-index stroke table from vert_count.
            storage.stroke_start_idx.clear();
            storage.stroke_len = vc;
            int offset = 0;
            for (int len : vc) {
                storage.stroke_start_idx.push_back(offset);
                offset += len;
            }
            storage.total_strokes = static_cast<int>(vc.size());

            // Synthesize per-stroke local timestamps if the captured ones
            // are missing or mismatched.
            if (storage.times.size() == verts.size() &&
                storage.total_strokes > 0) {
                bool needs_synth = false;
                for (int s = 0; s < storage.total_strokes; ++s) {
                    int s0 = storage.stroke_start_idx[s];
                    if (s0 < static_cast<int>(storage.times.size()) &&
                        storage.times[s0] != 0.0f) {
                        // captured timestamps exist; trust them.
                    }
                    else {
                        needs_synth = true;
                    }
                }
                if (needs_synth) {
                    for (int s = 0; s < storage.total_strokes; ++s) {
                        int s0 = storage.stroke_start_idx[s];
                        int len = storage.stroke_len[s];
                        for (int i = 0; i < len; ++i) {
                            storage.times[s0 + i] =
                                static_cast<float>(i) * (1.0f / 60.0f);
                        }
                    }
                }
            }

            // New trajectory -> reset playback.
            storage.playback_started = false;
            storage.cursor = 0.0f;
            storage.cur_stroke = 0;
        }
    }

    const float replay_speed = params.get_input<float>("Replay Speed");

    // Only emit during active simulation (spacebar / Stage play started).
    // First sim frame has is_simulating==false (init frame per
    // animation.cpp:183-186) — skip it, begin emitting next frame.
    if (payload.is_simulating && storage.total_points > 0) {
        if (!storage.playback_started) {
            storage.playback_started = true;
            storage.cursor = 0.0f;
            storage.cur_stroke = 0;
            // Emit the very first point as a stroke_start.
            out.pos = storage.points[0];
            out.time = storage.times.empty() ? 0.0f : storage.times[0];
            out.active = true;
            out.stroke_start = true;
            storage.last_pos = out.pos;
            // Advance cursor by one frame's worth of time.
            float dt =
                payload.delta_time > 0.0f ? payload.delta_time : (1.0f / 60.0f);
            // Map elapsed sim time back into trajectory index space: use
            // the local time spacing of the current stroke.
            advance_cursor(storage, dt * replay_speed);
        }
        else {
            // Check for playback completion.
            if (storage.cursor >= static_cast<float>(storage.total_points)) {
                // Trajectory exhausted — pen up.
                out.active = false;
                out.stroke_start = false;
            }
            else {
                sample_trajectory(storage, out);
            }
            float dt =
                payload.delta_time > 0.0f ? payload.delta_time : (1.0f / 60.0f);
            advance_cursor(storage, dt * replay_speed);
        }
    }

    params.set_output("Current Point", out);
    params.set_storage(storage);
    return true;
}

// --- helpers (file-local) ---

// Advance the cursor by `elapsed` seconds, mapping time into index space
// using the local time spacing within the current stroke. Crosses stroke
// boundaries by jumping the cursor to the next stroke's start.
static void advance_cursor(EmitterStorage& s, float elapsed)
{
    if (s.total_strokes <= 0)
        return;
    // Clamp current stroke.
    if (s.cur_stroke >= s.total_strokes) {
        s.cursor = static_cast<float>(s.total_points);
        return;
    }
    // Time per point in current stroke (guard against zero-length).
    int s0 = s.stroke_start_idx[s.cur_stroke];
    int len = s.stroke_len[s.cur_stroke];
    if (len <= 1) {
        // Single-point stroke: jump to next stroke.
        s.cur_stroke++;
        if (s.cur_stroke < s.total_strokes) {
            s.cursor = static_cast<float>(s.stroke_start_idx[s.cur_stroke]);
        }
        else {
            s.cursor = static_cast<float>(s.total_points);
        }
        return;
    }
    // Estimate point spacing from captured timestamps, fallback 1/60.
    float t_per_pt = 1.0f / 60.0f;
    int end = s0 + len;
    if (!s.times.empty() && end - 1 < static_cast<int>(s.times.size()) &&
        s.times[end - 1] > s.times[s0]) {
        t_per_pt =
            (s.times[end - 1] - s.times[s0]) / static_cast<float>(len - 1);
        if (t_per_pt <= 0.0f)
            t_per_pt = 1.0f / 60.0f;
    }
    float idx_advance = elapsed / t_per_pt;
    s.cursor += idx_advance;
    // Cross stroke boundaries if needed.
    while (s.cur_stroke < s.total_strokes &&
           s.cursor >= static_cast<float>(
                           s.stroke_start_idx[s.cur_stroke] +
                           s.stroke_len[s.cur_stroke])) {
        s.cur_stroke++;
    }
}

// Sample the trajectory at the current cursor position via linear
// interpolation. Sets stroke_start=true on the first point of a stroke.
static void sample_trajectory(const EmitterStorage& s, BrushPoint& out)
{
    float c = s.cursor;
    int i0 = static_cast<int>(std::floor(c));
    int i1 = i0 + 1;
    float frac = c - static_cast<float>(i0);
    if (i0 < 0)
        i0 = 0;
    if (i0 >= s.total_points) {
        out.active = false;
        return;
    }
    if (i1 >= s.total_points) {
        i1 = s.total_points - 1;
        frac = 0.0f;
    }
    out.pos = glm::mix(s.points[i0], s.points[i1], frac);
    if (!s.times.empty() && i1 < static_cast<int>(s.times.size())) {
        out.time = glm::mix(s.times[i0], s.times[i1], frac);
    }
    else {
        out.time = 0.0f;
    }
    out.active = true;
    // stroke_start on the first point of whichever stroke the cursor is in.
    int eff_stroke = s.cur_stroke;
    if (eff_stroke >= s.total_strokes)
        eff_stroke = s.total_strokes - 1;
    int stroke_first = s.stroke_start_idx[eff_stroke];
    out.stroke_start = (i0 == stroke_first && frac < 0.5f);
}

NODE_DECLARATION_UI(mock_point_emitter);
NODE_DECLARATION_ALWAYS_DIRTY(mock_point_emitter);
// ALWAYS_DIRTY: this node's real input (payload.is_simulating / delta_time)
// arrives via the global payload, not a graph socket, so dirty-state
// propagation would otherwise never re-cook it.

NODE_DEF_CLOSE_SCOPE
