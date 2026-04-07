#ifdef __cplusplus
#include <pxr/base/gf/vec3f.h>

struct ISpectrum {
    // virtual pxr::GfVec3f toFloat3() = 0;
};

struct RGBSpectrum : ISpectrum {
    RGBSpectrum(const pxr::GfVec3f& data) : data(data)
    {
    }
    pxr::GfVec3f data;
    float padding;
};
#else

// ============================================================================
// Spectrum Configuration
// ============================================================================
// Toggle between RGB and Sampled spectrum by changing this define
// #define USE_RGB_SPECTRUM 1  // Use RGB spectrum (default)
// #define USE_SAMPLED_SPECTRUM 1  // Uncomment to use sampled spectrum

interface ISpectrum {
    // Convert the spectrum to a float3 representation
    float3 toFloat3();
}

// ============================================================================
// CIE 1931 XYZ Color Matching Functions (Multi-lobe Gaussian approximation)
// Reference: "Simple Analytic Approximations to the CIE XYZ Color Matching
// Functions"
// For Slang GPU side, we use the approximation since exact lookup tables are
// too large
// ============================================================================
float gaussianXYZ(float x, float lambda, float invSigma1, float invSigma2)
{
    float t = (x - lambda) * (x < lambda ? invSigma1 : invSigma2);
    return exp(-0.5f * t * t);
}

float3 wavelengthToXYZ(float lambda)
{
    // Multi-lobe piecewise Gaussian fit for CIE XYZ color matching functions
    // Based on "Simple Analytic Approximations to the CIE XYZ Color Matching
    // Functions" - Wyman et al., JCGT 2013
    // This gives good accuracy for GPU-side spectral calculations

    float x = 0.362f * gaussianXYZ(lambda, 442.0f, 0.0624f, 0.0374f) +
              1.056f * gaussianXYZ(lambda, 599.8f, 0.0264f, 0.0323f) -
              0.065f * gaussianXYZ(lambda, 501.1f, 0.0490f, 0.0382f);

    float y = 0.821f * gaussianXYZ(lambda, 568.8f, 0.0213f, 0.0247f) +
              0.286f * gaussianXYZ(lambda, 530.9f, 0.0613f, 0.0322f);

    float z = 1.217f * gaussianXYZ(lambda, 437.0f, 0.0845f, 0.0278f) +
              0.681f * gaussianXYZ(lambda, 459.0f, 0.0385f, 0.0725f);

    return float3(x, y, z);
}

// ============================================================================
// D65 Standard Illuminant
// Reference: CIE Standard Illuminant D65 (daylight)
// ============================================================================
float wavelengthToD65(float lambda)
{
    // D65 illuminant spectral power distribution (normalized)
    // More precise values sampled at regular intervals
    // Based on: https://en.wikipedia.org/wiki/Illuminant_D65

    if (lambda < 300.0f || lambda > 830.0f)
        return 0.0f;

    // D65 values at 5nm intervals from 300-830nm (107 samples)
    // These are normalized values
    const float d65_table[] = {
        0.034f,   1.664f,   3.294f,   11.765f,  20.236f,  28.644f,  37.053f,
        38.501f,  39.948f,  42.430f,  44.911f,  45.775f,  46.638f,  49.363f,
        52.089f,  51.032f,  49.975f,  52.311f,  54.648f,  68.701f,  82.754f,
        87.120f,  91.486f,  92.458f,  93.431f,  90.057f,  86.682f,  95.773f,
        104.864f, 110.936f, 117.008f, 117.410f, 117.812f, 116.336f, 114.861f,
        115.392f, 115.923f, 112.367f, 108.811f, 109.082f, 109.354f, 108.578f,
        107.802f, 106.296f, 104.790f, 106.239f, 107.689f, 106.047f, 104.405f,
        104.225f, 104.046f, 102.023f, 100.000f, 98.167f,  96.334f,  96.061f,
        95.788f,  92.236f,  88.685f,  89.345f,  90.006f,  89.802f,  89.599f,
        88.648f,  87.698f,  85.493f,  83.288f,  83.493f,  83.699f,  81.863f,
        80.026f,  80.120f,  80.214f,  81.246f,  82.277f,  80.281f,  78.284f,
        74.002f,  69.721f,  70.665f,  71.609f,  72.979f,  74.349f,  67.976f,
        61.604f,  65.744f,  69.885f,  72.486f,  75.087f,  69.339f,  63.592f,
        55.005f,  46.418f,  56.611f,  66.805f,  65.094f,  63.382f,  63.843f,
        64.304f,  61.877f,  59.451f,  55.705f,  51.959f,  54.699f,  57.440f,
        58.876f,  60.312f
    };

    // Normalize lambda to table index (5nm spacing from 300nm)
    float idx = (lambda - 300.0f) / 5.0f;

    if (idx < 0.0f)
        return d65_table[0];
    if (idx >= 106.0f)
        return d65_table[106];

    // Linear interpolation between table values
    int i0 = int(idx);
    int i1 = min(i0 + 1, 106);
    float t = frac(idx);

    return lerp(d65_table[i0], d65_table[i1], t);
}

