#ifdef HOST_CODE
#include "utils/Math/PackedFormats.h"
using uint = uint32_t;
#endif

struct MaterialParams {
    uint data[42];
};

#ifndef HOST_CODE
RWStructuredBuffer<MaterialParams> materialParamsBuffer;
#endif