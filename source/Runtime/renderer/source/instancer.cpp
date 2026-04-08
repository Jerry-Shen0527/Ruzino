//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
#include "instancer.h"

#include <spdlog/spdlog.h>

#include "gpu_compute.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/quaternion.h"
#include "pxr/base/gf/quath.h"
#include "pxr/base/gf/rotation.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"

RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;

Hd_RUZINO_Instancer::Hd_RUZINO_Instancer(
    HdSceneDelegate* delegate,
    SdfPath const& id)
    : HdInstancer(delegate, id)
{
}

Hd_RUZINO_Instancer::~Hd_RUZINO_Instancer()
{
    TF_FOR_ALL(it, _primvarMap)
    {
        delete it->second;
    }
    _primvarMap.clear();
}

void Hd_RUZINO_Instancer::Sync(
    HdSceneDelegate* delegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits)
{
    _UpdateInstancer(delegate, dirtyBits);

    if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, GetId())) {
        _SyncPrimvars(delegate, *dirtyBits);
    }
}

void Hd_RUZINO_Instancer::_SyncPrimvars(
    HdSceneDelegate* delegate,
    HdDirtyBits dirtyBits)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    SdfPath const& id = GetId();

    HdPrimvarDescriptorVector primvars =
        delegate->GetPrimvarDescriptors(id, HdInterpolationInstance);

    for (HdPrimvarDescriptor const& pv : primvars) {
        if (HdChangeTracker::IsPrimvarDirty(dirtyBits, id, pv.name)) {
            VtValue value = delegate->Get(id, pv.name);
            if (!value.IsEmpty()) {
                if (_primvarMap.count(pv.name) > 0) {
                    delete _primvarMap[pv.name];
                }
                _primvarMap[pv.name] = new HdVtBufferSource(pv.name, value);
            }
        }
    }
}

VtMatrix4fArray Hd_RUZINO_Instancer::ComputeInstanceTransforms(
    SdfPath const& prototypeId)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    // The transforms for this level of instancer are computed by:
    // foreach(index : indices) {
    //     instancerTransform
    //     * hydra:instanceTranslations(index)
    //     * hydra:instanceRotations(index)
    //     * hydra:instanceScales(index)
    //     * hydra:instanceTransforms(index)
    // }
    // If any transform isn't provided, it's assumed to be the identity.

    GfMatrix4f instancerTransform =
        GfMatrix4f(GetDelegate()->GetInstancerTransform(GetId()));
    VtIntArray instanceIndices =
        GetDelegate()->GetInstanceIndices(GetId(), prototypeId);

    VtMatrix4fArray transforms(instanceIndices.size());
    for (size_t i = 0; i < instanceIndices.size(); ++i) {
        transforms[i] = instancerTransform;
    }

    // "hydra:instanceTranslations" holds a translation vector for each index.
    if (_primvarMap.count(HdInstancerTokens->instanceTranslations) > 0) {
        const GfVec3f* translations = static_cast<const GfVec3f*>(
            _primvarMap[HdInstancerTokens->instanceTranslations]->GetData());

        for (size_t i = 0; i < instanceIndices.size(); ++i) {
            int index = instanceIndices[i];
            GfMatrix4f translateMat(1);
            translateMat.SetTranslate(translations[index]);
            transforms[i] = translateMat * transforms[i];
        }
    }

    // "hydra:instanceRotations" holds a quaternion in <real, i, j, k>
    // format for each index.
    if (_primvarMap.count(HdInstancerTokens->instanceRotations) > 0) {
        const GfQuath* rotations = static_cast<const GfQuath*>(
            _primvarMap[HdInstancerTokens->instanceRotations]->GetData());

        for (size_t i = 0; i < instanceIndices.size(); ++i) {
            int index = instanceIndices[i];
            const GfQuath& quatHalf = rotations[index];

            // Convert half precision to float quaternion
            GfQuatf quat(
                quatHalf.GetReal(),
                quatHalf.GetImaginary()[0],
                quatHalf.GetImaginary()[1],
                quatHalf.GetImaginary()[2]);

            GfMatrix4f rotateMat(1);
            rotateMat.SetRotate(quat);
            transforms[i] = rotateMat * transforms[i];
        }
    }

    // "hydra:instanceScales" holds an axis-aligned scale vector for each index.
    if (_primvarMap.count(HdInstancerTokens->instanceScales) > 0) {
        const GfVec3f* scales = static_cast<const GfVec3f*>(
            _primvarMap[HdInstancerTokens->instanceScales]->GetData());

        for (size_t i = 0; i < instanceIndices.size(); ++i) {
            int index = instanceIndices[i];
            GfMatrix4f scaleMat(1);
            scaleMat.SetScale(scales[index]);
            transforms[i] = scaleMat * transforms[i];
        }
    }

    // "hydra:instanceTransforms" holds a 4x4 transform matrix for each index.
    if (_primvarMap.count(HdInstancerTokens->instanceTransforms) > 0) {
        const GfMatrix4d* instanceTransforms = static_cast<const GfMatrix4d*>(
            _primvarMap[HdInstancerTokens->instanceTransforms]->GetData());

        for (size_t i = 0; i < instanceIndices.size(); ++i) {
            int index = instanceIndices[i];
            transforms[i] =
                GfMatrix4f(instanceTransforms[index]) * transforms[i];
        }
    }

    if (GetParentId().IsEmpty()) {
        return transforms;
    }

    HdInstancer* parentInstancer =
        GetDelegate()->GetRenderIndex().GetInstancer(GetParentId());
    if (!TF_VERIFY(parentInstancer)) {
        return transforms;
    }

    VtMatrix4fArray parentTransforms =
        static_cast<Hd_RUZINO_Instancer*>(parentInstancer)
            ->ComputeInstanceTransforms(GetId());

    VtMatrix4fArray final(parentTransforms.size() * transforms.size());
    for (size_t i = 0; i < parentTransforms.size(); ++i) {
        for (size_t j = 0; j < transforms.size(); ++j) {
            final[i * transforms.size() + j] =
                transforms[j] * parentTransforms[i];
        }
    }
    return final;
}

