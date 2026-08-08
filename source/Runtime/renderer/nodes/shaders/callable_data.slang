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
    // BSDF pdf for the NEE light direction L (probability that this BSDF sampler
    // would have generated L). Used by MIS balance heuristic on the NEE path:
    // w_light = light_pdf(L) / (light_pdf(L) + bsdf_pdf(L)). The symmetric term
    // bsdf_pdf(sampledDir) is just `pdf` above (carried via RayPayload.bsdfPdf).
    float bsdfPdfForLightDir;
};

struct DomeLightCallableData {
    float3 rayDirection;     // Ray direction in world space
    float3 localDirection;   // Ray direction in dome light local space
    Spectrum envColor;       // Output environment color (using typedef Spectrum)
    float3 intensity;        // Dome light intensity
    int textureIndex;        // Bindless texture index
    uint seed;               // Random seed for sampling

    // Hosek-Wilkie analytic sky parameters (set by the dome miss handler when
    // the dome's shader_path points at eval_dome_light_hosek_wilkie.slang).
    // sunDirLocal is the sun direction in the dome's LOCAL space (+Y up),
    // derived from the scene's DistantLight so the sun and sky stay aligned.
    uint   hosekStateIndex;  // Index into hosekStateBuffer (0 if not a Hosek sky)
    float3 sunDirLocal;      // Sun direction in dome-local space (+Y up)
};

struct OpacityTestCallableData {
    uint materialBlobID;
    uint material_params_index; // Index into material parameters buffer
    bool isOpaque;              // Output: whether the surface is opaque
    VertexInfo vertexInfo;  // Vertex information for texture sampling if needed
    uint seed;
};
