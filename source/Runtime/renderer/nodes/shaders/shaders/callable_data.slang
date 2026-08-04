import Scene.SceneTypes;
import utils.Spectrum;

struct FetchCallableData {
    uint materialBlobID;
    uint material_params_index; // Index into material parameters buffer, set by data fetch callable
    uint shader_type_id;
    VertexInfo vertexInfo;
    float3 opacityColor; // Color3 opacity for tinted presence pass-through (set by opacity fetch callable)
};

struct EvalCallableData {
    Spectrum color;          // Using typedef Spectrum (RGBSpectrum or SampledSpectrum)
    Spectrum throughput;     // Using typedef Spectrum (RGBSpectrum or SampledSpectrum)
    float3 L;
    float3 V;
    float3 sampledDir;
    float pdf;
    uint seed;
    uint eta_flipped;
    uint material_params_index; // Index into material parameters buffer, set by data fetch callable
    VertexInfo vertexInfo;
};

struct DomeLightCallableData {
    float3 rayDirection;     // Ray direction in world space
    float3 localDirection;   // Ray direction in dome light local space
    Spectrum envColor;       // Output environment color (using typedef Spectrum)
    float3 intensity;        // Dome light intensity
    int textureIndex;        // Bindless texture index
    uint seed;               // Random seed for sampling
};

struct OpacityTestCallableData {
    uint materialBlobID;
    uint material_params_index; // Index into material parameters buffer
    bool isOpaque;              // Output: whether the surface is opaque
    VertexInfo vertexInfo;  // Vertex information for texture sampling if needed
    uint seed;
};
