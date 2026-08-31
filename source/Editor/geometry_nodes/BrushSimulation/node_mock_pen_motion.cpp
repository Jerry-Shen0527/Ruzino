// Mock pen motion — the analytic 3D pen-dynamics source for Wetbrush tests.
//
// This node authors the PEN directly: one StrokeSample per simulation frame,
// with every dynamics channel (position, orientation, velocity, angular
// velocity, pen down) evaluated ANALYTICALLY from an internal clock. It
// replaces the old mock path (mock_stroke -> curve -> mock_point_emitter
// polyline interpolation), whose "analytic" velocity was actually a polyline
// segment difference (C0-only positions -> accel spikes at every vertex),
// whose orientation was permanently identity (the StrokeSample orientation
// contract was never exercised), and whose pen was down for the whole
// trajectory (press/lift had to be smuggled in as a Z-profile curve via
// mock_press_lift).
//
// Timeline (tau = elapsed sim time since playback start):
//   DESCEND [0, T_d):            hover -> press Z at the stroke start point.
//                                Pen UP (active=false).
//   STROKE  [T_d, T_d+T_s):      pressed: sinuous path along +X centered on
//                                (Center X, Center Y). Pen DOWN; stroke_start
//                                fires on the first frame of this phase.
//                                T_s = Length/Speed (or Hold Duration when
//                                Length <= 0 — "blob" press-and-hold mode,
//                                absorbing mock_press_lift).
//   LIFT    [T_d+T_s, +T_l):     press -> hover Z at the stroke end point.
//                                Pen UP.
//   DONE:                        parked at hover above the stroke end. Pen UP.
//
// Orientation convention (matches deposit's [wb-pose] tilt diagnostic):
// identity = upright pen (local -Z = world down). Tilt Angle tips the pen
// toward the world-XY direction given by Tilt Azimuth; mat3_cast(q)[2].z =
// cos(tilt) so deposit's tilt_deg reads exactly the authored angle. Tilt
// Sweep ramps the tilt linearly along the stroke -> nonzero analytic omega.
//
// A/B switch: WB_NO_DYNAMICS=1 withholds the analytic derivatives (deposit
// falls back to its finite-difference path) — same gate as the old emitter.
//
// The curve replay path (mock_strokes + mock_point_emitter) is retained for
// captured-trajectory replay / multi-stroke color-mixing fixtures; this node
// is the primary input for physics tests.

#include <cmath>
#include <cstdlib>

#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "brush_sim_common.hpp"  // StrokeSample
#include "geom_node_base.h"

NODE_DEF_OPEN_SCOPE

// Per-instance playback clock. Not serialized (has_storage=false): playback
// restarts from the beginning when the graph is reloaded — same policy as
// mock_point_emitter's EmitterStorage.
struct PenMotionStorage {
    static constexpr bool has_storage = false;

    float elapsed = 0.0f;         // seconds since playback start
    bool started = false;         // first is_simulating cook seen
    bool stroke_started = false;  // STROKE phase entered (stroke_start once)
};

