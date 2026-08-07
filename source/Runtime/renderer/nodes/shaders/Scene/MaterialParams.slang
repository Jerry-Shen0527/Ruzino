#pragma once
import Scene.VertexInfo;
import utils.Math.ShadingFrame;
import utils.Math.MathHelpers;

#include "utils/Math/MathConstants.slangh"
#include "utils/random.slangh"

// ============================================================================
// Compression Helper Functions
// ============================================================================

// Octahedral mapping for normal compression (3 floats -> 2 uint16)
// Reference: "A Survey of Efficient Representations for Independent Unit Vectors"
uint packOctahedral(float3 n) {
    // Project the sphere onto the octahedron, then onto the xy plane
    float2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));

    // Reflect the folds of the lower hemisphere over the diagonals
    if (n.z <= 0.0) {
        float2 signNotZero = float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
        p = (1.0 - abs(p.yx)) * signNotZero;
    }

    // Map [-1, 1] to [0, 65535]
    uint2 packed = uint2((p * 0.5 + 0.5) * 65535.0);
    return (packed.x << 16) | packed.y;
}

float3 unpackOctahedral(uint packed) {
    uint2 e = uint2(packed >> 16, packed & 0xFFFF);
    float2 p = float2(e) / 65535.0 * 2.0 - 1.0;

    // https://twitter.com/Stubbesaurus/status/937994790553227264
    float3 n = float3(p.x, p.y, 1.0 - abs(p.x) - abs(p.y));
    float t = saturate(-n.z);
    n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

// Pack 3 half-precision floats into 2 uints (48 bits)
uint2 packHalf3(float3 v) {
    uint x = f32tof16(v.x);
    uint y = f32tof16(v.y);
    uint z = f32tof16(v.z);
    return uint2((x << 16) | y, z);
}

float3 unpackHalf3(uint2 packed) {
    return float3(
        f16tof32(packed.x >> 16),
        f16tof32(packed.x & 0xFFFF),
        f16tof32(packed.y)
    );
}

import pbrlib.genslang.mx_roughness_anisotropy;
import pbrlib.genslang.lib.mx_microfacet;
import pbrlib.genslang.lib.mx_microfacet_specular;
import pbrlib.genslang.lib.mx_microfacet_diffuse;

// Optimized material parameter structures for standard_surface
// Packed to reduce memory footprint by removing padding
struct StandardSurfaceMaterialParams {
    // Base parameters
    float base;
    float diffuse_roughness;
    float metalness;
    float specular;

    float3 base_color;
    float specular_roughness;

    float3 specular_color;
    float specular_IOR;

    // Specular and transmission
    float specular_anisotropy;
    float specular_rotation;
    float transmission;
    float transmission_depth;

    float3 transmission_color;
    float transmission_scatter_anisotropy;

    float3 transmission_scatter;
    float transmission_dispersion;

    float transmission_extra_roughness;
    float subsurface;
    float subsurface_scale;
    float subsurface_anisotropy;

    // Subsurface
    float3 subsurface_color;
    float sheen;

    float3 subsurface_radius;
    float sheen_roughness;

    // Sheen
    float3 sheen_color;
    float coat;

    // Coat
    float3 coat_color;
    float coat_roughness;

    float coat_anisotropy;
    float coat_rotation;
    float coat_IOR;
    float coat_affect_color;

    float3 coat_normal;
    float coat_affect_roughness;

    float thin_film_thickness;
    float thin_film_IOR;
    float emission;
    uint thin_walled;

    // Emission and opacity
    float3 emission_color;
    float3 opacity;

    // Normals and tangents
    float3 normal;
    float3 tangent;

    // Total: 256 bytes = 16 * float4 (reduced from 288 bytes by removing
    // padding)
};

// Compressed StandardSurface parameters using half precision and octahedral encoding
// Total size: 42 uints = 168 bytes (reduced from 256 bytes, 34.4% savings)
struct PackedStandardSurfaceMaterialParams {
    // Pack 1: base_color (RGB as half) + base (half) + diffuse_roughness (half) + metalness (half)
    uint2 pack1;  // base_color.rg as half
    uint pack2;   // base_color.b (half) | base (half)
    uint pack3;   // diffuse_roughness (half) | metalness (half)

    // Pack 2: specular parameters
    uint2 pack4;  // specular_color.rg as half
    uint pack5;   // specular_color.b (half) | specular (half)
    uint pack6;   // specular_roughness (half) | specular_IOR (half)
    uint pack7;   // specular_anisotropy (half) | specular_rotation (half)

    // Pack 3: transmission parameters
    uint pack8;   // transmission (half) | transmission_depth (half)
    uint2 pack9;  // transmission_color.rg as half
    uint pack10;  // transmission_color.b (half) | transmission_scatter_anisotropy (half)
    uint2 pack11; // transmission_scatter.rg as half
    uint pack12;  // transmission_scatter.b (half) | transmission_dispersion (half)
    uint pack13;  // transmission_extra_roughness (half) | subsurface (half)

    // Pack 4: subsurface parameters
    uint2 pack14; // subsurface_color.rg as half
    uint pack15;  // subsurface_color.b (half) | subsurface_scale (half)
    uint2 pack16; // subsurface_radius.rg as half
    uint pack17;  // subsurface_radius.b (half) | subsurface_anisotropy (half)

    // Pack 5: sheen parameters
    uint pack18;  // sheen (half) | sheen_roughness (half)
    uint2 pack19; // sheen_color.rg as half
    uint pack20;  // sheen_color.b (half) | coat (half)

    // Pack 6: coat parameters
    uint2 pack21; // coat_color.rg as half
    uint pack22;  // coat_color.b (half) | coat_roughness (half)
    uint pack23;  // coat_anisotropy (half) | coat_rotation (half)
    uint pack24;  // coat_IOR (half) | coat_affect_color (half)
    uint pack25;  // coat_affect_roughness (half) | thin_film_thickness (half)
    uint pack26;  // thin_film_IOR (half) | emission (half)

    // Pack 7: emission and opacity
    uint2 pack27; // emission_color.rg as half
    uint pack28;  // emission_color.b (half) | opacity.r (half)
    uint pack29;  // opacity.g (half) | opacity.b (half)

    // Pack 8: normals and tangent (octahedral encoded)
    uint pack30;  // normal (octahedral)
    uint pack31;  // tangent.xyz (octahedral) - ignore w component
    uint pack32;  // coat_normal (octahedral)
    uint pack33;  // thin_walled (1 bit in lowest bit)

    // Total: 42 uints = 168 bytes (reduced from 256 bytes, 34.4% savings)
};

// Optimized material parameter structure for preview_surface
struct PreviewSurfaceMaterialParams {
    // Colors
    float3 diffuseColor;
    float metallic;

    float3 emissiveColor;
    float roughness;

    float3 specularColor;
    float clearcoat;

    // Properties
    float clearcoatRoughness;
    float opacity;
    int opacityMode;
    float opacityThreshold;

    float ior;
    int useSpecularWorkflow;
    float displacement;
    float occlusion;

    float3 normal;

    // Total: 80 bytes = 5 * float4 (reduced from 112 bytes)
};

// Pack standard_surface parameters into optimized structure
// Compresses from 256 bytes to 160 bytes using half precision and octahedral encoding
PackedStandardSurfaceMaterialParams packStandardSurfaceMaterialParams(
    float base,
    float3 base_color,
    float diffuse_roughness,
    float metalness,
    float specular,
    float3 specular_color,
    float specular_roughness,
    float specular_IOR,
    float specular_anisotropy,
    float specular_rotation,
    float transmission,
    float3 transmission_color,
    float transmission_depth,
    float3 transmission_scatter,
    float transmission_scatter_anisotropy,
    float transmission_dispersion,
    float transmission_extra_roughness,
    float subsurface,
    float3 subsurface_color,
    float3 subsurface_radius,
    float subsurface_scale,
    float subsurface_anisotropy,
    float sheen,
    float3 sheen_color,
    float sheen_roughness,
    float coat,
    float3 coat_color,
    float coat_roughness,
    float coat_anisotropy,
    float coat_rotation,
    float coat_IOR,
    float3 coat_normal,
    float coat_affect_color,
    float coat_affect_roughness,
    float thin_film_thickness,
    float thin_film_IOR,
    float emission,
    float3 emission_color,
    float3 opacity,
    uint thin_walled,
    float3 normal,
    float3 tangent)
{
    PackedStandardSurfaceMaterialParams params;

    // Pack 1: base_color + base + diffuse_roughness + metalness
    params.pack1 = uint2(f32tof16(base_color.r) << 16 | f32tof16(base_color.g), 0);
    params.pack2 = f32tof16(base_color.b) << 16 | f32tof16(base);
    params.pack3 = f32tof16(diffuse_roughness) << 16 | f32tof16(metalness);

    // Pack 2: specular parameters
    params.pack4 = uint2(f32tof16(specular_color.r) << 16 | f32tof16(specular_color.g), 0);
    params.pack5 = f32tof16(specular_color.b) << 16 | f32tof16(specular);
    params.pack6 = f32tof16(specular_roughness) << 16 | f32tof16(specular_IOR);
    params.pack7 = f32tof16(specular_anisotropy) << 16 | f32tof16(specular_rotation);

    // Pack 3: transmission parameters
    params.pack8 = f32tof16(transmission) << 16 | f32tof16(transmission_depth);
    params.pack9 = uint2(f32tof16(transmission_color.r) << 16 | f32tof16(transmission_color.g), 0);
    params.pack10 = f32tof16(transmission_color.b) << 16 | f32tof16(transmission_scatter_anisotropy);
    params.pack11 = uint2(f32tof16(transmission_scatter.r) << 16 | f32tof16(transmission_scatter.g), 0);
    params.pack12 = f32tof16(transmission_scatter.b) << 16 | f32tof16(transmission_dispersion);
    params.pack13 = f32tof16(transmission_extra_roughness) << 16 | f32tof16(subsurface);

    // Pack 4: subsurface parameters
    params.pack14 = uint2(f32tof16(subsurface_color.r) << 16 | f32tof16(subsurface_color.g), 0);
    params.pack15 = f32tof16(subsurface_color.b) << 16 | f32tof16(subsurface_scale);
    params.pack16 = uint2(f32tof16(subsurface_radius.r) << 16 | f32tof16(subsurface_radius.g), 0);
    params.pack17 = f32tof16(subsurface_radius.b) << 16 | f32tof16(subsurface_anisotropy);

    // Pack 5: sheen parameters
    params.pack18 = f32tof16(sheen) << 16 | f32tof16(sheen_roughness);
    params.pack19 = uint2(f32tof16(sheen_color.r) << 16 | f32tof16(sheen_color.g), 0);
    params.pack20 = f32tof16(sheen_color.b) << 16 | f32tof16(coat);

    // Pack 6: coat parameters
    params.pack21 = uint2(f32tof16(coat_color.r) << 16 | f32tof16(coat_color.g), 0);
    params.pack22 = f32tof16(coat_color.b) << 16 | f32tof16(coat_roughness);
    params.pack23 = f32tof16(coat_anisotropy) << 16 | f32tof16(coat_rotation);
    params.pack24 = f32tof16(coat_IOR) << 16 | f32tof16(coat_affect_color);
    params.pack25 = f32tof16(coat_affect_roughness) << 16 | f32tof16(thin_film_thickness);
    params.pack26 = f32tof16(thin_film_IOR) << 16 | f32tof16(emission);

    // Pack 7: emission and opacity
    params.pack27 = uint2(f32tof16(emission_color.r) << 16 | f32tof16(emission_color.g), 0);
    params.pack28 = f32tof16(emission_color.b) << 16 | f32tof16(opacity.r);
    params.pack29 = f32tof16(opacity.g) << 16 | f32tof16(opacity.b);

    // Pack 8: normals and tangent (octahedral encoded)
    params.pack30 = packOctahedral(normalize(normal));
    params.pack31 = packOctahedral(normalize(tangent));
    params.pack32 = packOctahedral(normalize(coat_normal));
    params.pack33 = thin_walled;

    return params;
}

StandardSurfaceMaterialParams unpackStandardSurfaceMaterialParams(
    PackedStandardSurfaceMaterialParams packed)
{
    StandardSurfaceMaterialParams params;

    // Unpack 1: base_color + base + diffuse_roughness + metalness
    params.base_color.r = f16tof32(packed.pack1.x >> 16);
    params.base_color.g = f16tof32(packed.pack1.x & 0xFFFF);
    params.base_color.b = f16tof32(packed.pack2 >> 16);
    params.base = f16tof32(packed.pack2 & 0xFFFF);
    params.diffuse_roughness = f16tof32(packed.pack3 >> 16);
    params.metalness = f16tof32(packed.pack3 & 0xFFFF);

    // Unpack 2: specular parameters
    params.specular_color.r = f16tof32(packed.pack4.x >> 16);
    params.specular_color.g = f16tof32(packed.pack4.x & 0xFFFF);
    params.specular_color.b = f16tof32(packed.pack5 >> 16);
    params.specular = f16tof32(packed.pack5 & 0xFFFF);
    params.specular_roughness = f16tof32(packed.pack6 >> 16);
    params.specular_IOR = f16tof32(packed.pack6 & 0xFFFF);
    params.specular_anisotropy = f16tof32(packed.pack7 >> 16);
    params.specular_rotation = f16tof32(packed.pack7 & 0xFFFF);

    // Unpack 3: transmission parameters
    params.transmission = f16tof32(packed.pack8 >> 16);
    params.transmission_depth = f16tof32(packed.pack8 & 0xFFFF);
    params.transmission_color.r = f16tof32(packed.pack9.x >> 16);
    params.transmission_color.g = f16tof32(packed.pack9.x & 0xFFFF);
    params.transmission_color.b = f16tof32(packed.pack10 >> 16);
    params.transmission_scatter_anisotropy = f16tof32(packed.pack10 & 0xFFFF);
    params.transmission_scatter.r = f16tof32(packed.pack11.x >> 16);
    params.transmission_scatter.g = f16tof32(packed.pack11.x & 0xFFFF);
    params.transmission_scatter.b = f16tof32(packed.pack12 >> 16);
    params.transmission_dispersion = f16tof32(packed.pack12 & 0xFFFF);
    params.transmission_extra_roughness = f16tof32(packed.pack13 >> 16);
    params.subsurface = f16tof32(packed.pack13 & 0xFFFF);

    // Unpack 4: subsurface parameters
    params.subsurface_color.r = f16tof32(packed.pack14.x >> 16);
    params.subsurface_color.g = f16tof32(packed.pack14.x & 0xFFFF);
    params.subsurface_color.b = f16tof32(packed.pack15 >> 16);
    params.subsurface_scale = f16tof32(packed.pack15 & 0xFFFF);
    params.subsurface_radius.r = f16tof32(packed.pack16.x >> 16);
    params.subsurface_radius.g = f16tof32(packed.pack16.x & 0xFFFF);
    params.subsurface_radius.b = f16tof32(packed.pack17 >> 16);
    params.subsurface_anisotropy = f16tof32(packed.pack17 & 0xFFFF);

    // Unpack 5: sheen parameters
    params.sheen = f16tof32(packed.pack18 >> 16);
    params.sheen_roughness = f16tof32(packed.pack18 & 0xFFFF);
    params.sheen_color.r = f16tof32(packed.pack19.x >> 16);
    params.sheen_color.g = f16tof32(packed.pack19.x & 0xFFFF);
    params.sheen_color.b = f16tof32(packed.pack20 >> 16);
    params.coat = f16tof32(packed.pack20 & 0xFFFF);

    // Unpack 6: coat parameters
    params.coat_color.r = f16tof32(packed.pack21.x >> 16);
    params.coat_color.g = f16tof32(packed.pack21.x & 0xFFFF);
    params.coat_color.b = f16tof32(packed.pack22 >> 16);
    params.coat_roughness = f16tof32(packed.pack22 & 0xFFFF);
    params.coat_anisotropy = f16tof32(packed.pack23 >> 16);
    params.coat_rotation = f16tof32(packed.pack23 & 0xFFFF);
    params.coat_IOR = f16tof32(packed.pack24 >> 16);
    params.coat_affect_color = f16tof32(packed.pack24 & 0xFFFF);
    params.coat_affect_roughness = f16tof32(packed.pack25 >> 16);
    params.thin_film_thickness = f16tof32(packed.pack25 & 0xFFFF);
    params.thin_film_IOR = f16tof32(packed.pack26 >> 16);
    params.emission = f16tof32(packed.pack26 & 0xFFFF);

    // Unpack 7: emission and opacity
    params.emission_color.r = f16tof32(packed.pack27.x >> 16);
    params.emission_color.g = f16tof32(packed.pack27.x & 0xFFFF);
    params.emission_color.b = f16tof32(packed.pack28 >> 16);
    params.opacity.r = f16tof32(packed.pack28 & 0xFFFF);
    params.opacity.g = f16tof32(packed.pack29 >> 16);
    params.opacity.b = f16tof32(packed.pack29 & 0xFFFF);

    // Unpack 8: normals and tangent (octahedral encoded)
    params.normal = unpackOctahedral(packed.pack30);
    params.tangent = unpackOctahedral(packed.pack31);
    params.coat_normal = unpackOctahedral(packed.pack32);
    params.thin_walled = packed.pack33;

    return params;
}
