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

#include <array>
#include <cmath>
#include <cstdlib>
#include <string>

#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "brush_sim_common.hpp"  // StrokeSample
#include "geom_node_base.h"

NODE_DEF_OPEN_SCOPE

// Stroke pattern (the "Shape" socket).
enum class PenShape { Line, Circle, Square, Figure8, Triangle, Star, Spiral };

// Regularized power f(u) = u·(u²+ε²)^((k−1)/2), k = 1/3 — behaves like the
// real cube root for |u| ≫ ε but is LINEAR with bounded slope ε^(−2/3) near
// 0 (the cube root's slope diverges, which would spike the arc-length table
// of the square shape). C∞ everywhere. f' and f'' in closed form:
//   f'  = g^m + 2·m·u²·g^(m−1),  f'' = 6·m·u·g^(m−1) + 4·m·(m−1)·u³·g^(m−2)
// with g = u² + ε², m = (k−1)/2.
constexpr float SQ_REG_EPS2 = 1.0e-4f;    // ε = 1e-2 cm
constexpr float SQ_REG_M = -1.0f / 3.0f;  // (k−1)/2 with k = 1/3
inline float reg_pow(float u)
{
    const float g = u * u + SQ_REG_EPS2;
    return u * std::pow(g, SQ_REG_M);
}
inline float reg_pow_d(float u)
{
    const float g = u * u + SQ_REG_EPS2;
    return std::pow(g, SQ_REG_M) +
           2.0f * SQ_REG_M * u * u * std::pow(g, SQ_REG_M - 1.0f);
}
inline float reg_pow_dd(float u)
{
    const float g = u * u + SQ_REG_EPS2;
    return 6.0f * SQ_REG_M * u * std::pow(g, SQ_REG_M - 1.0f) +
           4.0f * SQ_REG_M * (SQ_REG_M - 1.0f) * u * u * u *
               std::pow(g, SQ_REG_M - 2.0f);
}

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
    // Stroke pattern. "line" = the legacy wavy stroke (Length = X travel,
    // Cycles = sine wobbles). The closed shapes ("circle", "square",
    // "figure8", "triangle", "star") are arc-length normalized: Length =
    // perimeter, Cycles = loops, Speed = exact average speed. "spiral"
    // (Archimedean, Cycles = turns) is likewise arc-length normalized with
    // Length = total arc. Every shape eases in/out (C1 velocity at
    // touchdown/liftoff/corners) — see the continuity note in the execution
    // body.
    b.add_input<std::string>("Shape").default_val("line");
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
    const std::string shape_name = params.get_input<std::string>("Shape");
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
        // A held stab has no pattern to follow.
        PenShape shape = PenShape::Line;
        if (!blob) {
            if (shape_name == "circle")
                shape = PenShape::Circle;
            else if (shape_name == "square")
                shape = PenShape::Square;
            else if (shape_name == "figure8")
                shape = PenShape::Figure8;
            else if (shape_name == "triangle")
                shape = PenShape::Triangle;
            else if (shape_name == "star")
                shape = PenShape::Star;
            else if (shape_name == "spiral")
                shape = PenShape::Spiral;
        }

        // ---- C1 continuity contract --------------------------------------
        // This fixture feeds the fluid's moving-wall BC and the merge pass:
        // a velocity STEP here becomes a pressure impulse in the sim (the
        // §31 floor-jet lesson). Every phase transition therefore eases
        // through a smootherstep profile (value AND slope reach 0 together):
        //   DESCEND: z eases down,        vz = 0 at touchdown
        //   STROKE:  path fraction eases, XY speed = 0 at both ends
        //   LIFT:    z eases up,          vz = 0 at liftoff
        auto smootherstep = [](float u) {
            u = std::min(std::max(u, 0.0f), 1.0f);
            return u * u * u * (u * (6.0f * u - 15.0f) + 10.0f);
        };
        auto smootherstep_d = [](float u) {
            u = std::min(std::max(u, 0.0f), 1.0f);
            const float w = u - 1.0f;
            return 30.0f * u * u * w * w;
        };

        // ---- stroke patterns ----------------------------------------------
        // Unit parametrizations (t in [0,1], centered at the origin). Line
        // keeps the legacy geometry (Length = X travel, sine wobble, NOT
        // arc-length normalized). The closed shapes are built at unit size
        // and scaled so their TOTAL arc length equals Length — Speed is then
        // the exact average speed, and Cycles counts loops.
        //
        // The square is the superellipse |x|^6 + |y|^6 = 1: real corners with
        // BOUNDED curvature. A polygon's tangent direction steps 90° at each
        // vertex = a velocity jump; the superellipse rotates the tangent
        // continuously through the corner. Its raw θ-parametrization has
        // |dp/dθ| → ∞ at the side centers, so non-line shapes reparametrize
        // by arc length through a per-cook CDF table (2048 intervals): the
        // stroke runs at uniform speed with the smootherstep ease at the
        // ends, and the tangent direction still turns analytically.
        const float w = 2.0f * glm::pi<float>() * cycles;
        const float pi = glm::pi<float>();
        auto unit_path = [&](float t) -> glm::vec2 {
            switch (shape) {
                case PenShape::Circle: {
                    const float th = -pi * 0.5f + w * t;
                    return { std::cos(th), std::sin(th) };
                }
                case PenShape::Square: {
                    // Superellipse |x|^6 + |y|^6 = 1 through the REGULARIZED
                    // power f(u) = u·(u²+ε²)^((k−1)/2), k = 1/3: equal to
                    // cbrt(u) for |u| ≫ ε, LINEAR with bounded slope ε^(k−1)
                    // near 0 — the raw cbrt parametrization has |dp/dθ| → ∞ at
                    // the side centers, which would spike the arc-length table.
                    const float th = -pi * 0.5f + w * t;
                    const float c = std::cos(th), sn = std::sin(th);
                    return { reg_pow(c), reg_pow(sn) };
                }
                case PenShape::Figure8:
                    return { std::sin(2.0f * w * t + pi * 0.5f),
                             0.6f * std::sin(w * t) };
                case PenShape::Triangle:
                case PenShape::Star: {
                    // Harmonic polar: r = 1 + A·cos(N·(θ−θ0)) — a rounded
                    // N-lobe closed curve (N=3 reads as a rounded triangle, N=5
                    // as a five-pointed star). C∞ everywhere, curvature
                    // alternates sign around the lobes.
                    const float N = shape == PenShape::Triangle ? 3.0f : 5.0f;
                    const float A = shape == PenShape::Triangle ? 0.38f : 0.35f;
                    const float th = pi * 0.5f + w * t;
                    const float r = 1.0f + A * std::cos(N * (th - pi * 0.5f));
                    return { r * std::cos(th), r * std::sin(th) };
                }
                case PenShape::Spiral: {
                    // Archimedean, unit OUTER radius 1 shrinking to 0.24 at the
                    // center; Cycles = number of turns. The pen continuously
                    // approaches its own previous pass — the drain disc
                    // re-enters existing liquid the whole way in.
                    const float th = -pi * 0.5f + w * t;
                    const float r = 1.0f - 0.76f * t;
                    return { r * std::cos(th), r * std::sin(th) };
                }
                case PenShape::Line:
                default: return { (t - 0.5f) * length, amp * std::sin(w * t) };
            }
        };
        auto unit_dpath = [&](float t) -> glm::vec2 {
            switch (shape) {
                case PenShape::Circle: {
                    const float th = -pi * 0.5f + w * t;
                    return { -w * std::sin(th), w * std::cos(th) };
                }
                case PenShape::Square: {
                    // x' = f'(c)·c', c' = −sinθ; y' = f'(s)·cosθ.  (f'
                    // bounded.)
                    const float th = -pi * 0.5f + w * t;
                    const float c = std::cos(th), sn = std::sin(th);
                    return { -w * sn * reg_pow_d(c), w * c * reg_pow_d(sn) };
                }
                case PenShape::Figure8: {
                    const float p2 = 2.0f * w * t + pi * 0.5f;
                    return { 2.0f * w * std::cos(p2),
                             0.6f * w * std::cos(w * t) };
                }
                case PenShape::Triangle:
                case PenShape::Star: {
                    // dp/dθ = r'·e_r + r·e_θ, times dθ/dt = w.
                    // r' w.r.t. θ = −A·N·sin(N(θ−θ0)).
                    const float N = shape == PenShape::Triangle ? 3.0f : 5.0f;
                    const float A = shape == PenShape::Triangle ? 0.38f : 0.35f;
                    const float th = pi * 0.5f + w * t;
                    const float phi = N * (th - pi * 0.5f);
                    const float r = 1.0f + A * std::cos(phi);
                    const float rt = -A * N * std::sin(phi);
                    const float ct = std::cos(th), st = std::sin(th);
                    return { w * (rt * ct - r * st), w * (rt * st + r * ct) };
                }
                case PenShape::Spiral: {
                    // dp/dt = r'·e_r + r·θ'·e_θ with r' = −0.76 (dr/dt).
                    const float th = -pi * 0.5f + w * t;
                    const float r = 1.0f - 0.76f * t;
                    const float ct = std::cos(th), st = std::sin(th);
                    return { -0.76f * ct - r * w * st,
                             -0.76f * st + r * w * ct };
                }
                case PenShape::Line:
                default: return { length, amp * w * std::cos(w * t) };
            }
        };
        auto unit_ddpath = [&](float t) -> glm::vec2 {
            switch (shape) {
                case PenShape::Circle: {
                    const float th = -pi * 0.5f + w * t;
                    return { -w * w * std::cos(th), -w * w * std::sin(th) };
                }
                case PenShape::Square: {
                    // x'' = w²·(f''(c)·sin²θ − f'(c)·cosθ),
                    // y'' = w²·(f''(s)·cos²θ − f'(s)·sinθ).
                    const float th = -pi * 0.5f + w * t;
                    const float c = std::cos(th), sn = std::sin(th);
                    const float xpp =
                        reg_pow_dd(c) * sn * sn - reg_pow_d(c) * c;
                    const float ypp =
                        reg_pow_dd(sn) * c * c - reg_pow_d(sn) * sn;
                    return { w * w * xpp, w * w * ypp };
                }
                case PenShape::Figure8: {
                    const float p2 = 2.0f * w * t + pi * 0.5f;
                    return { -4.0f * w * w * std::sin(p2),
                             -0.6f * w * w * std::sin(w * t) };
                }
                case PenShape::Triangle:
                case PenShape::Star: {
                    // d²p/dθ² = (r'' − r)·e_r + 2r'·e_θ, times w².
                    // r'' = −A·N²·cos(N(θ−θ0)).
                    const float N = shape == PenShape::Triangle ? 3.0f : 5.0f;
                    const float A = shape == PenShape::Triangle ? 0.38f : 0.35f;
                    const float th = pi * 0.5f + w * t;
                    const float phi = N * (th - pi * 0.5f);
                    const float r = 1.0f + A * std::cos(phi);
                    const float rt = -A * N * std::sin(phi);
                    const float rtt = -A * N * N * std::cos(phi);
                    const float ar = rtt - r;
                    const float ct = std::cos(th), st = std::sin(th);
                    return { w * w * (ar * ct - 2.0f * rt * st),
                             w * w * (ar * st + 2.0f * rt * ct) };
                }
                case PenShape::Spiral: {
                    // d²p/dt² = −r·θ'²·e_r + 2r'·θ'·e_θ (r'' = 0).
                    const float th = -pi * 0.5f + w * t;
                    const float r = 1.0f - 0.76f * t;
                    const float ct = std::cos(th), st = std::sin(th);
                    return { -r * w * w * ct - 2.0f * 0.76f * w * (-st),
                             -r * w * w * st - 2.0f * 0.76f * w * ct };
                }
                case PenShape::Line:
                default: return { 0.0f, -amp * w * w * std::sin(w * t) };
            }
        };

        // Arc-length table for the closed shapes: cumulative arclength
        // FRACTIONS C[k] at t_k = k/N (scale-invariant, built on the unit
        // shape), per-interval density D[k] = dC/dt. Inversion returns
        // (t, dt/ds) for a stroke fraction s — |dp/ds| ends up uniform.
        // Legacy line skips this: s IS t, preserving the original pacing.
        constexpr int ARC_N = 2048;
        std::array<float, ARC_N + 1> arc_c{};
        std::array<float, ARC_N> arc_d{};
        float arc_total = 1.0f;
        float shape_scale = 1.0f;
        if (shape != PenShape::Line) {
            glm::vec2 prev = unit_path(0.0f);
            arc_c[0] = 0.0f;
            for (int k = 1; k <= ARC_N; ++k) {
                const glm::vec2 cur = unit_path(float(k) / float(ARC_N));
                arc_c[k] = arc_c[k - 1] + glm::length(cur - prev);
                prev = cur;
            }
            arc_total = std::max(arc_c[ARC_N], 1e-6f);
            // Normalize to fractions BEFORE deriving densities: the lookup
            // receives the stroke fraction s in [0,1].
            for (int k = 0; k <= ARC_N; ++k)
                arc_c[k] /= arc_total;
            for (int k = 0; k < ARC_N; ++k)
                arc_d[k] = (arc_c[k + 1] - arc_c[k]) * float(ARC_N);
            // Length = total perimeter of the (multi-loop) shape.
            shape_scale = length / arc_total;
        }
        auto arc_lookup = [&](float s, float& t, float& dt_ds) {
            s = std::min(std::max(s, 0.0f), 1.0f);
            int lo = 0, hi = ARC_N;  // C[lo] <= s <= C[hi]
            while (hi - lo > 1) {
                const int mid = (lo + hi) / 2;
                (arc_c[mid] <= s ? lo : hi) = mid;
            }
            const float span = std::max(arc_c[hi] - arc_c[lo], 1e-9f);
            const float wgt =
                std::min(std::max((s - arc_c[lo]) / span, 0.0f), 1.0f);
            t = (float(lo) + wgt) / float(ARC_N);
            const float density = std::max(
                arc_d[lo] +
                    wgt * (arc_d[hi < ARC_N ? hi : ARC_N - 1] - arc_d[lo]),
                1e-9f);
            dt_ds = 1.0f / density;
        };

        auto path_xy = [&](float t) {
            return glm::vec2(cx, cy) + unit_path(t) * shape_scale;
        };
        auto path_dxy = [&](float t) { return unit_dpath(t) * shape_scale; };
        auto path_ddxy = [&](float t) { return unit_ddpath(t) * shape_scale; };

        // Stroke heading, in the tilt-azimuth convention (the direction the
        // pen TIP leans toward). Handle-forward dragging means the tip leans
        // OPPOSITE the motion: az = atan2(-dy, -dx).
        auto heading_at = [&](float t) {
            const glm::vec2 dp = path_dxy(t);
            return std::atan2(-dp.y, -dp.x);
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
            // DESCEND: straight down at the stroke start point, z eased so
            // vz = 0 at touchdown (no vertical velocity step into STROKE).
            const float u = std::min(tau / T_d, 1.0f);
            const glm::vec2 p = path_xy(0.0f);
            out.pos =
                glm::vec3(p.x, p.y, z_hi + (z_lo - z_hi) * smootherstep(u));
            out.vel =
                glm::vec3(0.0f, 0.0f, (z_lo - z_hi) * smootherstep_d(u) / T_d);
            if (tilt_follow && !blob)
                az_cur = glm::degrees(heading_at(0.0f));
        }
        else if (tau < T_d + T_s) {
            // STROKE: pressed and traveling. The eased fraction s(u) plus
            // the arc-length reparam (t, dt/ds) give a C1 speed profile:
            // zero at both ends (continuous with DESCEND/LIFT), uniform in
            // between for the closed shapes.
            const float u = std::min((tau - T_d) / T_s, 1.0f);
            const float s = smootherstep(u);
            float t = s;
            float dt_ds = 1.0f;
            if (shape != PenShape::Line)
                arc_lookup(s, t, dt_ds);
            const glm::vec2 dp = path_dxy(t);
            const float v = dt_ds * smootherstep_d(u) / T_s;
            out.pos = glm::vec3(path_xy(t), press_z);
            out.vel = glm::vec3(dp.x * v, dp.y * v, 0.0f);
            out.active = true;
            out.stroke_start = !storage.stroke_started;
            storage.stroke_started = true;
            // Tilt ramps over the stroke: theta(s) = tilt + sweep*s.
            tilt_cur = tilt_deg + tilt_sweep_deg * s;
            sweep_rate = glm::radians(tilt_sweep_deg) / T_s;  // rad/s
            if (tilt_follow && !blob) {
                az_cur = glm::degrees(heading_at(t));
                // Analytic yaw rate d(az)/dt = signed curvature x speed.
                // With q = Rz(az)Ry(-theta)Rz(-az), the spatial angular
                // velocity of the rotating tilt axis is omega = theta' *
                // tilt_axis + az' * (ez - q*ez); the second term is the
                // "coning" correction (rotating the azimuth of an upright
                // pen is a no-op, hence the -q*ez subtraction).
                const glm::vec2 ddp = path_ddxy(t);
                const float denom = dp.x * dp.x + dp.y * dp.y;
                if (denom > 1e-12f) {
                    const float crossv = dp.x * ddp.y - dp.y * ddp.x;
                    const float sp = glm::length(glm::vec2(out.vel));
                    yaw_rate = crossv * sp / (denom * std::sqrt(denom));
                }
            }
        }
        else if (tau < T_d + T_s + T_l) {
            // LIFT: straight up at the stroke end point, z eased so vz = 0
            // at liftoff (continuous with the stroke's final speed).
            const float u = std::min((tau - T_d - T_s) / T_l, 1.0f);
            const glm::vec2 p = path_xy(1.0f);
            out.pos =
                glm::vec3(p.x, p.y, z_lo + (z_hi - z_lo) * smootherstep(u));
            out.vel =
                glm::vec3(0.0f, 0.0f, (z_hi - z_lo) * smootherstep_d(u) / T_l);
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
            // Coning term of the rotating azimuth (see the STROKE branch).
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
