#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <glm/glm.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ds {

// Trim whitespace from left
inline void ltrim(std::string_view& s)
{
    auto it = std::find_if(
        s.begin(), s.end(), [](int ch) { return !std::isspace(ch); });
    s.remove_prefix(std::distance(s.begin(), it));
}

// Trim whitespace from right
inline void rtrim(std::string_view& s)
{
    auto it = std::find_if(
        s.rbegin(), s.rend(), [](int ch) { return !std::isspace(ch); });
    s.remove_suffix(std::distance(s.rbegin(), it));
}

// Trim whitespace from both ends
inline void trim(std::string_view& s)
{
    ltrim(s);
    rtrim(s);
}

// Trim whitespace from both ends (string version)
inline void trim(std::string& s)
{
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) {
                return !std::isspace(ch);
            }));
    s.erase(
        std::find_if(
            s.rbegin(), s.rend(), [](int ch) { return !std::isspace(ch); })
            .base(),
        s.end());
}

// Split string by delimiter
inline std::vector<std::string_view> split(
    std::string_view str,
    const char* delims = " \t\n\r")
{
    std::vector<std::string_view> result;

    size_t start = 0;
    while (start < str.length()) {
        // Skip leading delimiters
        while (start < str.length() && std::strchr(delims, str[start])) {
            ++start;
        }

        if (start >= str.length())
            break;

        // Find end of token
        size_t end = start;
        while (end < str.length() && !std::strchr(delims, str[end])) {
            ++end;
        }

        if (end > start) {
            result.emplace_back(str.substr(start, end - start));
        }

        start = end;
    }

    return result;
}

// Helper function to parse numeric types
template<typename T>
std::optional<T> parse_numeric(std::string_view str)
{
    T value;
    auto result = std::from_chars(str.data(), str.data() + str.size(), value);
    if (result.ec == std::errc{}) {
        return value;
    }
    return std::nullopt;
}

// Helper function to parse float from string
inline std::optional<float> parse_float(std::string_view str)
{
    float value;
    auto result = std::from_chars(str.data(), str.data() + str.size(), value);
    if (result.ec == std::errc{}) {
        return value;
    }
    // Fallback to strtof for better compatibility
    try {
        std::string s(str);
        char* end;
        float f = std::strtof(s.c_str(), &end);
        if (end != s.c_str() && *end == '\0') {
            return f;
        }
    }
    catch (...) {
    }
    return std::nullopt;
}

// Main parse function template - generic version that fails for unsupported
// types
template<typename T>
std::optional<T> parse(std::string_view str)
{
    static_assert(
        !std::is_same_v<T, T>, "parse() not implemented for this type");
    return std::nullopt;
}

// Specializations for basic types
template<>
inline std::optional<bool> parse<bool>(std::string_view str)
{
    if (str == "true" || str == "1")
        return true;
    if (str == "false" || str == "0")
        return false;
    return std::nullopt;
}

template<>
inline std::optional<int> parse<int>(std::string_view str)
{
    return parse_numeric<int>(str);
}

template<>
inline std::optional<float> parse<float>(std::string_view str)
{
    return parse_float(str);
}

template<>
inline std::optional<std::string> parse<std::string>(std::string_view str)
{
    return std::string(str);
}

// Specializations for GLM types
template<>
inline std::optional<glm::vec2> parse<glm::vec2>(std::string_view str)
{
    auto tokens = split(str);
    if (tokens.size() == 2) {
        auto x = parse_float(tokens[0]);
        auto y = parse_float(tokens[1]);
        if (x && y) {
            return glm::vec2(*x, *y);
        }
    }
    return std::nullopt;
}

template<>
inline std::optional<glm::vec3> parse<glm::vec3>(std::string_view str)
{
    auto tokens = split(str);
    if (tokens.size() == 3) {
        auto x = parse_float(tokens[0]);
        auto y = parse_float(tokens[1]);
        auto z = parse_float(tokens[2]);
        if (x && y && z) {
            return glm::vec3(*x, *y, *z);
        }
    }
    return std::nullopt;
}

template<>
inline std::optional<glm::vec4> parse<glm::vec4>(std::string_view str)
{
    auto tokens = split(str);
    if (tokens.size() == 4) {
        auto x = parse_float(tokens[0]);
        auto y = parse_float(tokens[1]);
        auto z = parse_float(tokens[2]);
        auto w = parse_float(tokens[3]);
        if (x && y && z && w) {
            return glm::vec4(*x, *y, *z, *w);
        }
    }
    return std::nullopt;
}

template<>
inline std::optional<glm::ivec2> parse<glm::ivec2>(std::string_view str)
{
    auto tokens = split(str);
    if (tokens.size() == 2) {
        auto x = parse_numeric<int>(tokens[0]);
        auto y = parse_numeric<int>(tokens[1]);
        if (x && y) {
            return glm::ivec2(*x, *y);
        }
    }
    return std::nullopt;
}

template<>
inline std::optional<glm::ivec3> parse<glm::ivec3>(std::string_view str)
{
    auto tokens = split(str);
    if (tokens.size() == 3) {
        auto x = parse_numeric<int>(tokens[0]);
        auto y = parse_numeric<int>(tokens[1]);
        auto z = parse_numeric<int>(tokens[2]);
        if (x && y && z) {
            return glm::ivec3(*x, *y, *z);
        }
    }
    return std::nullopt;
}

}  // namespace ds
