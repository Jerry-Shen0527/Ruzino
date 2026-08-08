#include "light.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

#include "../nodes/shaders/Scene/Lights/LightData.slang"
#include "RHI/Hgi/format_conversion.hpp"
#include "RHI/shaderCompiler.h"
#include "nvrhi/utils.h"
#include "pxr/imaging/glf/simpleLight.h"
#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/rprimCollection.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hio/image.h"
#include "pxr/usd/sdr/shaderNode.h"
#include "pxr/usd/usd/tokens.h"
#include "pxr/usdImaging/usdImaging/tokens.h"
#include "renderParam.h"

RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;
void Hd_RUZINO_Light::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits)
{
    TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    TF_UNUSED(renderParam);

    if (!TF_VERIFY(sceneDelegate != nullptr)) {
        return;
    }

    auto render_param = static_cast<Hd_RUZINO_RenderParam*>(renderParam);
    render_param->InstanceCollection->mark_lights_dirty();

    const SdfPath& id = GetId();

    // HdStLight communicates to the scene graph and caches all interesting
    // values within this class. Later on Get() is called from
    // TaskState (RenderPass) to perform aggregation/pre-computation,
    // in order to make the shader execution efficient.

    // Change tracking
    HdDirtyBits bits = *dirtyBits;

    // Transform
    if (bits & DirtyTransform) {
        _params[HdTokens->transform] = VtValue(sceneDelegate->GetTransform(id));
    }

    // Lighting Params
    if (bits & DirtyParams) {
        HdChangeTracker& changeTracker =
            sceneDelegate->GetRenderIndex().GetChangeTracker();

        // Remove old dependencies
        VtValue val = Get(HdTokens->filters);
        if (val.IsHolding<SdfPathVector>()) {
            auto lightFilterPaths = val.UncheckedGet<SdfPathVector>();
            for (const SdfPath& filterPath : lightFilterPaths) {
                changeTracker.RemoveSprimSprimDependency(filterPath, id);
            }
        }

        if (_lightType == HdPrimTypeTokens->simpleLight) {
            _params[HdLightTokens->params] =
                sceneDelegate->Get(id, HdLightTokens->params);
        }
        // else if (_lightType == HdPrimTypeTokens->domeLight)
        //{
        //     _params[HdLightTokens->params] =
        //         _PrepareDomeLight(id, sceneDelegate);
        // }
        //// If it is an area light we will extract the parameters and convert
        //// them to a GlfSimpleLight that approximates the light source.
        // else
        //{
        //     _params[HdLightTokens->params] =
        //         _ApproximateAreaLight(id, sceneDelegate);
        // }

        // Add new dependencies
        val = Get(HdTokens->filters);
        if (val.IsHolding<SdfPathVector>()) {
            auto lightFilterPaths = val.UncheckedGet<SdfPathVector>();
            for (const SdfPath& filterPath : lightFilterPaths) {
                changeTracker.AddSprimSprimDependency(filterPath, id);
            }
        }
    }

    if (bits & (DirtyTransform | DirtyParams)) {
        auto transform = Get(HdTokens->transform).GetWithDefault<GfMatrix4d>();
        // Update cached light objects.  Note that simpleLight ignores
        // scene-delegate transform, in favor of the transform passed in by
        // params...
        if (_lightType == HdPrimTypeTokens->domeLight) {
            // Apply domeOffset if present
            VtValue domeOffset = sceneDelegate->GetLightParamValue(
                id, HdLightTokens->domeOffset);
            if (domeOffset.IsHolding<GfMatrix4d>()) {
                transform = domeOffset.UncheckedGet<GfMatrix4d>() * transform;
            }
            auto light =
                Get(HdLightTokens->params).GetWithDefault<GlfSimpleLight>();
            light.SetTransform(transform);
            _params[HdLightTokens->params] = VtValue(light);
        }
        else if (_lightType != HdPrimTypeTokens->simpleLight) {
            // e.g. area light
            auto light =
                Get(HdLightTokens->params).GetWithDefault<GlfSimpleLight>();
            GfVec3f p = GfVec3f(transform.ExtractTranslation());
            GfVec4f pos(p[0], p[1], p[2], 1.0f);
            // Convention is to emit light along -Z
            GfVec4d zDir = GfVec4f(transform.GetRow(2));
            if (_lightType == HdPrimTypeTokens->rectLight ||
                _lightType == HdPrimTypeTokens->diskLight) {
                light.SetSpotDirection(GfVec3f(-zDir[0], -zDir[1], -zDir[2]));
            }
            else if (_lightType == HdPrimTypeTokens->distantLight) {
                // For a distant light, translate to +Z homogeneous limit
                // See simpleLighting.glslfx : integrateLightsDefault.
                pos = GfVec4f(zDir[0], zDir[1], zDir[2], 0.0f);
            }
            else if (_lightType == HdPrimTypeTokens->sphereLight) {
                _params[HdLightTokens->radius] =
                    sceneDelegate->GetLightParamValue(
                        id, HdLightTokens->radius);
            }
            auto diffuse =
                sceneDelegate->GetLightParamValue(id, HdLightTokens->diffuse)
                    .Get<float>();
            auto color =
                sceneDelegate->GetLightParamValue(id, HdLightTokens->color)
                    .Get<GfVec3f>() *
                diffuse;
            light.SetDiffuse(GfVec4f(color[0], color[1], color[2], 0));
            light.SetPosition(pos);
            _params[HdLightTokens->params] = VtValue(light);
        }
    }

    // Shadow Params
    if (bits & DirtyShadowParams) {
        _params[HdLightTokens->shadowParams] =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->shadowParams);
    }

    // Shadow Collection
    if (bits & DirtyCollection) {
        VtValue vtShadowCollection = sceneDelegate->GetLightParamValue(
            id, HdLightTokens->shadowCollection);

        // Optional
        if (vtShadowCollection.IsHolding<HdRprimCollection>()) {
            auto newCollection =
                vtShadowCollection.UncheckedGet<HdRprimCollection>();

            if (_params[HdLightTokens->shadowCollection] != newCollection) {
                _params[HdLightTokens->shadowCollection] =
                    VtValue(newCollection);

                HdChangeTracker& changeTracker =
                    sceneDelegate->GetRenderIndex().GetChangeTracker();

                changeTracker.MarkCollectionDirty(newCollection.GetName());
            }
        }
        else {
            _params[HdLightTokens->shadowCollection] =
                VtValue(HdRprimCollection());
        }
    }

    // Don't clear dirty bits here - let derived classes handle it
}