NODE_DECLARATION_FUNCTION(mock_pen_motion)
{
    // Enable=false: the pen never leaves hover — every frame emits an
    // inactive sample (no dip, no deposit). The input-contract equivalent
    // of the old "empty trajectory" degradation test.
    b.add_input<bool>("Enable").default_val(true);
    b.add_input<float>("Center X").default_val(0.0f).min(-1.0f).max(1.0f);
    b.add_input<float>("Center Y").default_val(0.0f).min(-1.0f).max(1.0f);
    b.add_input<float>("Length").default_val(0.3f).min(0.0f).max(2.0f);
    b.add_input<float>("Amplitude").default_val(0.05f).min(0.0f).max(1.0f);
    b.add_input<float>("Cycles").default_val(2.0f).min(0.0f).max(10.0f);
    b.add_input<float>("Speed").default_val(0.15f).min(0.001f).max(
        5.0f);  // world units / s along +X while pressed
    b.add_input<float>("Hover Z").default_val(0.08f).min(0.0f).max(1.0f);
    b.add_input<float>("Press Z").default_val(0.0f).min(-0.1f).max(0.5f);
    b.add_input<float>("Descend Duration")
        .default_val(0.05f)
        .min(0.001f)
        .max(5.0f);  // seconds
    b.add_input<float>("Lift Duration")
        .default_val(0.15f)
        .min(0.001f)
        .max(5.0f);  // seconds
    // Press-and-hold time in blob mode (Length <= 0): the pen stays at
    // Press Z for this long instead of traveling.
    b.add_input<float>("Hold Duration").default_val(0.3f).min(0.01f).max(5.0f);
    // 30deg is the DEFAULT: the validated drag pose (section 27/28 runs).
    // Upright (0deg) is the special case — set WB_PEN_TILT=0 explicitly.
    b.add_input<float>("Tilt").default_val(30.0f).min(0.0f).max(80.0f);  // deg
    b.add_input<float>("Tilt Azimuth")
        .default_val(0.0f)
        .min(-180.0f)
        .max(180.0f);  // deg, world XY direction the pen leans toward
    b.add_input<float>("Tilt Sweep")
        .default_val(0.0f)
        .min(-80.0f)
        .max(80.0f);  // deg, extra tilt ramped over the stroke
    // Tilt Follow Stroke: ignore "Tilt Azimuth" and derive the tilt direction
    // from the instantaneous stroke heading instead — the pen HANDLE leans
    // into the drag (tip azimuth = heading + 180deg), so the bristles trail
    // behind like a dragged real brush. DESCEND lands already slanted along
    // the initial heading (slanted pen-down); the heading rotates
    // continuously along the sine path with its analytic yaw rate folded
    // into angular_vel. Blob mode has no travel, so it keeps the fixed
    // azimuth (a stab stays a stab).
    b.add_input<bool>("Tilt Follow Stroke").default_val(true);
    b.add_input<float>("Ink R (RYB)").default_val(1.0f).min(0.0f).max(1.0f);
    b.add_input<float>("Ink Y (RYB)").default_val(0.0f).min(0.0f).max(1.0f);
    b.add_input<float>("Ink B (RYB)").default_val(0.0f).min(0.0f).max(1.0f);
    b.add_output<StrokeSample>("Stroke Sample");
}