// ============================================================================
// RGB to XYZ Conversion (Rec. 709 with D65 illuminant)
// ============================================================================
float3 rgbToXYZ_Rec709(float3 rgb)
{
    // RGB (Rec. 709) to XYZ conversion matrix (D65 illuminant)
    float3 xyz;
    xyz.x = 0.4124f * rgb.r + 0.3576f * rgb.g + 0.1805f * rgb.b;
    xyz.y = 0.2126f * rgb.r + 0.7152f * rgb.g + 0.0722f * rgb.b;
    xyz.z = 0.0193f * rgb.r + 0.1192f * rgb.g + 0.9505f * rgb.b;
    return xyz;
}

// ============================================================================
// XYZ to RGB (Rec. 709 with D65 illuminant)
// ============================================================================
float3 xyzToRGB_Rec709(float3 xyz)
{
    // XYZ to Rec. 709 RGB conversion matrix (D65 illuminant)
    float3 rgb;
    rgb.r = 3.2406f * xyz.x - 1.5372f * xyz.y - 0.4986f * xyz.z;
    rgb.g = -0.9689f * xyz.x + 1.8758f * xyz.y + 0.0415f * xyz.z;
    rgb.b = 0.0557f * xyz.x - 0.2040f * xyz.y + 1.0570f * xyz.z;

    return rgb;
}

struct RGBSpectrum : ISpectrum {
    __init(float v)
    {
        data = float3(v, v, v);
        padding = 0;
    }

    __init(float3 rgb)
    {
        data = rgb;
        padding = 0;
    }

    float3 data;
    float padding;

    float3 toFloat3()
    {
        return data;
    }
};

// Helper functions for RGBSpectrum operations
RGBSpectrum add(RGBSpectrum a, RGBSpectrum b)
{
    return RGBSpectrum(a.data + b.data);
}

RGBSpectrum sub(RGBSpectrum a, RGBSpectrum b)
{
    return RGBSpectrum(a.data - b.data);
}

RGBSpectrum mul(RGBSpectrum a, RGBSpectrum b)
{
    return RGBSpectrum(a.data * b.data);
}

RGBSpectrum mul(RGBSpectrum a, float scalar)
{
    return RGBSpectrum(a.data * scalar);
}

RGBSpectrum mul(float scalar, RGBSpectrum a)
{
    return RGBSpectrum(a.data * scalar);
}

RGBSpectrum div(RGBSpectrum a, float scalar)
{
    return RGBSpectrum(a.data / scalar);
}

// ============================================================================
// Sampled Spectrum Implementation (16-sample version with manual packing)
// Uses wavelength range 400nm-700nm (visible spectrum)
// Manually packs 2 half values into each uint for better control
// ============================================================================
struct SampledSpectrum : ISpectrum {
    static const int NUM_SAMPLES = 16;
    static const float LAMBDA_MIN = 400.0;  // nm
    static const float LAMBDA_MAX = 700.0;  // nm

    __init(float v)
    {
        uint packed = packHalf2x16(float2(v, v));
        for (int i = 0; i < NUM_SAMPLES / 2; ++i) {
            this.samples[i] = packed;
        }
    }

    __init(float3 rgb)
    {
        // Convert RGB to spectrum using improved reconstruction
        fromRGB(rgb);
    }

    uint samples[NUM_SAMPLES / 2];  // 8 uints = 32 bytes, stores 16 half values

    // Helper functions to get/set individual samples
    float getSample(int index)
    {
        int uintIndex = index / 2;
        float2 unpacked = f16tof32(uint2((samples[uintIndex] >> 0) & 0xFFFF, (samples[uintIndex] >> 16) & 0xFFFF));
        return (index % 2 == 0) ? unpacked.x : unpacked.y;
    }

    [mutating]
    void setSample(int index, float value)
    {
        int uintIndex = index / 2;
        uint2 current = uint2((samples[uintIndex] >> 0) & 0xFFFF, (samples[uintIndex] >> 16) & 0xFFFF);
        float2 unpacked = f16tof32(current);

        if (index % 2 == 0) {
            unpacked.x = value;
        } else {
            unpacked.y = value;
        }

        uint2 packed = f32tof16(unpacked);
        samples[uintIndex] = (packed.y << 16) | (packed.x & 0xFFFF);
    }

