// Implementation of float16ToFloat32 / float32ToFloat16 for HOST_CODE.
// These are declared in Float16.h but never had a .cpp — they were unused
// until LightBVHBuilder brought in the packed node encoding. The conversions
// use standard IEEE 754 half-precision bit manipulation.
#include "../../nodes/shaders/utils/Math/Float16.h"
#include <cstring>

namespace Ruzino {
namespace math {

uint16_t float32ToFloat16(float value) {
    uint32_t x;
    std::memcpy(&x, &value, sizeof(float));
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t exponent = ((x >> 23) & 0xff) - 127 + 15;
    uint32_t mantissa = x & 0x7fffff;

    if (exponent <= 0) {
        if (exponent < -10) return (uint16_t)sign;
        mantissa |= 0x800000;
        uint32_t shift = 14 - exponent;
        uint16_t m = (uint16_t)(mantissa >> shift);
        return (uint16_t)(sign | m);
    }
    if (exponent == 0xff - (127 - 15)) {
        if (mantissa) return (uint16_t)(sign | 0x7e00);  // NaN
        return (uint16_t)(sign | 0x7c00);                // Inf
    }
    if (exponent > 30) return (uint16_t)(sign | 0x7c00);  // overflow
    return (uint16_t)(sign | (exponent << 10) | (mantissa >> 13));
}

float float16ToFloat32(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exponent = (h >> 10) & 0x1f;
    uint32_t mantissa = h & 0x3ff;
    uint32_t result;

    if (exponent == 0) {
        if (mantissa == 0) {
            result = sign << 31;
        } else {
            int e = -1;
            uint32_t m = mantissa;
            do { e++; m <<= 1; } while (!(m & 0x400));
            exponent = 127 - 15 - e;
            mantissa = (m & 0x3ff) << 13;
            result = (sign << 31) | (exponent << 23) | mantissa;
        }
    } else if (exponent == 0x1f) {
        result = (sign << 31) | (0xff << 23) | (mantissa << 13);
    } else {
        exponent += 127 - 15;
        result = (sign << 31) | (exponent << 23) | (mantissa << 13);
    }

    float f;
    std::memcpy(&f, &result, sizeof(float));
    return f;
}

}  // namespace math
}  // namespace Ruzino
