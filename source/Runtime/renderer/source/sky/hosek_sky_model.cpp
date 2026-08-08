// Hosek-Wilkie analytic sky model — CPU-side cooking.
//
// The cooking math below is ported verbatim (algebra-for-algebra) from the
// reference implementation:
//   ArHosekSkyModel.c  v1.4a, February 22nd, 2013
//   by Lukas Hosek and Alexander Wilkie, Charles University, Prague.
// Published under the 3-clause BSD license (see the header comment in
// ArHosekSkyModelData_RGB.h). The dataset tables it reads are copied from
// the same official distribution, unchanged.
//
// Port notes:
//  - Only the RGB colour-space path is ported (no spectral / CIEXYZ / solar
//    disc / alienworld code).
//  - The reference cooks in double; we keep double for the Bernstein-blend
//    accumulation and cast to float at the end (matches Blender Cycles, which
//    proved the float result is visually identical).
//  - No heap allocation: the state is returned by value (30 floats).

#include "hosek_sky_model.h"

#include <cmath>

#include "ArHosekSkyModelData_RGB.h"

namespace ruzino {
namespace {

    constexpr double kPi = 3.141592653589793;

    // Degree-5 Bernstein blend over the 6 elevation knots, weighted by a
    // (1-albedo) / albedo and (1-turbidity_rem) / turbidity_rem factor. This is
    // the exact body of ArHosekSkyModel_CookConfiguration for one of the four
    // (albedo, turbidity) corners; factored out here so the bilinear blend
    // reads as four addends (matching the reference's structure).
    //
    // \param dataset    Pointer to the first of 6 consecutive 9-element knot
    // rows.
    // \param elevParam  Remapped elevation s = (elev / (pi/2))^(1/3).
    // \param coeffs     Accumulator (9 doubles).
    void addBernsteinCorner(
        const double* dataset,
        double elevParam,
        double weight,
        double* coeffs)
    {
        const double e0 = pow(1.0 - elevParam, 5.0);
        const double e1 = 5.0 * pow(1.0 - elevParam, 4.0) * elevParam;
        const double e2 =
            10.0 * pow(1.0 - elevParam, 3.0) * elevParam * elevParam;
        const double e3 =
            10.0 * pow(1.0 - elevParam, 2.0) * pow(elevParam, 3.0);
        const double e4 = 5.0 * (1.0 - elevParam) * pow(elevParam, 4.0);
        const double e5 = pow(elevParam, 5.0);
        for (int i = 0; i < 9; ++i) {
            coeffs[i] += weight * (e0 * dataset[i] + e1 * dataset[i + 9] +
                                   e2 * dataset[i + 18] + e3 * dataset[i + 27] +
                                   e4 * dataset[i + 36] + e5 * dataset[i + 45]);
        }
    }

    // Same blend, but for the 6-element radiance dataset (one value per knot,
    // returns the single blended scalar). Matches CookRadianceConfiguration.
    double cookRadianceCorner(
        const double* dataset,
        double elevParam,
        double weight,
        double acc)
    {
        const double e0 = pow(1.0 - elevParam, 5.0);
        const double e1 = 5.0 * pow(1.0 - elevParam, 4.0) * elevParam;
        const double e2 =
            10.0 * pow(1.0 - elevParam, 3.0) * elevParam * elevParam;
        const double e3 =
            10.0 * pow(1.0 - elevParam, 2.0) * pow(elevParam, 3.0);
        const double e4 = 5.0 * (1.0 - elevParam) * pow(elevParam, 4.0);
        const double e5 = pow(elevParam, 5.0);
        return acc +
               weight * (e0 * dataset[0] + e1 * dataset[1] + e2 * dataset[2] +
                         e3 * dataset[3] + e4 * dataset[4] + e5 * dataset[5]);
    }

}  // namespace

HosekSkyState
hosek_cook(float turbidityF, float groundAlbedoF, float elevationRadF)
{
    // Clamp inputs to the model's valid domain. Turbidity is sampled at
    // integers 1..10 in the dataset; albedo at 0 and 1; elevation >= 0.
    double turbidity = fmin(10.0, fmax(1.0, double(turbidityF)));
    double albedo = fmin(1.0, fmax(0.0, double(groundAlbedoF)));
    double elevation = fmin(kPi / 2.0, fmax(0.0, double(elevationRadF)));

    const int intTurbidity = int(turbidity);
    const double turbidityRem = turbidity - double(intTurbidity);

    // The dataset is keyed by the elevation remapped into "solar elevation
    // parameter" space, s = (elev / (pi/2))^(1/3), then blended across the 6
    // knots with degree-5 Bernstein weights. See reference code.
    const double s = pow(elevation / (kPi / 2.0), 1.0 / 3.0);

    HosekSkyState state{};
    double coeffsD[9];

    for (int channel = 0; channel < 3; ++channel) {
        const double* ds = datasetsRGB[channel];        // config dataset
        const double* dsRad = datasetsRGBRad[channel];  // radiance dataset

        // The config dataset layout (flat, per channel):
        //   [albedo 0: turbidity 1..10][albedo 1: turbidity 1..10]
        // each turbidity block is 9 coeffs * 6 knots = 54 doubles.
        // albedo-block stride = 9*6*10 = 540.
        for (int i = 0; i < 9; ++i)
            coeffsD[i] = 0.0;

        const double* alb0LowT = ds + 54 * (intTurbidity - 1);
        const double* alb1LowT = ds + 540 + 54 * (intTurbidity - 1);
        addBernsteinCorner(
            alb0LowT, s, (1.0 - albedo) * (1.0 - turbidityRem), coeffsD);
        addBernsteinCorner(alb1LowT, s, albedo * (1.0 - turbidityRem), coeffsD);

        if (intTurbidity != 10) {
            const double* alb0HighT = ds + 54 * intTurbidity;
            const double* alb1HighT = ds + 540 + 54 * intTurbidity;
            addBernsteinCorner(
                alb0HighT, s, (1.0 - albedo) * turbidityRem, coeffsD);
            addBernsteinCorner(alb1HighT, s, albedo * turbidityRem, coeffsD);
        }

        // Radiance dataset: same layout but 1 value per knot -> turbidity
        // block stride = 6, albedo-block stride = 6*10 = 60.
        double radD = 0.0;
        radD = cookRadianceCorner(
            dsRad + 6 * (intTurbidity - 1),
            s,
            (1.0 - albedo) * (1.0 - turbidityRem),
            radD);
        radD = cookRadianceCorner(
            dsRad + 60 + 6 * (intTurbidity - 1),
            s,
            albedo * (1.0 - turbidityRem),
            radD);
        if (intTurbidity != 10) {
            radD = cookRadianceCorner(
                dsRad + 6 * intTurbidity,
                s,
                (1.0 - albedo) * turbidityRem,
                radD);
            radD = cookRadianceCorner(
                dsRad + 60 + 6 * intTurbidity, s, albedo * turbidityRem, radD);
        }

        for (int i = 0; i < 9; ++i)
            state.configs[channel][i] = float(coeffsD[i]);
        state.radiances[channel] = float(radD);
    }

    return state;
}

}  // namespace ruzino