HdDirtyBits Hd_RUZINO_Light::GetInitialDirtyBitsMask() const
{
    // In the case of simple and distant lights we want to sync all dirty bits,
    // but for area lights coming from the scenegraph we just want to extract
    // the Transform and Params for now.
    if (_lightType == HdPrimTypeTokens->simpleLight ||
        _lightType == HdPrimTypeTokens->distantLight) {
        return AllDirty;
    }
    else {
        return (DirtyParams | DirtyTransform);
    }
}

VtValue Hd_RUZINO_Light::Get(const TfToken& token) const
{
    VtValue val;
    TfMapLookup(_params, token, &val);
    return val;
}
void Hd_RUZINO_Light::Finalize(HdRenderParam* renderParam)
{
    auto render_param = static_cast<Hd_RUZINO_RenderParam*>(renderParam);
    render_param->InstanceCollection->mark_lights_dirty();
    HdLight::Finalize(renderParam);
}

void Hd_RUZINO_Simple_Light::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits)
{
    Hd_RUZINO_Light::Sync(sceneDelegate, renderParam, dirtyBits);

    // Allocate and populate light buffer for simple/point light
    if (*dirtyBits & (DirtyParams | DirtyTransform)) {
        auto* render_param = static_cast<Hd_RUZINO_RenderParam*>(renderParam);
        if (!this->light_buffer) {
            this->light_buffer =
                render_param->InstanceCollection->light_pool.allocate(1);
        }

        const SdfPath& id = this->GetId();
        LightData lightData;
        lightData.type = (uint32_t)LightType::Point;

        // Get transform
        auto transform =
            this->Get(HdTokens->transform).GetWithDefault<GfMatrix4d>();
        GfVec3d pos = transform.ExtractTranslation();
        lightData.posW = float3(pos[0], pos[1], pos[2]);

        // Get color and intensity with standard USD Light API
        auto diffuse =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->diffuse)
                .GetWithDefault<float>(1.0f);
        auto color = sceneDelegate->GetLightParamValue(id, HdLightTokens->color)
                         .GetWithDefault<GfVec3f>(GfVec3f(1, 1, 1));
        auto intensity =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->intensity)
                .GetWithDefault<float>(1.0f);
        auto exposure =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->exposure)
                .GetWithDefault<float>(0.0f);

        float finalIntensity = intensity * pow(2.0f, exposure);
        lightData.intensity =
            float3(color[0], color[1], color[2]) * diffuse * finalIntensity;

        this->light_buffer->write_data(&lightData);
    }

    // Clear dirty bits
    *dirtyBits = Clean;
}

void Hd_RUZINO_Distant_Light::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits)
{
    Hd_RUZINO_Light::Sync(sceneDelegate, renderParam, dirtyBits);

    // Get distant light specific parameters
    const SdfPath& id = this->GetId();
    HdDirtyBits bits = *dirtyBits;

    if (bits & (DirtyTransform | DirtyParams)) {
        auto transform =
            this->Get(HdTokens->transform).GetWithDefault<GfMatrix4d>();

        // Extract direction from transform
        GfVec4d zDir = GfVec4f(transform.GetRow(2));
        _direction = GfVec3f(zDir[0], zDir[1], zDir[2]);

        // Get angle parameter if available
        VtValue angleValue =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->angle);
        if (!angleValue.IsEmpty()) {
            _angle = angleValue.Get<float>();
        }

        // Allocate and populate light buffer
        auto* render_param = static_cast<Hd_RUZINO_RenderParam*>(renderParam);
        if (!this->light_buffer) {
            this->light_buffer =
                render_param->InstanceCollection->light_pool.allocate(1);
        }

        LightData lightData;
        lightData.type = (uint32_t)LightType::Distant;
        lightData.dirW = float3(_direction[0], _direction[1], _direction[2]);
        lightData.cosSubtendedAngle = cos(_angle);

        // Get color and intensity with exposure
        auto diffuse =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->diffuse)
                .GetWithDefault<float>(1.0f);
        auto color = sceneDelegate->GetLightParamValue(id, HdLightTokens->color)
                         .GetWithDefault<GfVec3f>(GfVec3f(1, 1, 1));
        auto intensity =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->intensity)
                .GetWithDefault<float>(1.0f);
        auto exposure =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->exposure)
                .GetWithDefault<float>(0.0f);

        float finalIntensity = intensity * pow(2.0f, exposure);
        lightData.intensity =
            float3(color[0], color[1], color[2]) * diffuse * finalIntensity;

        this->light_buffer->write_data(&lightData);
    }

    // Clear dirty bits
    *dirtyBits = Clean;
}

