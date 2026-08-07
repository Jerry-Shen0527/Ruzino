import stdlib.genslang.mx_bsdf;
__exported import pbrlib.genslang.lib.mx_closure_type;
import pbrlib.genslang.lib.mx_transmission_refract;
import pbrlib.genslang.lib.mx_microfacet_specular;

void mx_dielectric_bsdf(
    ClosureData closureData,
    float weight,
    float3 tint,
    float ior,
    float2 roughness,
    float thinfilm_thickness,
    float thinfilm_ior,
    float3 N,
    float3 X,
    int distribution,
    int scatter_mode,
    inout BSDF bsdf)
{
    if (weight < M_FLOAT_EPS) {
        return;
    }
    if (closureData.closureType != CLOSURE_TYPE_TRANSMISSION &&
        scatter_mode == 1) {
        // This lobe is transmission-only but the closure is a reflection
        // evaluation: the lobe contributes neither response nor throughput.
        // Zero the throughput explicitly so downstream mx_mix_bsdf /
        // mx_layer_bsdf are not polluted by the BSDF default throughput (1.0)
        // — which would otherwise inflate the mixed result for refractive
        // materials evaluated in the reflection closure.
        bsdf.throughput = 0.0;
        bsdf.response = float3(0.0);
        return;
    }

    float3 V = closureData.V;
    float3 L = closureData.L;

    N = mx_forward_facing_normal(N, V);
    float NdotV = clamp(dot(N, V), M_FLOAT_EPS, 1.0);

    FresnelData fd =
        mx_init_fresnel_dielectric(ior, thinfilm_thickness, thinfilm_ior);
    float F0 = mx_ior_to_f0(ior);

    float2 safeAlpha = clamp(roughness, M_FLOAT_EPS, 1.0);
    float avgAlpha = mx_average_alpha(safeAlpha);
    float3 safeTint = max(tint, 0.0);

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

        // Multi-scatter GGX energy compensation is intentionally disabled here
        // (comp forced to 1.0). It was found to over-brighten in this pipeline;
        // re-enabling requires a separate validation pass. Keeping the dirAlbedo
        // throughput term (used so layered lobes below know how much energy this
        // lobe does NOT absorb), but without the comp multiplier.
        float3 dirAlbedo = mx_ggx_dir_albedo(NdotV, avgAlpha, F0, 1.0);
        bsdf.throughput = 1.0 - dirAlbedo * weight;

        bsdf.response = D * F * G * safeTint * closureData.occlusion *
                        weight / (4.0 * NdotV);
    }
    else if (closureData.closureType == CLOSURE_TYPE_TRANSMISSION) {
        X = normalize(X - dot(X, N) * N);

        if (dot(N, L) > 0.0) {
            bsdf.throughput = 0.0;
            bsdf.response = float3(0.0);
            return;
        }

        float3 F = mx_compute_fresnel(NdotV, fd);

        // Multi-scatter energy compensation disabled (see reflection branch above).
        float3 dirAlbedo = mx_ggx_dir_albedo(NdotV, avgAlpha, F0, 1.0);
        bsdf.throughput = 1.0 - dirAlbedo * weight;

        if (scatter_mode != 0) {
            bsdf.response =
                mx_surface_transmission(
                    N, V, L, X, safeAlpha, distribution, fd, safeTint) *
                weight;
        }
    }

}
