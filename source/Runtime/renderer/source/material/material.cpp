#include "material.h"

#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialNetwork2Interface.h>
#include <pxr/imaging/hio/image.h>

#include "nvrhi/nvrhi.h"
#include "pxr/base/arch/fileSystem.h"
#include "pxr/imaging/hd/changeTracker.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/sdr/registry.h"
RUZINO_NAMESPACE_OPEN_SCOPE

std::mutex Hd_RUZINO_Material::texture_mutex;
std::mutex Hd_RUZINO_Material::material_data_handle_mutex;

Hd_RUZINO_Material::Hd_RUZINO_Material(SdfPath const& id) : HdMaterial(id)
{
}

HdMaterialNetwork2Interface Hd_RUZINO_Material::FetchNetInterface(
    HdSceneDelegate* sceneDelegate,
    HdMaterialNetwork2& hdNetwork,
    SdfPath& materialPath)
{
    VtValue material = sceneDelegate->GetMaterialResource(GetId());
    HdMaterialNetworkMap networkMap = material.Get<HdMaterialNetworkMap>();

    bool isVolume;
    hdNetwork = HdConvertToHdMaterialNetwork2(networkMap, &isVolume);

    materialPath = GetId();

    HdMaterialNetwork2Interface netInterface =
        HdMaterialNetwork2Interface(materialPath, &hdNetwork);
    return netInterface;
}

HdDirtyBits Hd_RUZINO_Material::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::AllDirty;
}

void Hd_RUZINO_Material::Finalize(HdRenderParam* renderParam)
{
    auto render_param = static_cast<Hd_RUZINO_RenderParam*>(renderParam);
    render_param->InstanceCollection->mark_materials_dirty();
    material_header_handle = nullptr;
    HdMaterial::Finalize(renderParam);
}
void Hd_RUZINO_Material::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits)
{
    // Ensure material data handle is allocated
    auto render_param = static_cast<Hd_RUZINO_RenderParam*>(renderParam);
    render_param->InstanceCollection->mark_materials_dirty();
}

void Hd_RUZINO_Material::ensure_material_data_handle(
    Hd_RUZINO_RenderParam* render_param)
{
    std::lock_guard<std::mutex> lock(material_data_handle_mutex);
    if (!material_data_handle) {
        if (!render_param) {
            throw std::runtime_error("Render param is null.");
        }

        material_header_handle =
            render_param->InstanceCollection->material_header_pool.allocate(1);

        material_data_handle =
            render_param->InstanceCollection->material_pool.allocate(1);

        MaterialHeader header;
        header.material_blob_id = material_data_handle->index();
        header.material_type_id = material_header_handle->index();
        material_header_handle->write_data(&header);
    }
}

unsigned Hd_RUZINO_Material::GetMaterialLocation() const
{
    if (!material_data_handle) {
        return -1;
    }
    return material_header_handle->index();
}

// HLSL callable shader
std::string Hd_RUZINO_Material::eval_source_code_fallback = R"(
void fetch_shader_data(
    out float4 out1,
    in uint material_params_index,
    inout uint shader_type_id,
    in MaterialDataBlob data,
    in VertexInfo vertexInfo
    )
{
    shader_type_id = 2; // Fallback shader type id
    out1 = float4(1.0, 1.0, 1.0, 1.0);
}

void fetch_shader_opacity(
    inout uint material_params_index,
    inout uint shader_type_id,
    inout float3 opacityColor,
    in MaterialDataBlob data,
    in VertexInfo vertexInfo
    )
{
    shader_type_id = 2; // Fallback shader type id
    // Fallback: fully opaque
    material_params_index = asuint(1.0f);
    opacityColor = float3(1.0, 1.0, 1.0);
}
)";

std::string Hd_RUZINO_Material::slang_source_code_template = R"(
import Scene.VertexInfo;
import Scene.BindlessMaterial;
import Scene.MaterialParams;
import utils.Math.ShadingFrame;

struct FetchCallableData {
    uint materialBlobID;
    uint material_params_index; // Index into material parameters buffer, set by data fetch callable. Or you can reinterpret it as opacity.
    uint shader_type_id;
    VertexInfo vertexInfo;
    float3 opacityColor; // Color3 opacity for tinted presence pass-through (set by opacity fetch callable)
};

[shader("callable")]
void $getColor(inout FetchCallableData data)
{
    float4 placeholder_color = float4(1.0);
    MaterialDataBlob blob_data = materialBlobBuffer[data.materialBlobID];
    fetch_shader_data(placeholder_color, data.material_params_index, data.shader_type_id, blob_data, data.vertexInfo);
}

[shader("callable")]
void $getOpacity(inout FetchCallableData data)
{
    MaterialDataBlob blob_data = materialBlobBuffer[data.materialBlobID];
    fetch_shader_opacity(data.material_params_index, data.shader_type_id, data.opacityColor, blob_data, data.vertexInfo);
}

)";

void Hd_RUZINO_Material::ensure_shader_ready(const ShaderFactory& factory)
{
    // Check if shader is already ready to avoid redundant generation
    if (shader_ready) {
        return;
    }

    // Use fallback shader if no source is available
    std::string local_slang_source_code{};
    if (material_name.empty()) {
        material_name = "fallback";
        local_slang_source_code = eval_source_code_fallback;
    }

    slang_source_code_main =
        local_slang_source_code + slang_source_code_template;

    // Replace the callable function name with the material name in all code
    constexpr char FUNC_PLACEHOLDER[] = "$getColor";
    constexpr char OPACITY_PLACEHOLDER[] = "$getOpacity";

    // Replace $getColor with material_name
    auto pos = slang_source_code_main.find(FUNC_PLACEHOLDER);
    if (pos != std::string::npos) {
        slang_source_code_main.replace(
            pos, strlen(FUNC_PLACEHOLDER), material_name);
    }

    // Replace $getOpacity with material_name_opacity
    pos = slang_source_code_main.find(OPACITY_PLACEHOLDER);
    if (pos != std::string::npos) {
        slang_source_code_main.replace(
            pos, strlen(OPACITY_PLACEHOLDER), material_name + "_opacity");
    }

    // No longer appending eval code - that's in shared callables
    final_shader_source = slang_source_code_main;

    // Increment shader generation to signal change
    shader_generation++;
    shader_ready = true;
}

std::string Hd_RUZINO_Material::GetShader(const ShaderFactory& factory)
{
    ensure_shader_ready(factory);
    return final_shader_source;
}
std::string Hd_RUZINO_Material::GetMaterialName() const
{
    return material_name;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
