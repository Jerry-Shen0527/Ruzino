// Hosek-Wilkie analytic sky state, shared between host (light.cpp) and the
// dome-light callable shader (eval_dome_light_hosek_wilkie.slang).
//
// One HosekSkyState per Hosek dome light, cooked on the CPU from
// (turbidity, groundAlbedo, solarElevation) and uploaded into a
// StructuredBuffer<HosekSkyState> (hosekStateBuffer). The dome light's
// LightData.hosekStateIndex selects its row.
//
// This is the RGB flavour of the model (Hosek & Wilkie 2012): 3 colour
// channels, each 9 polynomial coefficients + 1 radiance scale = 30 floats.
#pragma once

struct HosekSkyState {
    float configs[3][9];  ///< [channel][coeff 0..8] — A..I of the Hosek model
    float radiances[3];   ///< [channel] radiance scale
};