void Hd_RUZINO_Sphere_Light::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits)
{
    Hd_RUZINO_Light::Sync(sceneDelegate, renderParam, dirtyBits);

    // Get sphere light specific parameters
    const SdfPath& id = this->GetId();
    HdDirtyBits bits = *dirtyBits;

    if (bits & DirtyParams) {
        VtValue radiusValue =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->radius);
        if (!radiusValue.IsEmpty()) {
            _radius = radiusValue.Get<float>();
        }
    }

    // Allocate and populate light buffer
    if (bits & (DirtyParams | DirtyTransform)) {
        auto* render_param = static_cast<Hd_RUZINO_RenderParam*>(renderParam);
        if (!this->light_buffer) {
            this->light_buffer =
                render_param->InstanceCollection->light_pool.allocate(1);
        }

        LightData lightData;
        lightData.type = (uint32_t)LightType::Sphere;

        // Get transform
        auto transform =
            this->Get(HdTokens->transform).GetWithDefault<GfMatrix4d>();
        GfVec3d pos = transform.ExtractTranslation();
        lightData.posW = float3(pos[0], pos[1], pos[2]);

        // Get color and intensity with exposure
        auto diffuse =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->diffuse)
                .GetWithDefault<float>(1.0f);
        auto color = sceneDelegate->GetLightParamValue(id, HdLightTokens->color)
                         .GetWithDefault<GfVec3f>(GfVec3f(1, 1, 1));
        auto intensity =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->intensity)
                .GetWithDefault<float>(1.0f);
        auto exposure =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->exposure)
                .GetWithDefault<float>(0.0f);

        float finalIntensity = intensity * pow(2.0f, exposure);
        lightData.intensity =
            float3(color[0], color[1], color[2]) * diffuse * finalIntensity;

        // Sphere-specific parameters
        // Compute surface area for future use in importance sampling
        lightData.surfaceArea = 4.0f * 3.14159265359f * _radius * _radius;

        this->light_buffer->write_data(&lightData);
    }

    // Clear dirty bits
    *dirtyBits = Clean;
}

void Hd_RUZINO_Rect_Light::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits)
{
    Hd_RUZINO_Light::Sync(sceneDelegate, renderParam, dirtyBits);

    // Get rectangle light specific parameters
    const SdfPath& id = this->GetId();
    HdDirtyBits bits = *dirtyBits;

    if (bits & DirtyParams) {
        VtValue widthValue =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->width);
        if (!widthValue.IsEmpty()) {
            _width = widthValue.Get<float>();
        }

        VtValue heightValue =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->height);
        if (!heightValue.IsEmpty()) {
            _height = heightValue.Get<float>();
        }
    }

    // Allocate and populate light buffer
    if (bits & (DirtyParams | DirtyTransform)) {
        auto* render_param = static_cast<Hd_RUZINO_RenderParam*>(renderParam);
        if (!this->light_buffer) {
            this->light_buffer =
                render_param->InstanceCollection->light_pool.allocate(1);
        }

        LightData lightData;
        lightData.type = (uint32_t)LightType::Rect;

        // Get transform
        auto transform =
            this->Get(HdTokens->transform).GetWithDefault<GfMatrix4d>();
        GfVec3d pos = transform.ExtractTranslation();
        lightData.posW = float3(pos[0], pos[1], pos[2]);

        // Get direction (rectangle emits along -Z in local space)
        GfVec4d zDir = transform.GetRow(2);
        float3 dir = float3(-zDir[0], -zDir[1], -zDir[2]);
        float dirLen = sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        lightData.dirW = float3(
            dir.x / dirLen, dir.y / dirLen, dir.z / dirLen);  // Normalized

        // Get tangent and bitangent for rectangle orientation
        // Extract normalized directions and compute scaled tangent/bitangent
        GfVec4d xDir = transform.GetRow(0);
        GfVec4d yDir = transform.GetRow(1);

        // Normalize and scale by width/height
        float3 xVec = float3(xDir[0], xDir[1], xDir[2]);
        float3 yVec = float3(yDir[0], yDir[1], yDir[2]);
        float xLen = sqrt(xVec.x * xVec.x + xVec.y * xVec.y + xVec.z * xVec.z);
        float yLen = sqrt(yVec.x * yVec.x + yVec.y * yVec.y + yVec.z * yVec.z);

        lightData.tangent =
            float3(xVec.x / xLen, xVec.y / xLen, xVec.z / xLen) * _width;
        lightData.bitangent =
            float3(yVec.x / yLen, yVec.y / yLen, yVec.z / yLen) * _height;

        // Get color and intensity with exposure
        auto diffuse =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->diffuse)
                .GetWithDefault<float>(1.0f);
        auto color = sceneDelegate->GetLightParamValue(id, HdLightTokens->color)
                         .GetWithDefault<GfVec3f>(GfVec3f(1, 1, 1));
        auto intensity =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->intensity)
                .GetWithDefault<float>(1.0f);
        auto exposure =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->exposure)
                .GetWithDefault<float>(0.0f);

        // Combine intensity with exposure: intensity * 2^exposure
        float finalIntensity = intensity * pow(2.0f, exposure);
        lightData.intensity =
            float3(color[0], color[1], color[2]) * diffuse * finalIntensity;

        // Rectangle area
        lightData.surfaceArea = _width * _height;

        // Store transformation matrix for potential texture mapping
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                lightData.transMat[i][j] = transform[i][j];
            }
        }

        // Compute inverse transpose for normal transformation
        GfMatrix4d invTranspose = transform.GetInverse().GetTranspose();
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                lightData.transMatIT[i][j] = invTranspose[i][j];
            }
        }

        this->light_buffer->write_data(&lightData);

        // Build (or rebuild) the intersectable light geometry so that
        // BSDF-sampled rays can hit this RectLight and return its Le. The quad
        // is built in world space from posW/tangent/bitangent (already
        // world-space, scaled by width/height), so the TLAS instance transform
        // is identity. light_buffer->index() is the stable LightData slot used
        // by the ClosestHit shader to read Le.
        BuildLightGeometry(
            render_param,
            lightData.posW,
            lightData.tangent,
            lightData.bitangent);
    }

    // Clear dirty bits
    *dirtyBits = Clean;
}

