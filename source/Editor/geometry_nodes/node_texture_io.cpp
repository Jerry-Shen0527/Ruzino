#include <cmath>
#include <filesystem>
#include <memory>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <stb_image.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "GCore/GOP.h"
#include "GCore/Texture/Texture.h"
#include "geom_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(load_texture_2d)
{
    b.add_input<std::string>("Path").default_val("");
    b.add_input<int>("Wrap Mode").default_val(0).min(0).max(2);
    b.add_output<TextureHandle>("Texture");
    b.add_output<int>("Width");
    b.add_output<int>("Height");
    b.add_output<int>("Channels");
}

NODE_EXECUTION_FUNCTION(load_texture_2d)
{
    auto path_str = params.get_input<std::string>("Path");
    int wrap_mode = params.get_input<int>("Wrap Mode");

    std::filesystem::path executable_path;

#ifdef _WIN32
    char p[MAX_PATH];
    GetModuleFileNameA(NULL, p, MAX_PATH);
    executable_path = std::filesystem::path(p).parent_path();
#else
    char p[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", p, PATH_MAX);
    if (count != -1) {
        p[count] = '\0';
        executable_path = std::filesystem::path(p).parent_path();
    }
    else {
        throw std::runtime_error("Failed to get executable path.");
    }
#endif

    if (path_str.empty()) {
        spdlog::warn("Texture path is empty");
        params.set_output("Texture", nullptr);
        params.set_output("Width", 0);
        params.set_output("Height", 0);
        params.set_output("Channels", 0);
        return false;
    }

    std::filesystem::path texture_path(path_str);
    if (!texture_path.is_absolute()) {
        texture_path = executable_path / texture_path;
    }
    texture_path = texture_path.lexically_normal();

    if (!std::filesystem::exists(texture_path)) {
        spdlog::warn("Texture file not found: {}", texture_path.string());
        params.set_output("Texture", nullptr);
        params.set_output("Width", 0);
        params.set_output("Height", 0);
        params.set_output("Channels", 0);
        return false;
    }

    int width, height, channels;
    stbi_set_flip_vertically_on_load(true);
    float* data = stbi_loadf(
        texture_path.string().c_str(), &width, &height, &channels, 4);

    if (!data) {
        spdlog::error("Failed to load texture: {}", texture_path.string());
        params.set_output("Texture", nullptr);
        params.set_output("Width", 0);
        params.set_output("Height", 0);
        params.set_output("Channels", 0);
        return false;
    }

    auto texture = std::make_shared<DataTexture2D>();
    texture->set_wrap_mode(static_cast<DataTexture2D::WrapMode>(wrap_mode));
    texture->resize(width, height);

    std::vector<float> texture_data(width * height * 4);
    std::copy(data, data + width * height * 4, texture_data.begin());
    texture->set_data(texture_data);

    stbi_image_free(data);

    spdlog::info("Loaded texture: {}x{}, {} channels", width, height, channels);

    params.set_output("Texture", std::static_pointer_cast<Texture>(texture));
    params.set_output("Width", width);
    params.set_output("Height", height);
    params.set_output("Channels", channels);
    return true;
}

NODE_DECLARATION_UI(load_texture_2d);

NODE_DECLARATION_FUNCTION(create_texture_2d)
{
    b.add_input<int>("Width").default_val(256).min(1).max(8192);
    b.add_input<int>("Height").default_val(256).min(1).max(8192);
    b.add_input<int>("Wrap Mode").default_val(0).min(0).max(2);
    b.add_input<float>("R").default_val(0.0f).min(0.0f).max(1.0f);
    b.add_input<float>("G").default_val(0.0f).min(0.0f).max(1.0f);
    b.add_input<float>("B").default_val(0.0f).min(0.0f).max(1.0f);
    b.add_input<float>("A").default_val(1.0f).min(0.0f).max(1.0f);
    b.add_output<TextureHandle>("Texture");
}

NODE_EXECUTION_FUNCTION(create_texture_2d)
{
    int width = params.get_input<int>("Width");
    int height = params.get_input<int>("Height");
    int wrap_mode = params.get_input<int>("Wrap Mode");
    float r = params.get_input<float>("R");
    float g = params.get_input<float>("G");
    float b = params.get_input<float>("B");
    float a = params.get_input<float>("A");

    auto texture = std::make_shared<DataTexture2D>();
    texture->set_wrap_mode(static_cast<DataTexture2D::WrapMode>(wrap_mode));
    texture->resize(width, height);

    std::vector<float> data(width * height * 4);
    for (int i = 0; i < width * height; ++i) {
        data[i * 4 + 0] = r;
        data[i * 4 + 1] = g;
        data[i * 4 + 2] = b;
        data[i * 4 + 3] = a;
    }
    texture->set_data(data);

    params.set_output("Texture", std::static_pointer_cast<Texture>(texture));
    return true;
}

NODE_DECLARATION_UI(create_texture_2d);

NODE_DECLARATION_FUNCTION(create_texture_3d)
{
    b.add_input<int>("Width").default_val(64).min(1).max(512);
    b.add_input<int>("Height").default_val(64).min(1).max(512);
    b.add_input<int>("Depth").default_val(64).min(1).max(512);
    b.add_input<int>("Wrap Mode").default_val(0).min(0).max(2);
    b.add_input<float>("R").default_val(0.0f).min(0.0f).max(1.0f);
    b.add_input<float>("G").default_val(0.0f).min(0.0f).max(1.0f);
    b.add_input<float>("B").default_val(0.0f).min(0.0f).max(1.0f);
    b.add_input<float>("A").default_val(1.0f).min(0.0f).max(1.0f);
    b.add_output<TextureHandle>("Texture");
}

NODE_EXECUTION_FUNCTION(create_texture_3d)
{
    int width = params.get_input<int>("Width");
    int height = params.get_input<int>("Height");
    int depth = params.get_input<int>("Depth");
    int wrap_mode = params.get_input<int>("Wrap Mode");
    float r = params.get_input<float>("R");
    float g = params.get_input<float>("G");
    float b = params.get_input<float>("B");
    float a = params.get_input<float>("A");

    auto texture = std::make_shared<DataTexture3D>();
    texture->set_wrap_mode(static_cast<DataTexture3D::WrapMode>(wrap_mode));
    texture->resize(width, height, depth);

    std::vector<float> data(width * height * depth * 4);
    for (int i = 0; i < width * height * depth; ++i) {
        data[i * 4 + 0] = r;
        data[i * 4 + 1] = g;
        data[i * 4 + 2] = b;
        data[i * 4 + 3] = a;
    }
    texture->set_data(data);

    params.set_output("Texture", std::static_pointer_cast<Texture>(texture));
    return true;
}

NODE_DECLARATION_UI(create_texture_3d);

NODE_DECLARATION_FUNCTION(function_texture_2d)
{
    b.add_input<std::string>("Expression R").default_val("sin(x*6.28)*0.5+0.5");
    b.add_input<std::string>("Expression G").default_val("sin(y*6.28)*0.5+0.5");
    b.add_input<std::string>("Expression B").default_val("0.5");
    b.add_output<TextureHandle>("Texture");
}

NODE_EXECUTION_FUNCTION(function_texture_2d)
{
    auto expr_r = params.get_input<std::string>("Expression R");
    auto expr_g = params.get_input<std::string>("Expression G");
    auto expr_b = params.get_input<std::string>("Expression B");

    auto eval_expr = [](const std::string& expr, float x, float y) -> float {
        if (expr == "x")
            return x;
        if (expr == "y")
            return y;
        if (expr == "0" || expr == "0.0")
            return 0.0f;
        if (expr == "1" || expr == "1.0")
            return 1.0f;
        if (expr == "0.5")
            return 0.5f;

        float result = 0.0f;
        if (expr.find("sin(x*6.28)*0.5+0.5") != std::string::npos) {
            result = std::sin(x * 6.28f) * 0.5f + 0.5f;
        }
        else if (expr.find("sin(y*6.28)*0.5+0.5") != std::string::npos) {
            result = std::sin(y * 6.28f) * 0.5f + 0.5f;
        }
        else if (expr.find("sin(x*3.14)") != std::string::npos) {
            result = std::sin(x * 3.14f);
        }
        else if (expr.find("cos(x*3.14)") != std::string::npos) {
            result = std::cos(x * 3.14f);
        }
        else if (expr.find("x") != std::string::npos) {
            result = x;
        }
        else if (expr.find("y") != std::string::npos) {
            result = y;
        }
        else {
            try {
                result = std::stof(expr);
            }
            catch (...) {
                result = 0.0f;
            }
        }
        return glm::clamp(result, 0.0f, 1.0f);
    };

    auto texture =
        std::make_shared<FunctionTexture2D>(FunctionTexture2D::VectorFunc(
            [expr_r, expr_g, expr_b, eval_expr](
                const glm::vec2& coord) -> glm::vec3 {
                float r = eval_expr(expr_r, coord.x, coord.y);
                float g = eval_expr(expr_g, coord.x, coord.y);
                float b = eval_expr(expr_b, coord.x, coord.y);
                return glm::vec3(r, g, b);
            }));

    params.set_output("Texture", std::static_pointer_cast<Texture>(texture));
    return true;
}

NODE_DECLARATION_UI(function_texture_2d);

NODE_DECLARATION_FUNCTION(function_texture_3d)
{
    b.add_input<std::string>("Expression R").default_val("x");
    b.add_input<std::string>("Expression G").default_val("y");
    b.add_input<std::string>("Expression B").default_val("z");
    b.add_output<TextureHandle>("Texture");
}

NODE_EXECUTION_FUNCTION(function_texture_3d)
{
    auto expr_r = params.get_input<std::string>("Expression R");
    auto expr_g = params.get_input<std::string>("Expression G");
    auto expr_b = params.get_input<std::string>("Expression B");

    auto eval_expr =
        [](const std::string& expr, float x, float y, float z) -> float {
        if (expr == "x")
            return x;
        if (expr == "y")
            return y;
        if (expr == "z")
            return z;
        if (expr == "0" || expr == "0.0")
            return 0.0f;
        if (expr == "1" || expr == "1.0")
            return 1.0f;
        if (expr == "0.5")
            return 0.5f;

        float result = 0.0f;
        try {
            result = std::stof(expr);
        }
        catch (...) {
            result = 0.0f;
        }
        return glm::clamp(result, 0.0f, 1.0f);
    };

    auto texture =
        std::make_shared<FunctionTexture3D>(FunctionTexture3D::VectorFunc(
            [expr_r, expr_g, expr_b, eval_expr](
                const glm::vec3& coord) -> glm::vec3 {
                float r = eval_expr(expr_r, coord.x, coord.y, coord.z);
                float g = eval_expr(expr_g, coord.x, coord.y, coord.z);
                float b = eval_expr(expr_b, coord.x, coord.y, coord.z);
                return glm::vec3(r, g, b);
            }));

    params.set_output("Texture", std::static_pointer_cast<Texture>(texture));
    return true;
}

NODE_DECLARATION_UI(function_texture_3d);

NODE_DEF_CLOSE_SCOPE