NODE_EXECUTION_FUNCTION(mock_pen_motion)
{
    auto& storage = params.get_storage<PenMotionStorage&>();
    auto payload = params.get_global_payload<GeomPayload>();

    const bool enable = params.get_input<bool>("Enable");
    const float cx = params.get_input<float>("Center X");
    const float cy = params.get_input<float>("Center Y");
    const float length = params.get_input<float>("Length");
    const float amplitude = params.get_input<float>("Amplitude");
    const float cycles = params.get_input<float>("Cycles");
    const float speed = params.get_input<float>("Speed");
    const float hover_z = params.get_input<float>("Hover Z");
    const float press_z = params.get_input<float>("Press Z");
    const float descend_dur = params.get_input<float>("Descend Duration");
    const float lift_dur = params.get_input<float>("Lift Duration");
    const float hold_dur = params.get_input<float>("Hold Duration");
    const float tilt_deg = params.get_input<float>("Tilt");
    const float tilt_az_deg = params.get_input<float>("Tilt Azimuth");
    const float tilt_sweep_deg = params.get_input<float>("Tilt Sweep");
    const bool tilt_follow = params.get_input<bool>("Tilt Follow Stroke");
    const glm::vec3 ink(
        params.get_input<float>("Ink R (RYB)"),
        params.get_input<float>("Ink Y (RYB)"),
        params.get_input<float>("Ink B (RYB)"));

    StrokeSample out;
    out.active = false;

    // Only emit during active simulation. First sim frame has
    // is_simulating == false (init frame, animation.cpp:183-186) — same
    // convention as mock_point_emitter. Disabled pens park at hover above
    // the stroke start and never come down.
    if (payload.is_simulating) {
        if (!storage.started) {
            storage.started = true;
            storage.elapsed = 0.0f;
            storage.stroke_started = false;
        }

        const float tau = storage.elapsed;
        const float z_hi = std::max(hover_z, press_z);
        const float z_lo = std::min(hover_z, press_z);

        // Phase durations. Blob mode (Length <= 0): no travel, hold in place.
        const bool blob = length <= 1e-6f;
        const float T_d = std::max(descend_dur, 1e-4f);
        const float T_s =
            blob ? std::max(hold_dur, 1e-4f) : length / std::max(speed, 1e-4f);
        const float T_l = std::max(lift_dur, 1e-4f);
        // The sine wobble only makes sense while traveling.
        const float amp = blob ? 0.0f : amplitude;

        // The tilt axis is phase-dependent (azimuth may follow the stroke
        // heading), so it is built after the phase switch (tilt_axis_cur).

        // Stroke path (analytic, s in [0,1]): centered on (cx, cy), matching
        // mock_stroke's shape y = A*sin(4*pi*s) at Cycles=2 so rendered
        // strokes stay visually comparable across the fixture swap.
        auto path_xy = [&](float s) {
            return glm::vec2(
                cx + (s - 0.5f) * length,
                cy + amp * std::sin(2.0f * glm::pi<float>() * cycles * s));
        };
        auto path_dxy = [&](float s) {
            // d(pos_xy)/ds
            return glm::vec2(
                length,
                amp * std::cos(2.0f * glm::pi<float>() * cycles * s) * 2.0f *
                    glm::pi<float>() * cycles);
        };
        auto path_ddxy = [&](float s) {
            // d²(pos_xy)/ds²
            const float w = 2.0f * glm::pi<float>() * cycles;
            return glm::vec2(0.0f, -amp * std::sin(w * s) * w * w);
        };
        // Stroke heading, in the tilt-azimuth convention (the direction the
        // pen TIP leans toward). Handle-forward dragging means the tip leans
        // OPPOSITE the motion: az = atan2(-dy, -dx).
        auto heading_at = [&](float s) {
            glm::vec2 dp = path_dxy(s);
            return std::atan2(-dp.y, -dp.x);
        };
        // Analytic yaw rate d(az)/dt. With q = Rz(az)Ry(-theta)Rz(-az), the
        // spatial angular velocity of the rotating tilt axis is
        // omega = theta' * tilt_axis + az' * (ez - q*ez); the second term is
        // the "coning" correction (rotating the azimuth of an upright pen is
        // a no-op, hence the -q*ez subtraction).
        auto yaw_rate_at = [&](float s) {
            glm::vec2 dp = path_dxy(s);
            glm::vec2 ddp = path_ddxy(s);
            const float ds_dt = 1.0f / T_s;
            const float denom = dp.x * dp.x + dp.y * dp.y;
            if (denom < 1e-12f)
                return 0.0f;
            return (dp.x * ddp.y - dp.y * ddp.x) / denom * ds_dt;
        };

        float tilt_cur = tilt_deg;   // deg, varies only during STROKE (sweep)
        float az_cur = tilt_az_deg;  // deg, follows the stroke when enabled
        float yaw_rate = 0.0f;       // rad/s, d(az_cur)/dt while following
        float sweep_rate = 0.0f;     // rad/s, d(theta)/dt during STROKE sweep

        if (!enable) {
            // Disabled: park at hover above the stroke start.
            glm::vec2 p = path_xy(0.0f);
            out.pos = glm::vec3(p.x, p.y, z_hi);
        }
        else if (tau < T_d) {
            // DESCEND: straight down at the stroke start point. With tilt
            // follow, land already slanted along the initial heading.
            float u = tau / T_d;
            glm::vec2 p = path_xy(0.0f);
            out.pos = glm::vec3(p.x, p.y, z_hi + (z_lo - z_hi) * u);
            out.vel = glm::vec3(0.0f, 0.0f, (z_lo - z_hi) / T_d);
            if (tilt_follow && !blob)
                az_cur = glm::degrees(heading_at(0.0f));
        }
        else if (tau < T_d + T_s) {
            // STROKE: pressed and traveling. vel = d(pos)/dt with ds/dt=1/T_s.
            float s = std::min((tau - T_d) / T_s, 1.0f);
            glm::vec2 p = path_xy(s);
            glm::vec2 dp = path_dxy(s);
            out.pos = glm::vec3(p.x, p.y, press_z);
            out.vel = glm::vec3(dp.x / T_s, dp.y / T_s, 0.0f);
            out.active = true;
            out.stroke_start = !storage.stroke_started;
            storage.stroke_started = true;
            // Tilt ramps linearly over the stroke: theta(s) = tilt + sweep*s.
            tilt_cur = tilt_deg + tilt_sweep_deg * s;
            sweep_rate = glm::radians(tilt_sweep_deg) / T_s;  // rad/s
            if (tilt_follow && !blob) {
                az_cur = glm::degrees(heading_at(s));
                yaw_rate = yaw_rate_at(s);
            }
        }
        else if (tau < T_d + T_s + T_l) {
            // LIFT: straight up at the stroke end point.
            float u = (tau - T_d - T_s) / T_l;
            glm::vec2 p = path_xy(1.0f);
            out.pos = glm::vec3(p.x, p.y, z_lo + (z_hi - z_lo) * u);
            out.vel = glm::vec3(0.0f, 0.0f, (z_hi - z_lo) / T_l);
            tilt_cur = tilt_deg + tilt_sweep_deg;
            if (tilt_follow && !blob)
                az_cur = glm::degrees(heading_at(1.0f));
        }
        else {
            // DONE: parked at hover above the stroke end.
            glm::vec2 p = path_xy(1.0f);
            out.pos = glm::vec3(p.x, p.y, z_hi);
            out.vel = glm::vec3(0.0f);
            tilt_cur = tilt_deg + tilt_sweep_deg;
            if (tilt_follow && !blob)
                az_cur = glm::degrees(heading_at(1.0f));
        }

        // Orientation from the current tilt angle (identity when tilt = 0):
        // rotating about tilt_axis_cur tips the pen tip (local -Z) toward
        // (cos az_cur, sin az_cur, 0).
        const float az_rad = glm::radians(az_cur);
        const glm::vec3 tilt_axis_cur(
            std::sin(az_rad), -std::cos(az_rad), 0.0f);
        out.orientation = glm::angleAxis(glm::radians(tilt_cur), tilt_axis_cur);
        glm::vec3 omega = tilt_axis_cur * sweep_rate;
        if (yaw_rate != 0.0f) {
            // Coning term of the rotating azimuth (see yaw_rate_at).
            glm::vec3 pen_axis = glm::mat3_cast(out.orientation)[2];
            omega += yaw_rate * (glm::vec3(0.0f, 0.0f, 1.0f) - pen_axis);
        }
        out.angular_vel = omega;
        out.has_dynamics = std::getenv("WB_NO_DYNAMICS") == nullptr;
        out.color = ink;
        out.time = tau;

        storage.elapsed +=
            payload.delta_time > 0.0f ? payload.delta_time : (1.0f / 60.0f);
    }

    params.set_output("Stroke Sample", out);
    params.set_storage(storage);
    return true;
}

NODE_DECLARATION_UI(mock_pen_motion);

// ALWAYS_DIRTY: the real input (payload.is_simulating / delta_time) arrives
// via the global payload, not a graph socket, so dirty-state propagation
// would never re-cook this node.
NODE_DECLARATION_ALWAYS_DIRTY(mock_pen_motion);

NODE_DEF_CLOSE_SCOPE