void Hd_RUZINO_Rect_Light::BuildLightGeometry(
    Hd_RUZINO_RenderParam* render_param,
    float3 posW,
    float3 tangent,
    float3 bitangent)
{
    auto device = RHI::get_device();
    if (!light_command_list) {
        light_command_list =
            device->createCommandList({ .enableImmediateExecution = false });
    }
    auto descriptor_table =
        render_param->InstanceCollection->get_buffer_descriptor_table();

    // 4 corner vertices of the quad (world space), 2 triangles.
    // tangent/bitangent are full-width/height vectors, so half-extents are 0.5.
    float3 corners[4] = {
        posW - 0.5f * tangent - 0.5f * bitangent,
        posW + 0.5f * tangent - 0.5f * bitangent,
        posW + 0.5f * tangent + 0.5f * bitangent,
        posW - 0.5f * tangent + 0.5f * bitangent,
    };
    float positions[4 * 3];
    for (int i = 0; i < 4; ++i) {
        positions[i * 3 + 0] = corners[i].x;
        positions[i * 3 + 1] = corners[i].y;
        positions[i * 3 + 2] = corners[i].z;
    }
    uint indices[6] = { 0, 1, 2, 0, 2, 3 };

    const size_t positions_bytes = sizeof(positions);  // 48
    const size_t indices_bytes = sizeof(indices);      // 24
    const size_t index_offset = positions_bytes;
    const size_t total_bytes = positions_bytes + indices_bytes;

    // (Re)create the vertex/index buffer each time geometry changes. This is
    // cheap (72 bytes) and avoids tracking whether only params (intensity) vs.
    // geometry (width/height/transform) changed.
    nvrhi::BufferDesc desc =
        nvrhi::BufferDesc{}
            .setCanHaveRawViews(true)
            .setByteSize(total_bytes)
            .setIsVertexBuffer(true)
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setCpuAccess(nvrhi::CpuAccessMode::None)
            .setIsAccelStructBuildInput(true)
            .setKeepInitialState(true)
            .setDebugName("rectLightVertexBuffer");
    light_vertex_buffer = device->createBuffer(desc);

    light_command_list->open();
    light_command_list->writeBuffer(
        light_vertex_buffer, positions, positions_bytes, 0);
    light_command_list->writeBuffer(
        light_vertex_buffer, indices, indices_bytes, index_offset);
    light_command_list->close();

    {
        std::lock_guard lock(execution_launch_mutex);
        device->executeCommandList(light_command_list);

        // Build the BLAS (2 triangles, RGB32_FLOAT positions, R32_UINT indices
        // -- same layout as Hd_RUZINO_Mesh so it uses the standard triangle
        // hit group 0/1).
        nvrhi::rt::AccelStructDesc blas_desc;
        nvrhi::rt::GeometryDesc geometry_desc;
        geometry_desc.geometryType = nvrhi::rt::GeometryType::Triangles;
        nvrhi::rt::GeometryTriangles triangles;
        triangles.setVertexBuffer(light_vertex_buffer)
            .setVertexOffset(0)
            .setIndexBuffer(light_vertex_buffer)
            .setIndexOffset(index_offset)
            .setIndexCount(6)
            .setVertexCount(4)
            .setVertexStride(3 * sizeof(float))
            .setVertexFormat(nvrhi::Format::RGB32_FLOAT)
            .setIndexFormat(nvrhi::Format::R32_UINT);
        geometry_desc.setTriangles(triangles);
        blas_desc.addBottomLevelGeometry(geometry_desc);
        blas_desc.isTopLevel = false;
        light_blas = device->createAccelStruct(blas_desc);

        light_command_list->open();
        nvrhi::utils::BuildBottomLevelAccelStruct(
            light_command_list, light_blas, blas_desc);
        light_command_list->close();
        device->executeCommandList(light_command_list);
        device->waitForIdle();

        // Register the vertex/index buffer in the bindless descriptor table so
        // the hit shaders can fetch positions via MeshDesc.bindlessIndex.
        light_descriptor_handle = descriptor_table->CreateDescriptorHandle(
            nvrhi::BindingSetItem::RawBuffer_SRV(0, light_vertex_buffer.Get()));
    }

    // Write a MeshDesc so the standard BindlessVertexBuffer fetch path works.
    // Only positions + indices are populated; normals/tangents/texcoords are
    // unused for a light (Le is read from lightBuffer, not a material).
    MeshDesc mesh_desc;
    mesh_desc.vbOffset = 0;
    mesh_desc.ibOffset = index_offset;
    mesh_desc.normalOffset = 0;
    mesh_desc.tangentOffset = 0;
    mesh_desc.texCrdOffset = 0;
    mesh_desc.subsetMatIdOffset = 0;
    mesh_desc.skinningVbOffset = 0;
    mesh_desc.prevVbOffset = 0;
    mesh_desc.flags = 0;
    mesh_desc.bindlessIndex = light_descriptor_handle.Get();
    mesh_desc.texCrdInterpolation = InterpolationType::Vertex;
    mesh_desc.normalInterpolation = InterpolationType::Vertex;
    mesh_desc.tangentInterpolation = InterpolationType::Vertex;
    mesh_desc.padding = 0;

    if (!light_mesh_desc_buffer) {
        light_mesh_desc_buffer =
            render_param->InstanceCollection->mesh_pool.allocate(1);
    }
    light_mesh_desc_buffer->write_data(&mesh_desc);

    // Write the TLAS instance. geometryID = light_buffer->index() so the
    // ClosestHit shader can look up Le. materialID = -1 (lights have no
    // material). flags marks this as GeometryType::Light (top kTypeBits). The
    // quad is already in world space, so the instance transform is identity.
    GeometryInstanceData instance_data(GeometryType::Light);
    instance_data.geometryID = light_buffer->index();
    instance_data.materialID = uint(-1);
    GfMatrix4f identity(1.0f);
    memcpy(
        &instance_data.transform,
        identity.data(),
        sizeof(GfMatrix4f));  // float4x4 is binary-compatible with GfMatrix4f

    if (!light_instance_buffer) {
        light_instance_buffer =
            render_param->InstanceCollection->instance_pool.allocate(1);
    }
    light_instance_buffer->write_data(&instance_data);

    nvrhi::rt::InstanceDesc rt_instance;
    rt_instance.blasDeviceAddress = light_blas->getDeviceAddress();
    // Light geometry is a NON-OCCLUDER: a light does not block light (it would
    // be nonsensical for a light to shadow itself, or to block another light's
    // illumination in a way its own surface already accounts for). We give
    // light instances a dedicated instanceMask bit (0x2) so that shadow rays
    // can exclude them via InstanceInclusionMask = 0xFF & ~0x2 = 0xFD, while
    // radiance rays keep 0xFF so BSDF-sampled rays still hit the light and
    // return Le. Meshes use mask 0x1, so the two namespaces don't collide.
    rt_instance.instanceMask = 0x2;
    rt_instance.flags = nvrhi::rt::InstanceFlags::None;
    // identity transform (reuse `identity` declared above for instance_data)
    GfMatrix4f identity_T = identity.GetTranspose();  // nvrhi wants row-major
    memcpy(
        rt_instance.transform,
        identity_T.data(),
        sizeof(nvrhi::rt::AffineTransform));
    rt_instance.instanceID = light_instance_buffer->index();
    rt_instance.instanceContributionToHitGroupIndex =
        0;  // standard triangle hit group (ClosestHit / ShadowHit)

    if (!light_rt_instance_buffer) {
        light_rt_instance_buffer =
            render_param->InstanceCollection->rt_instance_pool.allocate(1);
    }
    light_rt_instance_buffer->write_data(&rt_instance);

    render_param->InstanceCollection->set_require_rebuild_tlas();
}