void Hd_RUZINO_Instancer::ComputeInstanceTransforms(
    SdfPath const& prototypeId,
    DeviceMemoryPool<GeometryInstanceData>::MemoryHandle instance_buffer,
    DeviceMemoryPool<nvrhi::rt::InstanceDesc>::MemoryHandle rt_instance_buffer,
    uint64_t BLAS_address,
    const pxr::GfMatrix4f& prototype_transform,
    unsigned material_id,
    unsigned geometry_id)
{
    // GPU-based instance transform computation
    // This function will be called to fill the instance buffers on the GPU
    // instead of doing it on the CPU

    GfMatrix4f instancerTransform =
        GfMatrix4f(GetDelegate()->GetInstancerTransform(GetId()));
    VtIntArray instanceIndices =
        GetDelegate()->GetInstanceIndices(GetId(), prototypeId);

    spdlog::info(
        "GPU ComputeInstanceTransforms: prototype={}, instanceCount={}, "
        "geometryID={}, materialID={}",
        prototypeId.GetText(),
        instanceIndices.size(),
        geometry_id,
        material_id);

    const GfVec3f* translations = nullptr;
    const GfVec3f* scales = nullptr;
    const GfMatrix4d* instanceTransforms = nullptr;

    // For rotations, we need to convert from half to float
    VtArray<GfQuatf> rotationsFloat;
    const GfQuatf* rotations = nullptr;

    // Get primvar data pointers
    if (_primvarMap.count(HdInstancerTokens->instanceTranslations) > 0) {
        translations = static_cast<const GfVec3f*>(
            _primvarMap[HdInstancerTokens->instanceTranslations]->GetData());
    }

    if (_primvarMap.count(HdInstancerTokens->instanceRotations) > 0) {
        const GfQuath* rotationsHalf = static_cast<const GfQuath*>(
            _primvarMap[HdInstancerTokens->instanceRotations]->GetData());

        // Convert half precision to float
        size_t count = instanceIndices.size();
        rotationsFloat.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const GfQuath& qh = rotationsHalf[instanceIndices[i]];
            rotationsFloat[i] = GfQuatf(
                qh.GetReal(),
                qh.GetImaginary()[0],
                qh.GetImaginary()[1],
                qh.GetImaginary()[2]);
        }
        rotations = rotationsFloat.data();
    }

    if (_primvarMap.count(HdInstancerTokens->instanceScales) > 0) {
        scales = static_cast<const GfVec3f*>(
            _primvarMap[HdInstancerTokens->instanceScales]->GetData());
    }

    if (_primvarMap.count(HdInstancerTokens->instanceTransforms) > 0) {
        instanceTransforms = static_cast<const GfMatrix4d*>(
            _primvarMap[HdInstancerTokens->instanceTransforms]->GetData());
    }

    // Call the GPU assembler to compute instance transforms on GPU
    GPUSceneAssember::fill_instances(
        instancerTransform,
        instanceIndices,
        translations,
        rotations,
        scales,
        instanceTransforms,
        instance_buffer,
        rt_instance_buffer,
        BLAS_address,
        prototype_transform,
        material_id,
        geometry_id);
}

RUZINO_NAMESPACE_CLOSE_SCOPE
