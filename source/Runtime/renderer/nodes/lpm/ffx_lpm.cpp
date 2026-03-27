// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include <stdlib.h>  // for _countof
#include <string.h>  // for memset

#include <cmath>   // for fabs, abs, sinf, sqrt, etc.
#include <cwchar>  // for wcscmp

// Linux compatibility: _countof is MSVC-specific
#ifndef _countof
#define _countof(arr) (sizeof(arr) / sizeof(arr[0]))
#endif

#include "ffx_lpm_host.h"

#define FFX_CPU
#include "ffx_core.h"

static float fs2S;
static float hdr10S;
static uint32_t ctl[24 * 4];

static void LpmSetupOut(uint32_t i, uint32_t* v)
{
    for (int j = 0; j < 4; ++j) {
        ctl[i * 4 + j] = v[j];
    }
}
#include "ffx_lpm.h"
#include "ffx_lpm_private.h"

// lists to map shader resource bindpoint name to resource identifier
typedef struct ResourceBinding {
    uint32_t index;
    wchar_t name[64];
} ResourceBinding;

static const ResourceBinding srvTextureBindingTable[] = {
    { FFX_LPM_RESOURCE_IDENTIFIER_INPUT_COLOR, L"r_input_color" },
};

static const ResourceBinding uavTextureBindingTable[] = {
    { FFX_LPM_RESOURCE_IDENTIFIER_OUTPUT_COLOR, L"rw_output_color" },
};

static const ResourceBinding cbResourceBindingTable[] = {
    { FFX_LPM_CONSTANTBUFFER_IDENTIFIER_LPM, L"cbLPM" },
};

static FfxErrorCode patchResourceBindings(FfxPipelineState* inoutPipeline)
{
    for (uint32_t srvIndex = 0; srvIndex < inoutPipeline->srvTextureCount;
         ++srvIndex) {
        int32_t mapIndex = 0;
        for (mapIndex = 0; mapIndex < _countof(srvTextureBindingTable);
             ++mapIndex) {
            if (0 == wcscmp(
                         srvTextureBindingTable[mapIndex].name,
                         inoutPipeline->srvTextureBindings[srvIndex].name))
                break;
        }
        if (mapIndex == _countof(srvTextureBindingTable))
            return FFX_ERROR_INVALID_ARGUMENT;

        inoutPipeline->srvTextureBindings[srvIndex].resourceIdentifier =
            srvTextureBindingTable[mapIndex].index;
    }

    for (uint32_t uavIndex = 0; uavIndex < inoutPipeline->uavTextureCount;
         ++uavIndex) {
        int32_t mapIndex = 0;
        for (mapIndex = 0; mapIndex < _countof(uavTextureBindingTable);
             ++mapIndex) {
            if (0 == wcscmp(
                         uavTextureBindingTable[mapIndex].name,
                         inoutPipeline->uavTextureBindings[uavIndex].name))
                break;
        }
        if (mapIndex == _countof(uavTextureBindingTable))
            return FFX_ERROR_INVALID_ARGUMENT;

        inoutPipeline->uavTextureBindings[uavIndex].resourceIdentifier =
            uavTextureBindingTable[mapIndex].index;
    }

    for (uint32_t cbIndex = 0; cbIndex < inoutPipeline->constCount; ++cbIndex) {
        int32_t mapIndex = 0;
        for (mapIndex = 0; mapIndex < _countof(cbResourceBindingTable);
             ++mapIndex) {
            if (0 == wcscmp(
                         cbResourceBindingTable[mapIndex].name,
                         inoutPipeline->constantBufferBindings[cbIndex].name))
                break;
        }
        if (mapIndex == _countof(cbResourceBindingTable))
            return FFX_ERROR_INVALID_ARGUMENT;

        inoutPipeline->constantBufferBindings[cbIndex].resourceIdentifier =
            cbResourceBindingTable[mapIndex].index;
    }

    return FFX_OK;
}

FFX_API FfxErrorCode FfxPopulateLpmConsts(
    bool incon,
    bool insoft,
    bool incon2,
    bool inclip,
    bool inscaleOnly,
    uint32_t& outcon,
    uint32_t& outsoft,
    uint32_t& outcon2,
    uint32_t& outclip,
    uint32_t& outscaleOnly)
{
    outcon = incon;
    outsoft = insoft;
    outcon2 = incon2;
    outclip = inclip;
    outscaleOnly = inscaleOnly;

    return FFX_OK;
}

FFX_API FfxVersionNumber ffxLpmGetEffectVersion()
{
    return FFX_SDK_MAKE_VERSION(
        FFX_LPM_VERSION_MAJOR, FFX_LPM_VERSION_MINOR, FFX_LPM_VERSION_PATCH);
}