void Hd_RUZINO_Disk_Light::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits)
{
    Hd_RUZINO_Light::Sync(sceneDelegate, renderParam, dirtyBits);

    // Get disk light specific parameters
    const SdfPath& id = this->GetId();
    HdDirtyBits bits = *dirtyBits;

    if (bits & DirtyParams) {
        VtValue radiusValue =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->radius);
        if (!radiusValue.IsEmpty()) {
            _radius = radiusValue.Get<float>();
        }
    }

    // Allocate and populate light buffer
    if (bits & (DirtyParams | DirtyTransform)) {
        auto* render_param = static_cast<Hd_RUZINO_RenderParam*>(renderParam);
        if (!this->light_buffer) {
            this->light_buffer =
                render_param->InstanceCollection->light_pool.allocate(1);
        }

        LightData lightData;
        lightData.type = (uint32_t)LightType::Disc;

        // Get transform
        auto transform =
            this->Get(HdTokens->transform).GetWithDefault<GfMatrix4d>();
        GfVec3d pos = transform.ExtractTranslation();
        lightData.posW = float3(pos[0], pos[1], pos[2]);

        // Get direction (disk emits along -Z in local space)
        GfVec4d zDir = transform.GetRow(2);
        float3 dir = float3(-zDir[0], -zDir[1], -zDir[2]);
        float dirLen = sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        lightData.dirW = float3(
            dir.x / dirLen, dir.y / dirLen, dir.z / dirLen);  // Normalized

        // Get tangent and bitangent for disk orientation
        // Extract normalized directions and scale by radius
        GfVec4d xDir = transform.GetRow(0);
        GfVec4d yDir = transform.GetRow(1);

        float3 xVec = float3(xDir[0], xDir[1], xDir[2]);
        float3 yVec = float3(yDir[0], yDir[1], yDir[2]);
        float xLen = sqrt(xVec.x * xVec.x + xVec.y * xVec.y + xVec.z * xVec.z);
        float yLen = sqrt(yVec.x * yVec.x + yVec.y * yVec.y + yVec.z * yVec.z);

        lightData.tangent =
            float3(xVec.x / xLen, xVec.y / xLen, xVec.z / xLen) * _radius;
        lightData.bitangent =
            float3(yVec.x / yLen, yVec.y / yLen, yVec.z / yLen) * _radius;

        // Get color and intensity with exposure
        auto diffuse =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->diffuse)
                .GetWithDefault<float>(1.0f);
        auto color = sceneDelegate->GetLightParamValue(id, HdLightTokens->color)
                         .GetWithDefault<GfVec3f>(GfVec3f(1, 1, 1));
        auto intensity =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->intensity)
                .GetWithDefault<float>(1.0f);
        auto exposure =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->exposure)
                .GetWithDefault<float>(0.0f);

        float finalIntensity = intensity * pow(2.0f, exposure);
        lightData.intensity =
            float3(color[0], color[1], color[2]) * diffuse * finalIntensity;

        // Disk area
        lightData.surfaceArea = 3.14159265359f * _radius * _radius;

        this->light_buffer->write_data(&lightData);
    }

    // Clear dirty bits
    *dirtyBits = Clean;
}

