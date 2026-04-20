import pbrlib.genslang.lib.mx_microfacet_specular;
import stdlib.genslang.mx_bsdf;
import pbrlib.genslang.lib.mx_closure_type;

void mx_conductor_bsdf(
    ClosureData closureData,
    float weight,
    float3 ior_n,
    float3 ior_k,
    float2 roughness,
    float thinfilm_thickness,
    float thinfilm_ior,
    float3 N,
    float3 X,
    int distribution,
    inout BSDF bsdf)
{
    bsdf.throughput = float3(0.0);

    if (weight < M_FLOAT_EPS) {
        return;
    }

    float3 V = closureData.V;
    float3 L = closureData.L;

    N = mx_forward_facing_normal(N, V);
    float NdotV = clamp(dot(N, V), M_FLOAT_EPS, 1.0);

    FresnelData fd = mx_init_fresnel_conductor(
        ior_n, ior_k, thinfilm_thickness, thinfilm_ior);
    float2 safeAlpha = clamp(roughness, M_FLOAT_EPS, 1.0);
    float avgAlpha = mx_average_alpha(safeAlpha);

    if (closureData.closureType == CLOSURE_TYPE_REFLECTION) {
        if (dot(N, L) < 0.0) {
            bsdf.throughput = 0.0;
            bsdf.response = float3(0.0);
            return;
        }

        X = normalize(X - dot(X, N) * N);
        float3 Y = cross(N, X);
        float3 H = normalize(L + V);

        float NdotL = clamp(dot(N, L), M_FLOAT_EPS, 1.0);
        float VdotH = clamp(dot(V, H), M_FLOAT_EPS, 1.0);

        float3 Ht = float3(dot(H, X), dot(H, Y), dot(H, N));

        float3 F = mx_compute_fresnel(VdotH, fd);
        float D = mx_ggx_NDF(Ht, safeAlpha);
        float G = mx_ggx_smith_G2(NdotL, NdotV, avgAlpha);

        float3 comp = mx_ggx_energy_compensation(NdotV, avgAlpha, F);

        bsdf.response = D * F * G * comp * closureData.occlusion * weight /
                        (4.0 * NdotV);
    }
}