    [mutating]
    void fromRGB(float3 rgb)
    {
        // RGB to spectrum conversion using smooth basis functions
        // Based on "An RGB to Spectrum Conversion for Reflectances" by Smits (1999)
        // This uses smooth gaussian-like profiles for each RGB channel

        // Clamp input to prevent extreme values
        rgb = clamp(rgb, 0.0, 65504.0);  // Max value for half precision

        float lambdaStep = (LAMBDA_MAX - LAMBDA_MIN) / float(NUM_SAMPLES - 1);

        for (int i = 0; i < NUM_SAMPLES; ++i) {
            float lambda = LAMBDA_MIN + float(i) * lambdaStep;

            // Define smooth basis functions for R, G, B channels
            // These are designed to integrate to approximately 1.0 when converted back to RGB

            // Red: centered at 620nm, broader distribution
            float r_response = exp(-pow((lambda - 620.0) / 50.0, 2.0));

            // Green: centered at 540nm, medium width
            float g_response = exp(-pow((lambda - 540.0) / 40.0, 2.0));

            // Blue: centered at 450nm, narrower distribution
            float b_response = exp(-pow((lambda - 450.0) / 30.0, 2.0));

            // Linear combination of basis functions weighted by RGB values
            float value = rgb.r * r_response + rgb.g * g_response + rgb.b * b_response;

            // Clamp before storing
            setSample(i, clamp(value, 0.0, 65504.0));
        }
    }

    static float3 RGBtoXYZ(float3 rgb)
    {
        return rgbToXYZ_Rec709(rgb);
    }

    static float3 XYZtoRGB(float3 xyz)
    {
        return xyzToRGB_Rec709(xyz);
    }

    static float3 spectrumToXYZ(SampledSpectrum spec)
    {
        // Integrate spectrum against CIE XYZ color matching functions
        // Using trapezoidal rule for numerical integration
        float3 xyz = float3(0.0, 0.0, 0.0);
        float lambdaStep = (LAMBDA_MAX - LAMBDA_MIN) / float(NUM_SAMPLES - 1);

        for (int i = 0; i < NUM_SAMPLES; ++i) {
            float lambda = LAMBDA_MIN + float(i) * lambdaStep;
            float3 cmf = wavelengthToXYZ(lambda);

            // Trapezoidal rule weights (0.5 for endpoints, 1.0 for interior points)
            float weight = (i == 0 || i == NUM_SAMPLES - 1) ? 0.5 : 1.0;

            // Accumulate weighted by the spectrum value and wavelength step
            xyz += spec.getSample(i) * cmf * weight;
        }

        // Normalize by wavelength step and the sum of CMF Y values
        // This ensures unit spectrum (all 1.0) maps to white
        return xyz * lambdaStep / 106.856895f;
    }

    [mutating]
    void fromXYZ(float3 xyz)
    {
        // Convert XYZ to RGB first, then use RGB reconstruction
        float3 rgb = XYZtoRGB(xyz);
        fromRGB(rgb);
    }

    float3 toFloat3()
    {
        // Convert spectrum to XYZ, then to RGB
        float3 xyz = spectrumToXYZ(this);
        float3 rgb = XYZtoRGB(xyz);

        // Clamp to prevent invalid colors
        return clamp(rgb, 0.0, 65504.0);
    }
};

// Helper functions for SampledSpectrum operations
SampledSpectrum add(SampledSpectrum a, SampledSpectrum b)
{
    SampledSpectrum result = SampledSpectrum(0.0);
    for (int i = 0; i < 16; ++i) {
        result.setSample(i, a.getSample(i) + b.getSample(i));
    }
    return result;
}

SampledSpectrum sub(SampledSpectrum a, SampledSpectrum b)
{
    SampledSpectrum result = SampledSpectrum(0.0);
    for (int i = 0; i < 16; ++i) {
        result.setSample(i, a.getSample(i) - b.getSample(i));
    }
    return result;
}

SampledSpectrum mul(SampledSpectrum a, SampledSpectrum b)
{
    SampledSpectrum result = SampledSpectrum(0.0);
    for (int i = 0; i < 16; ++i) {
        result.setSample(i, a.getSample(i) * b.getSample(i));
    }
    return result;
}

SampledSpectrum mul(SampledSpectrum a, float scalar)
{
    SampledSpectrum result = SampledSpectrum(0.0);
    // Clamp scalar to prevent half overflow
    float safe_scalar = clamp(scalar, -65504.0, 65504.0);
    for (int i = 0; i < 16; ++i) {
        result.setSample(i, a.getSample(i) * safe_scalar);
    }
    return result;
}

SampledSpectrum mul(float scalar, SampledSpectrum a)
{
    return mul(a, scalar);
}

SampledSpectrum div(SampledSpectrum a, float scalar)
{
    SampledSpectrum result = SampledSpectrum(0.0);
    // Prevent division by zero and clamp
    float safe_scalar = max(abs(scalar), 1e-7) * sign(scalar);
    safe_scalar = clamp(safe_scalar, -65504.0, 65504.0);
    for (int i = 0; i < 16; ++i) {
        result.setSample(i, a.getSample(i) / safe_scalar);
    }
    return result;
}

// ============================================================================
// Spectrum Type Selection
// ============================================================================
#ifdef USE_SAMPLED_SPECTRUM
typedef SampledSpectrum Spectrum;
#else
typedef RGBSpectrum Spectrum;
#endif

#endif