void Hd_RUZINO_Cylinder_Light::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits)
{
    Hd_RUZINO_Light::Sync(sceneDelegate, renderParam, dirtyBits);

    // Get cylinder light specific parameters
    const SdfPath& id = this->GetId();
    HdDirtyBits bits = *dirtyBits;

    if (bits & DirtyParams) {
        VtValue radiusValue =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->radius);
        if (!radiusValue.IsEmpty()) {
            _radius = radiusValue.Get<float>();
        }

        VtValue lengthValue =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->length);
        if (!lengthValue.IsEmpty()) {
            _length = lengthValue.Get<float>();
        }
    }

    // Allocate and populate light buffer
    if (bits & (DirtyParams | DirtyTransform)) {
        auto* render_param = static_cast<Hd_RUZINO_RenderParam*>(renderParam);
        if (!this->light_buffer) {
            this->light_buffer =
                render_param->InstanceCollection->light_pool.allocate(1);
        }

        LightData lightData;
        // Note: Cylinder light is not in the standard LightType enum
        // For now, treat it as a special case or extend the enum
        // Using Point as placeholder - you may want to add LightType::Cylinder
        lightData.type = (uint32_t)LightType::Point;  // TODO: Add Cylinder type

        // Get transform
        auto transform =
            this->Get(HdTokens->transform).GetWithDefault<GfMatrix4d>();
        GfVec3d pos = transform.ExtractTranslation();
        lightData.posW = float3(pos[0], pos[1], pos[2]);

        // Cylinder axis direction (along local Z)
        GfVec4d zDir = transform.GetRow(2);
        lightData.dirW = float3(zDir[0], zDir[1], zDir[2]);

        // Get color and intensity
        auto diffuse =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->diffuse)
                .GetWithDefault<float>(1.0f);
        auto color = sceneDelegate->GetLightParamValue(id, HdLightTokens->color)
                         .GetWithDefault<GfVec3f>(GfVec3f(1, 1, 1));
        auto intensity =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->intensity)
                .GetWithDefault<float>(1.0f);
        lightData.intensity =
            float3(color[0], color[1], color[2]) * diffuse * intensity;

        // Cylinder surface area (without caps)
        lightData.surfaceArea = 2.0f * 3.14159265359f * _radius * _length;

        this->light_buffer->write_data(&lightData);
    }

    // Clear dirty bits
    *dirtyBits = Clean;
}

void Hd_RUZINO_Dome_Light::_PrepareDomeLight(
    SdfPath const& id,
    HdSceneDelegate* sceneDelegate)
{
    const VtValue v =
        sceneDelegate->GetLightParamValue(id, HdLightTokens->textureFile);

    // Only get texture file if the value is not empty and holds the correct
    // type
    if (!v.IsEmpty() && v.IsHolding<pxr::SdfAssetPath>()) {
        textureFileName = v.Get<pxr::SdfAssetPath>();
        env_texture.image =
            HioImage::OpenForReading(textureFileName.GetAssetPath(), 0, 0);
    }

    auto diffuse = sceneDelegate->GetLightParamValue(id, HdLightTokens->diffuse)
                       .GetWithDefault<float>(1.0f);
    radiance = sceneDelegate->GetLightParamValue(id, HdLightTokens->color)
                   .GetWithDefault<GfVec3f>(GfVec3f(1.0f, 1.0f, 1.0f)) *
               diffuse;

    auto intensity =
        sceneDelegate->GetLightParamValue(id, HdLightTokens->intensity)
            .GetWithDefault<float>(1.0f);
    auto exposure =
        sceneDelegate->GetLightParamValue(id, HdLightTokens->exposure)
            .GetWithDefault<float>(0.0f);

    // Combine with exposure: intensity * 2^exposure
    float finalIntensity = intensity * pow(2.0f, exposure);
    radiance *= finalIntensity;
}

