// Damp velocity and increase dryness — 3D version
// Wetbrush Section 4.2: "we also increase the dryness of every grid cell
// by a small amount. Once the dryness reaches a threshold, we ignore its velocity."
//
// Two dispatch modes (cb.damp_mode) split along the buffer residency split:
//   0 — GLOBAL dispatch (res³ threads): wetness update only. Wetness is
//       canvas state and exists everywhere paint does, so drying is global
//       (§4.2: "every grid cell") even though the solve is windowed.
//   1 — WINDOW dispatch (win_n3d threads): velocity damp + dry-zero. The
//       velocity family is window-sized; outside the window there IS no
//       velocity (no liquid motion — §4.2).

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

// Grid density (canvas state, GLOBAL indexing) — gates the dryness test in
// the window pass: only cells that HAVE paint can be "dried solid". Wetness
// is written only by particle deposits, so the swarm region under the brush
// (mass in particles, no deposits) reads wetness 0 WITHOUT this gate and the
// merged swarm velocity was zeroed every substep — the grid velocity field
// was identically zero for the whole run (paper §4.2's "ignore its velocity"
// is about DRIED PAINT cells, not empty cells).
StructuredBuffer<float> density : register(t0);

// oil_density was declared here as an SRV but never sampled — removed (dead
// binding). If §6 oil-density viscosity coupling is ever implemented, it
// belongs in the fluid solve (jacobi), not here.

RWStructuredBuffer<float> vel_x   : register(u0);
RWStructuredBuffer<float> vel_y   : register(u1);
RWStructuredBuffer<float> vel_z   : register(u2);
RWStructuredBuffer<float> wetness : register(u3);

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    if (cb.damp_mode == 0) {
        // Global wetness-only dispatch.
        int N = cb.res;
        int D = cb.res_z;
        uint total = (uint)N * (uint)N * (uint)D;
        if (dtid.x >= total) return;
        uint idx = dtid.x;
        // Increase dryness (decrease wetness)
        wetness[idx] = max(0.0f, wetness[idx] - cb.drying_rate * cb.dt);
        return;
    }

    // Window dispatch: velocity handling (+ wetness at the same cells, so the
    // zero-velocity threshold check reads the PRE-drying wetness — same
    // ordering as the original single-pass shader).
    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    int WSX = cb.window_size_x;
    int WSZ = cb.window_size_z;
    int w0 = wnb(c, 0, 0, 0, WSX, WSZ);

    // Per-dispatch velocity damping (paper §4.2's damp step; the host sends
    // pow(frame_damp, 1/substeps) so decay is substep-count independent).
    // Default is 1.0 (off) — the paper's decay is viscosity diffusion
    // dilution + the dryness threshold below.
    float damp = cb.velocity_damp > 0.0 ? cb.velocity_damp : 1.0;
    vel_x[w0] *= damp;
    vel_y[w0] *= damp;
    vel_z[w0] *= damp;

    // Gravity body force on FLUID cells (wet + painted — the density-marked
    // liquid; dried-solid cells are excluded, empty cells carry no liquid
    // mass to accelerate). Standard Eulerian external-force step (paper §4.2
    // "standard way"); the following projection converts the downward push
    // into incompressible flow — against the solid canvas this becomes
    // LATERAL SPREADING, the sag/slump thick paint shows under gravity.
    // Restores the blob test's "should settle/spread like a liquid" —
    // previously the grid liquid had NO force at all and hovered where
    // deposited (user-observed 悬空 ink).
    if (density[c.idx] > 1e-6f && wetness[c.idx] >= 0.01f) {
        vel_x[w0] += cb.gravity_x * cb.dt;
        vel_y[w0] += cb.gravity_y * cb.dt;
        vel_z[w0] += cb.gravity_z * cb.dt;
    }

    // Zero velocity where DRIED PAINT is solidified (§4.2: "Once the dryness
    // reaches a threshold, we ignore its velocity"). The density gate matches
    // is_solid in fluid_divergence/gradient: an EMPTY cell (wetness 0 because
    // nothing was ever deposited there — e.g. the swarm region under the
    // brush) is not dry paint; its velocity field is the flowing liquid and
    // must survive.
    if (density[c.idx] > 1e-6f && wetness[c.idx] < 0.01f) {
        vel_x[w0] = 0.0f;
        vel_y[w0] = 0.0f;
        vel_z[w0] = 0.0f;
    }
}
