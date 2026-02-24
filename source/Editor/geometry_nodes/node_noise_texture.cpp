#include <cmath>
#include <functional>
#include <memory>

#include "GCore/GOP.h"
#include "GCore/Texture/Noise.h"
#include "GCore/Texture/Texture.h"
#include "nodes/core/socket_trait.inl"
template<>
struct ValueTrait<glm::vec3> {
    static constexpr bool has_min = true;
    static constexpr bool has_max = true;
    static constexpr bool has_default = true;
};
template<>
struct ValueTrait<glm::vec2> {
    static constexpr bool has_min = true;
    static constexpr bool has_max = true;
    static constexpr bool has_default = true;
};
#include "geom_node_base.h"
#include "nodes/core/def/node_def.hpp"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(noise_texture_2d)
{
    b.add_input<glm::vec2>("Coordinate");
    b.add_input<float>("Scale").default_val(5.0f).min(0.01f).max(100.0f);
    b.add_input<int>("Seed").default_val(0).min(0).max(65535);
    b.add_input<int>("Noise Type").default_val(0).min(0).max(5);
    b.add_input<int>("Octaves").default_val(4).min(1).max(8);
    b.add_input<float>("Lacunarity").default_val(2.0f).min(1.0f).max(4.0f);
    b.add_input<float>("Gain").default_val(0.5f).min(0.0f).max(1.0f);
    b.add_input<bool>("Invert").default_val(false);
    b.add_output<float>("Value");
    b.add_output<glm::vec3>("Color");
    b.add_output<TextureHandle>("Texture");
}

NODE_EXECUTION_FUNCTION(noise_texture_2d)
{
    auto coord = params.get_input<glm::vec2>("Coordinate");
    float scale = params.get_input<float>("Scale");
    int seed = params.get_input<int>("Seed");
    int noise_type = params.get_input<int>("Noise Type");
    int octaves = params.get_input<int>("Octaves");
    float lacunarity = params.get_input<float>("Lacunarity");
    float gain = params.get_input<float>("Gain");
    bool invert = params.get_input<bool>("Invert");

    glm::vec2 p = coord * scale;
    float value = 0.0f;

    switch (noise_type) {
        case 0: value = Noise::value_noise_2d(p, seed); break;
        case 1: value = Noise::perlin_noise_2d(p, seed); break;
        case 2: value = Noise::simplex_noise_2d(p, seed); break;
        case 3:
            value = Noise::fbm_2d(p, octaves, lacunarity, gain, seed);
            break;
        case 4:
            value = Noise::turbulence_2d(p, octaves, lacunarity, gain, seed);
            break;
        case 5: value = Noise::voronoi_2d(p, seed); break;
        default: value = Noise::perlin_noise_2d(p, seed); break;
    }

    if (invert)
        value = 1.0f - value;
    value = glm::clamp(value, 0.0f, 1.0f);

    auto texture =
        std::make_shared<FunctionTexture2D>(FunctionTexture2D::VectorFunc(
            [scale, seed, noise_type, octaves, lacunarity, gain, invert](
                const glm::vec2& c) {
                glm::vec2 p = c * scale;
                float v = 0.0f;
                switch (noise_type) {
                    case 0: v = Noise::value_noise_2d(p, seed); break;
                    case 1: v = Noise::perlin_noise_2d(p, seed); break;
                    case 2: v = Noise::simplex_noise_2d(p, seed); break;
                    case 3:
                        v = Noise::fbm_2d(p, octaves, lacunarity, gain, seed);
                        break;
                    case 4:
                        v = Noise::turbulence_2d(
                            p, octaves, lacunarity, gain, seed);
                        break;
                    case 5: v = Noise::voronoi_2d(p, seed); break;
                    default: v = Noise::perlin_noise_2d(p, seed); break;
                }
                v = invert ? (1.0f - v) : v;
                return glm::vec3(v, v, v);
            }));

    params.set_output("Value", value);
    params.set_output("Color", glm::vec3(value, value, value));
    params.set_output("Texture", std::static_pointer_cast<Texture>(texture));
    return true;
}

NODE_DECLARATION_UI(noise_texture_2d);

NODE_DECLARATION_FUNCTION(noise_texture_3d)
{
    b.add_input<glm::vec3>("Coordinate");
    b.add_input<float>("Scale").default_val(5.0f).min(0.01f).max(100.0f);
    b.add_input<int>("Seed").default_val(0).min(0).max(65535);
    b.add_input<int>("Noise Type").default_val(0).min(0).max(5);
    b.add_input<int>("Octaves").default_val(4).min(1).max(8);
    b.add_input<float>("Lacunarity").default_val(2.0f).min(1.0f).max(4.0f);
    b.add_input<float>("Gain").default_val(0.5f).min(0.0f).max(1.0f);
    b.add_input<bool>("Invert").default_val(false);
    b.add_output<float>("Value");
    b.add_output<glm::vec3>("Color");
    b.add_output<TextureHandle>("Texture");
}

