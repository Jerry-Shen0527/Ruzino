import callable_data;
#include "utils/random.slangh"
import utils.Spectrum;

// Native spectral sky model based on Rayleigh scattering and solar spectrum
// References:
// - Preetham et al. "A Practical Analytic Model for Daylight" (SIGGRAPH 1999)
// - Bruneton et al. "Precomputed Atmospheric Scattering" (Eurographics 2008)

// Simplified Rayleigh scattering coefficient (wavelength dependent)
float rayleighScattering(float lambda)
{
    // Rayleigh scattering is proportional to 1/lambda^4
    // Normalized at 550nm (green)
    const float lambda_ref = 550.0;  // nm
    float ratio = lambda_ref / lambda;
    return pow(ratio, 4.0);
}

// Mie scattering coefficient (less wavelength dependent)
float mieScattering(float lambda)
{
    // Mie scattering is approximately proportional to 1/lambda
    // For aerosols and larger particles
    const float lambda_ref = 550.0;
    float ratio = lambda_ref / lambda;
    return pow(ratio, 0.8);  // Slightly wavelength dependent
}

// Solar spectrum approximation (extraterrestrial)
// Based on Wehrli AM0 solar spectrum
float solarSpectrum(float lambda)
{
    // Simplified blackbody-like solar spectrum at ~5800K
    // Peak around 500nm (green-yellow)

    if (lambda < 300.0 || lambda > 2500.0)
        return 0.0;

    // Approximate using Planck's law with corrections
    float temp = 5778.0;     // Solar effective temperature in Kelvin
    float c1 = 3.74183e-16;  // 2*pi*h*c^2
    float c2 = 1.43878e-2;   // h*c/k

    float l_m = lambda * 1e-9;  // Convert to meters
    float exponent = c2 / (l_m * temp);

    // Avoid overflow for small wavelengths
    if (exponent > 100.0)
        return 0.0;

    float spectral_radiance = c1 / (pow(l_m, 5.0) * (exp(exponent) - 1.0));

    // Normalize and scale to reasonable values
    return spectral_radiance * 1e-13;
}

// Atmospheric transmittance (simple exponential model)
float atmosphericTransmittance(float lambda, float zenithAngle, float altitude)
{
    // Optical depth depends on wavelength and path length through atmosphere
    float rayleigh = rayleighScattering(lambda) * 0.008;
    float mie = mieScattering(lambda) * 0.002;

    // Air mass factor (path length through atmosphere)
    float cosTheta = max(0.0, cos(zenithAngle));
    float airMass =
        1.0 /
        (cosTheta + 0.15 * pow(93.885 - zenithAngle * 180.0 / 3.14159, -1.253));

    float opticalDepth = (rayleigh + mie) * airMass;

    return exp(-opticalDepth);
}

// Sky radiance for a given wavelength
float skyRadianceAtWavelength(float lambda, float3 viewDir, float3 sunDir)
{
    float cosGamma = dot(viewDir, sunDir);
    float gamma = acos(clamp(cosGamma, -1.0, 1.0));  // Scattering angle

    // Zenith angle
    float zenithAngle = acos(clamp(viewDir.z, -1.0, 1.0));
    float sunZenith = acos(clamp(sunDir.z, -1.0, 1.0));

    // For directions below horizon (viewDir.z < 0), provide ambient ground radiance
    if (viewDir.z < 0.0) {
        // Ground ambient: reflected skylight (simplified as wavelength-dependent diffuse)
        float groundAlbedo = 0.5;  // Typical ground albedo
        // Use solar spectrum attenuated by atmosphere as indirect illumination
        float solar = solarSpectrum(lambda);
        float sunTransmittance = atmosphericTransmittance(lambda, sunZenith, 0.0);
        float groundRadiance = solar * sunTransmittance * groundAlbedo * 0.3;
        return groundRadiance;
    }

    // Rayleigh phase function
    float rayleighPhase = 0.75 * (1.0 + cosGamma * cosGamma);

    // Mie phase function (Henyey-Greenstein with g=0.76)
    float g = 0.76;
    float miePhase = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cosGamma, 1.5);
    miePhase *= 0.05;  // Reduce mie contribution

    // Solar contribution
    float solar = solarSpectrum(lambda);

    // Atmospheric scattering
    float rayleigh = rayleighScattering(lambda) * rayleighPhase;
    float mie = mieScattering(lambda) * miePhase;

    // Transmittance for sun-to-scattering point
    float sunTransmittance = atmosphericTransmittance(lambda, sunZenith, 0.0);

    // Transmittance for scattering point-to-eye
    float viewTransmittance =
        atmosphericTransmittance(lambda, zenithAngle, 0.0);

    // Total sky radiance at this wavelength
    float skyRadiance =
        solar * (rayleigh + mie) * sunTransmittance * viewTransmittance;

    // Add direct sun contribution for rays close to sun direction
    if (gamma < 0.05) {  // About 2.86 degrees
        float sunDisk =
            solar * sunTransmittance * 1000.0 * exp(-gamma * gamma / 0.0001);
        skyRadiance += sunDisk;
    }

    return skyRadiance;
}

// Procedural spectral sky dome light callable
[shader("callable")]
void eval_dome_light_procedural_sky_full_spec(inout DomeLightCallableData data)
{
    float3 rayDir = data.localDirection;

    // Sun direction (can be parameterized)
    float3 sunDir = normalize(float3(0.5, 0.5, 0.7));

    // Create spectral sky by computing radiance at each wavelength
    SampledSpectrum skySpectrum = SampledSpectrum(0.0);

    const int numSamples = 16;
    const float lambdaMin = 400.0;  // nm
    const float lambdaMax = 700.0;  // nm
    float lambdaStep = (lambdaMax - lambdaMin) / float(numSamples - 1);

    for (int i = 0; i < numSamples; ++i) {
        float lambda = lambdaMin + float(i) * lambdaStep;
        float radiance = skyRadianceAtWavelength(lambda, rayDir, sunDir);

        // Clamp and scale to reasonable range
        radiance = clamp(radiance, 0.0, 100.0) * 0.01;

        skySpectrum.setSample(i, radiance);
    }

    // Convert to Spectrum type and apply intensity scaling
    // If using RGBSpectrum, convert spectral data to RGB first
#ifdef USE_SAMPLED_SPECTRUM
    // Convert intensity from RGB to spectrum, then multiply
    SampledSpectrum intensitySpec = SampledSpectrum(data.intensity);
    data.envColor = mul(skySpectrum, intensitySpec);
#else
    // Convert sampled spectrum to RGB for RGBSpectrum mode
    float3 rgb = skySpectrum.toFloat3();
    data.envColor = Spectrum(rgb * data.intensity);
#endif
}
