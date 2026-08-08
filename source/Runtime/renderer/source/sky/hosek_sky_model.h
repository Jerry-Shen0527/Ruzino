// Hosek-Wilkie analytic sky model — CPU-side cooking of the per-channel
// polynomial "state" that the GPU dome-light callable consumes.
//
// The model is:
//   Hosek & Wilkie, "An Analytic Model for Full Spectral Sky-Dome Radiance",
//   ACM TOG 31(4), SIGGRAPH 2012.
// Reference implementation: ArHosekSkyModel v1.4a (BSD-3-Clause) by
// Lukas Hosek and Alexander Wilkie, Charles University, Prague.
//
// This file ports ONLY the RGB cooking math (the CPU-side
// CookConfiguration/CookRadianceConfiguration that turn the precomputed
// dataset tables into a compact per-channel state) — enough to fill a tiny
// constant buffer each frame. The per-pixel radiance evaluation itself runs
// in the dome-light callable shader (eval_dome_light_hosek_wilkie.slang).
//
// The RGB dataset tables live in ArHosekSkyModelData_RGB.h (verbatim from the
// official 1.4a distribution, BSD-3-Clause) — using RGB avoids an XYZ->RGB
// matrix in the shader.
#pragma once

#include <cstdint>

namespace ruzino {

/// Cooked sky state: 3 colour channels (R, G, B), each with 9 polynomial
/// coefficients + 1 radiance scale. This is exactly what
/// arhosek_rgb_skymodelstate_alloc_init() produces in the reference code.
/// 30 floats total — fits trivially in a constant buffer / structured
/// buffer element.
struct HosekSkyState {
    float configs[3][9];  ///< [channel][coeff] — A..I of the Hosek model
    float radiances[3];   ///< [channel] radiance scale
};

/// Cook a Hosek-Wilkie sky state for the given parameters.
///
/// \param turbidity     Atmospheric turbidity, clamped to [1, 10].
///                      1 = very clear, 3 = clear, 6 = light haze,
///                      10 = hazy. Typical daytime: 2-4.
/// \param groundAlbedo  Ground reflectance [0, 1].
/// \param elevationRad  Solar elevation (sun above horizon), radians [0, pi/2].
///                      NOTE: this is *elevation*, not zenith angle.
/// \return              Cooked per-channel polynomial state.
HosekSkyState
hosek_cook(float turbidity, float groundAlbedo, float elevationRad);

}  // namespace ruzino