NODE_EXECUTION_FUNCTION(noise_texture_3d)
{
    auto coord = params.get_input<glm::vec3>("Coordinate");
    float scale = params.get_input<float>("Scale");
    int seed = params.get_input<int>("Seed");
    int noise_type = params.get_input<int>("Noise Type");
    int octaves = params.get_input<int>("Octaves");
    float lacunarity = params.get_input<float>("Lacunarity");
    float gain = params.get_input<float>("Gain");
    bool invert = params.get_input<bool>("Invert");

    glm::vec3 p = coord * scale;
    float value = 0.0f;

    switch (noise_type) {
        case 0: value = Noise::value_noise_3d(p, seed); break;
        case 1: value = Noise::perlin_noise_3d(p, seed); break;
        case 2:
            value = Noise::fbm_3d(p, octaves, lacunarity, gain, seed);
            break;
        case 3:
            value = Noise::turbulence_3d(p, octaves, lacunarity, gain, seed);
            break;
        case 4: value = Noise::voronoi_3d(p, seed); break;
        case 5:
            value = Noise::ridged_multifractal_3d(
                p, octaves, lacunarity, gain, 1.0f, seed);
            break;
        default: value = Noise::perlin_noise_3d(p, seed); break;
    }

    if (invert)
        value = 1.0f - value;
    value = glm::clamp(value, 0.0f, 1.0f);

    auto texture =
        std::make_shared<FunctionTexture3D>(FunctionTexture3D::VectorFunc(
            [scale, seed, noise_type, octaves, lacunarity, gain, invert](
                const glm::vec3& c) {
                glm::vec3 p = c * scale;
                float v = 0.0f;
                switch (noise_type) {
                    case 0: v = Noise::value_noise_3d(p, seed); break;
                    case 1: v = Noise::perlin_noise_3d(p, seed); break;
                    case 2:
                        v = Noise::fbm_3d(p, octaves, lacunarity, gain, seed);
                        break;
                    case 3:
                        v = Noise::turbulence_3d(
                            p, octaves, lacunarity, gain, seed);
                        break;
                    case 4: v = Noise::voronoi_3d(p, seed); break;
                    case 5:
                        v = Noise::ridged_multifractal_3d(
                            p, octaves, lacunarity, gain, 1.0f, seed);
                        break;
                    default: v = Noise::perlin_noise_3d(p, seed); break;
                }
                v = invert ? (1.0f - v) : v;
                return glm::vec3(v, v, v);
            }));

    params.set_output("Value", value);
    params.set_output("Color", glm::vec3(value, value, value));
    params.set_output("Texture", std::static_pointer_cast<Texture>(texture));
    return true;
}

NODE_DECLARATION_UI(noise_texture_3d);

NODE_DECLARATION_FUNCTION(sample_texture)
{
    b.add_input<TextureHandle>("Texture");
    b.add_input<glm::vec3>("Coordinate");
    b.add_output<float>("Value");
    b.add_output<glm::vec3>("Color");
    b.add_output<glm::vec4>("RGBA");
}

NODE_EXECUTION_FUNCTION(sample_texture)
{
    auto texture = params.get_input<TextureHandle>("Texture");
    auto coord = params.get_input<glm::vec3>("Coordinate");

    if (!texture) {
        params.set_output("Value", 0.0f);
        params.set_output("Color", glm::vec3(0.0f));
        params.set_output("RGBA", glm::vec4(0.0f));
        return false;
    }

    float value = 0.0f;
    glm::vec3 color(0.0f);
    glm::vec4 rgba(0.0f);

    switch (texture->get_dimension()) {
        case TextureDimension::Tex2D: {
            glm::vec2 c(coord.x, coord.y);
            value = texture->sample_scalar_2d(c);
            color = texture->sample_vector_2d(c);
            rgba = texture->sample_rgba_2d(c);
            break;
        }
        case TextureDimension::Tex3D: {
            value = texture->sample_scalar_3d(coord);
            color = texture->sample_vector_3d(coord);
            rgba = texture->sample_rgba_3d(coord);
            break;
        }
        default: break;
    }

    params.set_output("Value", value);
    params.set_output("Color", color);
    params.set_output("RGBA", rgba);
    return true;
}

NODE_DECLARATION_UI(sample_texture);

NODE_DECLARATION_FUNCTION(clone_texture)
{
    b.add_input<TextureHandle>("Texture");
    b.add_output<TextureHandle>("Texture Copy");
}

NODE_EXECUTION_FUNCTION(clone_texture)
{
    auto texture = params.get_input<TextureHandle>("Texture");

    if (!texture) {
        params.set_output("Texture Copy", nullptr);
        return false;
    }

    auto cloned = texture->clone();
    params.set_output("Texture Copy", cloned);
    return true;
}

NODE_DECLARATION_UI(clone_texture);

NODE_DEF_CLOSE_SCOPE