void Hd_RUZINO_Dome_Light::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits)
{
    Hd_RUZINO_Light::Sync(sceneDelegate, renderParam, dirtyBits);

    const SdfPath& id = GetId();
    HdDirtyBits bits = *dirtyBits;

    // Always check shader_path — custom attribute changes don't trigger
    // DirtyParams
    {
        // USD's LookupLightParamAttribute only maps KNOWN light params
        // (intensity, color, ...) from "inputs:X" to the light container key
        // "X". Custom params fall back to a BARE attribute lookup (no "inputs:"
        // prefix), so a scene that writes "inputs:shader_path" (the natural
        // convention, matching inputs:intensity) will NOT be found. Try both
        // forms.
        VtValue shaderPathValue =
            sceneDelegate->GetLightParamValue(id, TfToken("shader_path"));
        if (shaderPathValue.IsEmpty()) {
            shaderPathValue = sceneDelegate->GetLightParamValue(
                id, TfToken("inputs:shader_path"));
        }
        this->has_valid_shader = false;
        if (shaderPathValue.IsHolding<std::string>()) {
            shader_path = shaderPathValue.UncheckedGet<std::string>();
        }
        else if (shaderPathValue.IsHolding<SdfAssetPath>()) {
            shader_path =
                shaderPathValue.UncheckedGet<SdfAssetPath>().GetAssetPath();
        }
        else {
            shader_path.clear();
        }

        if (!shader_path.empty()) {
            std::filesystem::path shader_file_path(shader_path);
            if (!shader_file_path.is_absolute()) {
                shader_file_path = std::filesystem::path(
                                       SlangShaderCompiler::get_shader_dir(
                                           ShaderDirType::Renderer)) /
                                   shader_path;
            }

            if (std::filesystem::exists(shader_file_path) &&
                std::filesystem::is_regular_file(shader_file_path)) {
                this->has_valid_shader = true;
                spdlog::info(
                    "DomeLight {}: Valid shader file '{}' (resolved to '{}')",
                    id.GetText(),
                    shader_path,
                    shader_file_path.string());
            }
            else if (std::filesystem::exists(shader_file_path)) {
                spdlog::warn(
                    "DomeLight {}: shader_path '{}' exists but is not a file",
                    id.GetText(),
                    shader_path);
            }
            else {
                spdlog::warn(
                    "DomeLight {}: shader_path '{}' does not exist (looked at "
                    "'{}')",
                    id.GetText(),
                    shader_path,
                    shader_file_path.string());
            }
        }
    }

    if (bits & (DirtyParams | DirtyTransform)) {
        _PrepareDomeLight(id, sceneDelegate);

        auto* render_param = static_cast<Hd_RUZINO_RenderParam*>(renderParam);

        // Allocate light buffer if needed
        if (!this->light_buffer) {
            this->light_buffer =
                render_param->InstanceCollection->light_pool.allocate(1);
        }

        LightData lightData;
        lightData.type = (uint32_t)LightType::Dome;
        lightData.intensity = float3(radiance[0], radiance[1], radiance[2]);

        // Hosek-Wilkie analytic sky: if this dome points at the Hosek callable,
        // read turbidity / groundAlbedo / sunDirection, cook the polynomial
        // state, and stamp the row index into the light data. The shader-side
        // hosekStateBuffer is always bound (a dummy zero row lives at index 0),
        // so non-Hosek domes just leave hosekStateIndex = 0 and dirW = default.
        if (this->has_valid_shader) {
            auto readFloat = [&](const char* name, float def) -> float {
                VtValue v =
                    sceneDelegate->GetLightParamValue(id, TfToken(name));
                if (v.IsEmpty())
                    v = sceneDelegate->GetLightParamValue(
                        id, TfToken(std::string("inputs:") + name));
                if (v.IsHolding<float>())
                    return v.UncheckedGet<float>();
                if (v.IsHolding<double>())
                    return float(v.UncheckedGet<double>());
                return def;
            };

            float turbidity = readFloat("turbidity", 3.0f);
            float albedo = readFloat("groundAlbedo", 0.3f);

            // Sun direction in dome-local space (+Y up). Default ~45 deg.
            GfVec3f sunDir(0.5f, 0.7f, 0.5f);
            VtValue sunV =
                sceneDelegate->GetLightParamValue(id, TfToken("sunDirection"));
            if (sunV.IsEmpty())
                sunV = sceneDelegate->GetLightParamValue(
                    id, TfToken("inputs:sunDirection"));
            if (sunV.IsHolding<GfVec3f>())
                sunDir = sunV.UncheckedGet<GfVec3f>();
            else if (sunV.IsHolding<GfVec3d>()) {
                GfVec3d d = sunV.UncheckedGet<GfVec3d>();
                sunDir = GfVec3f(float(d[0]), float(d[1]), float(d[2]));
            }
            // Clamp to above horizon (Hosek is undefined for sun below 0).
            if (sunDir[1] < 0.0f)
                sunDir[1] = 0.0f;
            float len = std::sqrt(
                sunDir[0] * sunDir[0] + sunDir[1] * sunDir[1] +
                sunDir[2] * sunDir[2]);
            if (len > 1e-6f)
                sunDir /= len;

            // Re-cook only when a parameter changed.
            if (turbidity != hosek_turbidity || albedo != hosek_albedo ||
                sunDir != hosek_sun_dir || !hosek_state_handle) {
                hosek_turbidity = turbidity;
                hosek_albedo = albedo;
                hosek_sun_dir = sunDir;

                float elevation = std::asin(std::clamp(sunDir[1], -1.0f, 1.0f));
                ruzino::HosekSkyState state =
                    ruzino::hosek_cook(turbidity, albedo, elevation);

                if (!hosek_state_handle) {
                    hosek_state_handle = render_param->InstanceCollection
                                             ->hosek_state_pool.allocate(1);
                }
                hosek_state_handle->write_data(&state);
                hosek_state_index = hosek_state_handle->index();
                spdlog::info(
                    "DomeLight {}: cooked Hosek sky (turbidity={}, albedo={}, "
                    "elev={:.1f} deg, stateIndex={})",
                    id.GetText(),
                    turbidity,
                    albedo,
                    elevation * 180.0 / 3.14159265358979,
                    hosek_state_index);
            }

            lightData.hosekStateIndex = hosek_state_index;
            // dirW carries the dome-local sun direction for the miss handler to
            // forward to the callable as sunDirLocal.
            lightData.dirW = float3(sunDir[0], sunDir[1], sunDir[2]);
        }

        // Get transform with domeOffset
        auto transform =
            this->Get(HdTokens->transform).GetWithDefault<GfMatrix4d>();
        VtValue domeOffset =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->domeOffset);
        if (domeOffset.IsHolding<GfMatrix4d>()) {
            transform = domeOffset.UncheckedGet<GfMatrix4d>() * transform;
        }

        // Store transformation matrix for dome light rotation
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                lightData.transMat[i][j] = transform[i][j];
            }
        }

        // Load and register texture with bindless system if available
        if (env_texture.image) {
            int width = env_texture.image->GetWidth();
            int height = env_texture.image->GetHeight();
            auto hioFormat = env_texture.image->GetFormat();

            auto image = env_texture.image;

            auto storage_byte_size = image->GetBytesPerPixel();

            std::vector<uint8_t> data(
                image->GetWidth() * image->GetHeight() * storage_byte_size, 0);

            // Read image data
            HioImage::StorageSpec storage;
            storage.width = width;
            storage.height = height;
            storage.format = hioFormat;
            storage.flipped = false;
            storage.data = data.data();

            env_texture.image->Read(storage);

            if (storage.data) {
                // Convert format to NVRHI format
                nvrhi::Format format = nvrhi::Format::RGBA8_UNORM;
                size_t bytesPerPixel = 4;

                if (hioFormat == HioFormatFloat32Vec3) {
                    format = nvrhi::Format::RGBA32_FLOAT;
                    bytesPerPixel = 16;  // Will convert RGB to RGBA
                }
                else if (hioFormat == HioFormatFloat16Vec3) {
                    format = nvrhi::Format::RGBA16_FLOAT;
                    bytesPerPixel = 8;
                }
                else if (hioFormat == HioFormatUNorm8Vec3) {
                    format = nvrhi::Format::RGBA8_UNORM;
                    bytesPerPixel = 4;
                }

                // Create texture descriptor
                nvrhi::TextureDesc desc;
                desc.width = width;
                desc.height = height;
                desc.format = format;
                desc.isRenderTarget = false;
                desc.isUAV = false;
                desc.debugName = "DomeLight_" + id.GetString();
                desc.initialState = nvrhi::ResourceStates::ShaderResource;
                desc.keepInitialState = true;

                // Convert RGB to RGBA if needed
                std::vector<uint8_t> rgba_data;
                const uint8_t* source_data =
                    static_cast<const uint8_t*>(storage.data);

                if (hioFormat == HioFormatFloat32Vec3) {
                    // RGB32F -> RGBA32F
                    rgba_data.resize(width * height * 16);
                    const float* src =
                        reinterpret_cast<const float*>(source_data);
                    float* dst = reinterpret_cast<float*>(rgba_data.data());
                    for (int i = 0; i < width * height; i++) {
                        dst[i * 4 + 0] = src[i * 3 + 0];
                        dst[i * 4 + 1] = src[i * 3 + 1];
                        dst[i * 4 + 2] = src[i * 3 + 2];
                        dst[i * 4 + 3] = 1.0f;
                    }
                    source_data = rgba_data.data();
                }
                else if (hioFormat == HioFormatUNorm8Vec3) {
                    // RGB8 -> RGBA8
                    rgba_data.resize(width * height * 4);
                    for (int i = 0; i < width * height; i++) {
                        rgba_data[i * 4 + 0] = source_data[i * 3 + 0];
                        rgba_data[i * 4 + 1] = source_data[i * 3 + 1];
                        rgba_data[i * 4 + 2] = source_data[i * 3 + 2];
                        rgba_data[i * 4 + 3] = 255;
                    }
                    source_data = rgba_data.data();
                }

                // Load texture to GPU
                auto [gpu_texture, staging] =
                    RHI::load_texture(desc, source_data);
                env_texture.texture = gpu_texture;

                // Register with bindless texture system
                auto descriptor_table = render_param->InstanceCollection
                                            ->get_texture_descriptor_table();
                env_texture.descriptor =
                    descriptor_table->CreateDescriptorHandle(
                        nvrhi::BindingSetItem::Texture_SRV(
                            0, env_texture.texture, format));

                lightData.textureIndex = env_texture.descriptor.Get();

                spdlog::info(
                    "DomeLight {}: Loaded texture '{}' ({}x{}) with bindless "
                    "index {}",
                    id.GetText(),
                    textureFileName.GetAssetPath(),
                    width,
                    height,
                    lightData.textureIndex);
            }
        }
        else {
            // No texture - use solid color
            lightData.textureIndex = -1;
            spdlog::info(
                "DomeLight {}: Using solid color ({},{},{})",
                id.GetText(),
                radiance[0],
                radiance[1],
                radiance[2]);
        }

        this->light_buffer->write_data(&lightData);
    }

    // Clear dirty bits
    *dirtyBits = Clean;
}

void Hd_RUZINO_Dome_Light::Finalize(HdRenderParam* renderParam)
{
    // Clean up texture descriptor
    if (env_texture.descriptor.IsValid()) {
        env_texture.descriptor.Reset();
    }

    // Clean up old GL texture if it exists
    if (env_texture.glTexture) {
        glDeleteTextures(1, &env_texture.glTexture);
        env_texture.glTexture = 0;
    }

    Hd_RUZINO_Light::Finalize(renderParam);
}

RUZINO_NAMESPACE_CLOSE_SCOPE
