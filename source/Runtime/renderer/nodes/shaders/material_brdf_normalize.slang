// Normalize Sample Buffer to Texture
// Converts atomic accumulation to normalized float texture

RWStructuredBuffer<uint> sample_buffer;  // Input: [value, count] pairs stored as uint (float bits)
RWTexture2D<float4> sample_output;       // Output: normalized texture

struct NormalizeConstants {
    uint resolution;
    uint num_samples;
    uint min_samples_threshold;  // Minimum samples required (0 = no filtering)
    uint _pad;
};

ConstantBuffer<NormalizeConstants> normalize_params;

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadID.xy;
    uint resolution = normalize_params.resolution;

    if (any(pixel >= resolution)) {
        return;
    }

    uint pixel_index = pixel.y * resolution + pixel.x;

    // Read accumulated values (stored as uint, convert back to float)
    float value = asfloat(sample_buffer[pixel_index * 2 + 0]);
    float count = asfloat(sample_buffer[pixel_index * 2 + 1]);

    float output_value;

    if (normalize_params.min_samples_threshold == 0) {
        // Sample distribution mode: just copy raw values (normalization done in Python)
        output_value = value;
    } else {
        // Importance test mode: compute average weight with threshold filtering
        if (count >= float(8)) {
            output_value = value;
        } else {
            output_value = 0.0;  // Not enough samples
        }
    }

    // Output: RGB = value, A = count for debugging
    sample_output[pixel] = float4(output_value, output_value, output_value, count);
}
