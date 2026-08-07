#pragma once
#include "SceneDefines.slangh"
#include "utils/HostDeviceShared.slangh"

#ifdef HOST_CODE
#include "utils/Math/PackedFormats.h"
using uint = uint32_t;
#endif

struct MaterialHeader {
    uint material_type_id;
    uint material_blob_id;
    uint padding[2];
};

struct MaterialDataBlob {
    uint data[256];
};

#ifndef HOST_CODE
StructuredBuffer<MaterialDataBlob> materialBlobBuffer : register(t3);
StructuredBuffer<MaterialHeader> materialHeaderBuffer : register(t4);
__exported import Scene.MaterialParamsBuffer;

[[vk::binding(1, 1)]]
Texture2D t_BindlessTextures[] : register(t0, space2);

// Sampler array for different addressing and filtering modes
// Index calculation: (uaddressmode * 3) + filtertype
// 0=wrap, 1=clamp, 2=mirror for address modes
// 0=point, 1=linear, 2=aniso for filter types
SamplerState samplers[9];

#endif