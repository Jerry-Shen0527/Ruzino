//
// Copyright 2020 Pixar
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
//
// #define __GNUC__
#include "mesh.h"

#include <spdlog/spdlog.h>

#include "../instancer.h"
#include "../renderParam.h"
#include "Scene/SceneTypes.slang"
#include "material/material.h"
#include "nvrhi/utils.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/imaging/hd/extComputationUtils.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/smoothNormals.h"

RUZINO_NAMESPACE_OPEN_SCOPE
class Hd_RUZINO_RenderParam;
using namespace pxr;
Hd_RUZINO_Mesh::Hd_RUZINO_Mesh(const SdfPath& id)
    : HdMesh(id),
      _cullStyle(HdCullStyleDontCare),
      _doubleSided(false),
      _normalsValid(false),
      _adjacencyValid(false),
      _normalInterp(HdInterpolationVertex),
      _refined(false)
{
}

Hd_RUZINO_Mesh::~Hd_RUZINO_Mesh()
{
}

HdDirtyBits Hd_RUZINO_Mesh::GetInitialDirtyBitsMask() const
{
    int mask =
        HdChangeTracker::Clean | HdChangeTracker::InitRepr |
        HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTopology |
        HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
        HdChangeTracker::DirtyCullStyle | HdChangeTracker::DirtyDoubleSided |
        HdChangeTracker::DirtyDisplayStyle | HdChangeTracker::DirtySubdivTags |
        HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyNormals |
        HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyMaterialId;

    return (HdDirtyBits)mask;
}

HdDirtyBits Hd_RUZINO_Mesh::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

TfTokenVector Hd_RUZINO_Mesh::_UpdateComputedPrimvarSources(
    HdSceneDelegate* sceneDelegate,
    HdDirtyBits dirtyBits)
{
    HD_TRACE_FUNCTION();

    const SdfPath& id = GetId();

    // Get all the dirty computed primvars
    HdExtComputationPrimvarDescriptorVector dirtyCompPrimvars;
    for (size_t i = 0; i < HdInterpolationCount; ++i) {
        HdExtComputationPrimvarDescriptorVector compPrimvars;
        auto interp = static_cast<HdInterpolation>(i);
        compPrimvars =
            sceneDelegate->GetExtComputationPrimvarDescriptors(GetId(), interp);

        for (const auto& pv : compPrimvars) {
            if (HdChangeTracker::IsPrimvarDirty(dirtyBits, id, pv.name)) {
                dirtyCompPrimvars.emplace_back(pv);
            }
        }
    }

    if (dirtyCompPrimvars.empty()) {
        return TfTokenVector();
    }

    HdExtComputationUtils::ValueStore valueStore =
        HdExtComputationUtils::GetComputedPrimvarValues(
            dirtyCompPrimvars, sceneDelegate);

    TfTokenVector compPrimvarNames;
    // Update local primvar map and track the ones that were computed
    for (const auto& compPrimvar : dirtyCompPrimvars) {
        const auto it = valueStore.find(compPrimvar.name);
        if (!TF_VERIFY(it != valueStore.end())) {
            continue;
        }

        compPrimvarNames.emplace_back(compPrimvar.name);
        _primvarSourceMap[compPrimvar.name] = { it->second,
                                                compPrimvar.interpolation };
    }

    return compPrimvarNames;
}

void Hd_RUZINO_Mesh::_UpdatePrimvarSources(
    HdSceneDelegate* sceneDelegate,
    HdDirtyBits dirtyBits,
    HdRenderParam* param)
{
    HD_TRACE_FUNCTION();
    const SdfPath& id = GetId();

    HdPrimvarDescriptorVector primvars;
    for (size_t i = 0; i < HdInterpolationCount; ++i) {
        auto interp = static_cast<HdInterpolation>(i);
        primvars = GetPrimvarDescriptors(sceneDelegate, interp);
        for (const HdPrimvarDescriptor& pv : primvars) {
            if (HdChangeTracker::IsPrimvarDirty(dirtyBits, id, pv.name) &&
                pv.name != HdTokens->points) {
                _primvarSourceMap[pv.name] = {
                    GetPrimvar(sceneDelegate, pv.name), interp
                };
            }
        }
    }
}

void Hd_RUZINO_Mesh::create_gpu_resources(Hd_RUZINO_RenderParam* render_param)
{
    auto device = RHI::get_device();

    if (!copy_commandlist)
        copy_commandlist =
            device->createCommandList({ .enableImmediateExecution = false });

    auto descriptor_table =
        render_param->InstanceCollection->get_buffer_descriptor_table();

    size_t index_buffer_offset = 0;
    size_t normal_buffer_offset = 0;
    size_t tangent_buffer_offset = 0;
    size_t texcoord_buffer_offset = 0;
    size_t subset_mat_id_offset = 0;

    // Helper lambda to conditionally add buffer data and update offset
    auto add_buffer_offset =
        [](size_t& offset, size_t& total_size, size_t data_size) {
            if (data_size > 0) {
                offset = total_size;
                total_size += data_size;
            }
            else {
                offset = 0;  // Signal shader that data doesn't exist
            }
        };

    size_t total_buffer_size = points.size() * 3 * sizeof(float);
    index_buffer_offset = total_buffer_size;
    total_buffer_size += triangulatedIndices.size() * 3 * sizeof(uint);

    add_buffer_offset(
        normal_buffer_offset,
        total_buffer_size,
        normals.size() * 3 * sizeof(float));

    VtVec2fArray texcoords;
    InterpolationType texCrdInterpolation = InterpolationType::Vertex;
    for (auto pv : _primvarSourceMap) {
        if (pv.first == pxr::TfToken("UVMap") ||
            pv.first == pxr::TfToken("st")) {
            texcoords = pv.second.data.Get<VtVec2fArray>();
            if (pv.second.interpolation == HdInterpolationFaceVarying) {
                texCrdInterpolation = InterpolationType::FaceVarying;
            }
            else {
                texCrdInterpolation = InterpolationType::Vertex;
            }
        }
    }

    add_buffer_offset(
        tangent_buffer_offset,
        total_buffer_size,
        tangents.size() * 4 * sizeof(float));
    add_buffer_offset(
        texcoord_buffer_offset,
        total_buffer_size,
        texcoords.size() * 2 * sizeof(float));

    VtArray<unsigned> subset_material_id;

    if (_primvarSourceMap.find(pxr::TfToken("subset_material_id")) !=
        _primvarSourceMap.end()) {
        subset_mat_id_offset = total_buffer_size;
        subset_material_id =
            _primvarSourceMap[pxr::TfToken("subset_material_id")]
                .data.Get<VtArray<unsigned>>();
        total_buffer_size +=
            _primvarSourceMap[pxr::TfToken("subset_material_id")]
                .data.Get<VtArray<unsigned>>()
                .size() *
            sizeof(int);
    }

    nvrhi::BufferDesc desc =
        nvrhi::BufferDesc{}
            .setCanHaveRawViews(true)
            .setByteSize(total_buffer_size)
            .setIsVertexBuffer(true)
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setCpuAccess(nvrhi::CpuAccessMode::None)
            .setIsAccelStructBuildInput(true)
            .setKeepInitialState(true)
            .setDebugName("vertexBuffer");

    vertexBuffer = device->createBuffer(desc);

    copy_commandlist->open();

    copy_commandlist->writeBuffer(
        vertexBuffer, points.data(), points.size() * 3 * sizeof(float), 0);

    copy_commandlist->writeBuffer(
        vertexBuffer,
        triangulatedIndices.data(),
        triangulatedIndices.size() * 3 * sizeof(uint),
        index_buffer_offset);

    if (!normals.empty()) {
        copy_commandlist->writeBuffer(
            vertexBuffer,
            normals.data(),
            normals.size() * 3 * sizeof(float),
            normal_buffer_offset);
    }

    if (!tangents.empty()) {
        copy_commandlist->writeBuffer(
            vertexBuffer,
            tangents.data(),
            tangents.size() * 4 * sizeof(float),
            tangent_buffer_offset);
    }

    if (!texcoords.empty()) {
        copy_commandlist->writeBuffer(
            vertexBuffer,
            texcoords.data(),
            texcoords.size() * 2 * sizeof(float),
            texcoord_buffer_offset);
    }
    if (!subset_material_id.empty()) {
        copy_commandlist->writeBuffer(
            vertexBuffer,
            subset_material_id.data(),
            subset_material_id.size() * sizeof(int),
            subset_mat_id_offset);
    }

    copy_commandlist->close();

    {
        std::lock_guard lock(execution_launch_mutex);
        device->executeCommandList(copy_commandlist);

        nvrhi::rt::AccelStructDesc blas_desc;
        nvrhi::rt::GeometryDesc geometry_desc;
        geometry_desc.geometryType = nvrhi::rt::GeometryType::Triangles;
        nvrhi::rt::GeometryTriangles triangles;
        triangles.setVertexBuffer(vertexBuffer)
            .setVertexOffset(0)
            .setIndexBuffer(vertexBuffer)
            .setIndexOffset(index_buffer_offset)
            .setIndexCount(triangulatedIndices.size() * 3)
            .setVertexCount(points.size())
            .setVertexStride(3 * sizeof(float))
            .setVertexFormat(nvrhi::Format::RGB32_FLOAT)
            .setIndexFormat(nvrhi::Format::R32_UINT);
        geometry_desc.setTriangles(triangles);
        blas_desc.addBottomLevelGeometry(geometry_desc);
        blas_desc.isTopLevel = false;
        BLAS = device->createAccelStruct(blas_desc);

        copy_commandlist->open();
        nvrhi::utils::BuildBottomLevelAccelStruct(
            copy_commandlist, BLAS, blas_desc);
        copy_commandlist->close();
        device->executeCommandList(copy_commandlist);
        device->waitForIdle();

        descriptor_handle = descriptor_table->CreateDescriptorHandle(
            nvrhi::BindingSetItem::RawBuffer_SRV(0, vertexBuffer.Get()));
    }

    MeshDesc mesh_desc;
    mesh_desc.vbOffset = 0;
    mesh_desc.ibOffset = index_buffer_offset;
    mesh_desc.normalOffset = normal_buffer_offset;
    mesh_desc.tangentOffset = tangent_buffer_offset;
    mesh_desc.texCrdOffset = texcoord_buffer_offset;
    mesh_desc.subsetMatIdOffset = subset_mat_id_offset;
    mesh_desc.bindlessIndex = descriptor_handle.Get();

    mesh_desc.texCrdInterpolation = texCrdInterpolation;
    mesh_desc.normalInterpolation =
        (_normalInterp == HdInterpolationFaceVarying)
            ? InterpolationType::FaceVarying
            : InterpolationType::Vertex;
    mesh_desc.tangentInterpolation = tangents.size() == points.size()
                                         ? InterpolationType::Vertex
                                         : InterpolationType::FaceVarying;

    mesh_desc_buffer = render_param->InstanceCollection->mesh_pool.allocate(1);
    mesh_desc_buffer->write_data(&mesh_desc);
}

void Hd_RUZINO_Mesh::updateTLAS(
    Hd_RUZINO_RenderParam* render_param,
    HdSceneDelegate* sceneDelegate,
    HdDirtyBits* dirtyBits)
{
    _UpdateInstancer(sceneDelegate, dirtyBits);
    const SdfPath& id = GetId();

    HdInstancer::_SyncInstancerAndParents(
        sceneDelegate->GetRenderIndex(), GetInstancerId());

    auto material_id = GetMaterialId();

    // Check if mesh has GeomSubset materials
    bool has_subset_materials =
        _primvarSourceMap.find(TfToken("subset_material_id")) !=
        _primvarSourceMap.end();

    if (material_id.IsEmpty()) {
        if (!has_subset_materials) {
            spdlog::info(
                "Mesh {} has no material assigned. Using default material.",
                id.GetText());
        }
        else {
            spdlog::info(
                "Mesh {} uses GeomSubset materials only.", id.GetText());
        }
    }
    else {
        spdlog::info(
            "Mesh {} has material {} assigned{}.",
            id.GetText(),
            material_id.GetText(),
            has_subset_materials ? " with GeomSubset overrides" : "");
    }

    Hd_RUZINO_Material* material = nullptr;
    if (!material_id.IsEmpty()) {
        material = (*render_param->material_map)[material_id];
    }
    if (!material && !has_subset_materials && !material_id.IsEmpty()) {
        spdlog::warn(
            "Material {} not found for mesh {}. Using default material.",
            material_id.GetText(),
            id.GetText());
    }

    size_t instance_count = 1;

    // Determine instance count
    if (!GetInstancerId().IsEmpty()) {
        VtIntArray instanceIndices =
            sceneDelegate->GetInstanceIndices(GetInstancerId(), GetId());
        instance_count = instanceIndices.size();
        spdlog::info(
            "Mesh {} has instancer {} with {} instances",
            id.GetText(),
            GetInstancerId().GetText(),
            instance_count);
    }
    else {
        spdlog::info(
            "Mesh {} has no instancer, using single instance", id.GetText());
    }

    auto& rt_instance_pool = render_param->InstanceCollection->rt_instance_pool;

    if (!rt_instanceBuffer || rt_instanceBuffer->count() != instance_count)
        rt_instanceBuffer = rt_instance_pool.allocate(instance_count);

    if (!instanceBuffer || instanceBuffer->count() != instance_count)
        instanceBuffer =
            render_param->InstanceCollection->instance_pool.allocate(
                instance_count);

    if (material) {
        material->ensure_material_data_handle(render_param);
    }

    int material_location = material ? material->GetMaterialLocation() : -1;
    if (!GetInstancerId().IsEmpty()) {
        // GPU path: Let instancer compute transforms on GPU
        HdRenderIndex& renderIndex = sceneDelegate->GetRenderIndex();
        HdInstancer* instancer = renderIndex.GetInstancer(GetInstancerId());
        static_cast<Hd_RUZINO_Instancer*>(instancer)->ComputeInstanceTransforms(
            GetId(),
            instanceBuffer,
            rt_instanceBuffer,
            BLAS->getDeviceAddress(),
            transform,
            material_location,
            mesh_desc_buffer->index());
    }
    else {
        // CPU path: Single instance, no instancer
        GeometryInstanceData instance_data;
        instance_data.geometryID = mesh_desc_buffer->index();
        instance_data.materialID = material_location;
        memcpy(
            &instance_data.transform,
            transform.data(),
            sizeof(pxr::GfMatrix4f));
        instance_data.flags = 0;

        instanceBuffer->write_data(&instance_data);

        nvrhi::rt::InstanceDesc rt_instance;
        rt_instance.blasDeviceAddress = BLAS->getDeviceAddress();
        rt_instance.instanceMask = 1;
        rt_instance.flags = nvrhi::rt::InstanceFlags::None;

        GfMatrix4f mat_transposed = transform.GetTranspose();
        memcpy(
            rt_instance.transform,
            mat_transposed.data(),
            sizeof(nvrhi::rt::AffineTransform));
        rt_instance.instanceID = instanceBuffer->index();

        rt_instanceBuffer->write_data(&rt_instance);
    }

    render_param->InstanceCollection->set_require_rebuild_tlas();

    draw_indirect =
        render_param->InstanceCollection->draw_indirect_pool.allocate(1);
    nvrhi::DrawIndirectArguments args;

    args.vertexCount = triangulatedIndices.size() * 3;
    args.instanceCount = instance_count;
    args.startVertexLocation = 0;
    args.startInstanceLocation = instanceBuffer->index();

    draw_indirect->write_data(&args);

    // Register emissive geometry for NEE sampling. This registers only
    // lightweight metadata (instanceSlot, meshDescIndex, triangleCount,
    // materialLocation) — the GPU compute pass reads the actual vertex/index
    // data from the mesh's existing bindless vertexBuffer (zero CPU copies).
    // We always register so material changes (non-emissive -> emissive) are
    // picked up; the actual isEmissive() filtering happens at build time.
    updateEmissiveEntry(render_param);
}

void Hd_RUZINO_Mesh::updateEmissiveEntry(Hd_RUZINO_RenderParam* render_param)
{
    if (!instanceBuffer)
        return;

    // If the mesh has no triangles, deregister any previous entry.
    if (triangulatedIndices.empty()) {
        render_param->InstanceCollection->emissive_registry.deregister_mesh(
            instanceBuffer->index());
        return;
    }

    EmissiveMeshEntry entry;
    entry.instanceSlot = instanceBuffer->index();
    entry.meshDescIndex = mesh_desc_buffer->index();
    entry.triangleCount = static_cast<uint32_t>(triangulatedIndices.size());

    // Resolve default material location (mesh-level).
    auto material_id = GetMaterialId();
    auto material_map = render_param->material_map;
    entry.defaultMaterialLocation = 0xffffffffu;
    if (!material_id.IsEmpty() && material_map) {
        auto it = material_map->find(material_id);
        if (it != material_map->end() && it->second) {
            entry.defaultMaterialLocation = it->second->GetMaterialLocation();
        }
    }

    render_param->InstanceCollection->emissive_registry.register_mesh(
        instanceBuffer->index(), std::move(entry));
}

void Hd_RUZINO_Mesh::_InitRepr(const TfToken& reprToken, HdDirtyBits* dirtyBits)
{
}

void Hd_RUZINO_Mesh::_SetMaterialId(
    HdSceneDelegate* delegate,
    Hd_RUZINO_Mesh* rprim)
{
    SdfPath const& newMaterialId = delegate->GetMaterialId(rprim->GetId());
    if (rprim->GetMaterialId() != newMaterialId) {
        rprim->SetMaterialId(newMaterialId);
    }
}

void Hd_RUZINO_Mesh::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits,
    const TfToken& reprToken)
{
    _dirtyBits = *dirtyBits;
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    _MeshReprConfig::DescArray descs = _GetReprDesc(reprToken);

    const SdfPath& id = GetId();
    std::string path = id.GetText();

    if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id)) {
        _sharedData.visible = sceneDelegate->GetVisible(id);
    }

    if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
        _SetMaterialId(sceneDelegate, this);
        // Notify renderer that material assignments changed so path tracing
        // rebuilds the shader pipeline.
        auto render_param = static_cast<Hd_RUZINO_RenderParam*>(renderParam);
        render_param->InstanceCollection->mark_materials_dirty();
    }

    bool requires_rebuild_blas =
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points) ||
        HdChangeTracker::IsTopologyDirty(*dirtyBits, id);

    bool requires_rebuild_tlas =
        requires_rebuild_blas ||
        HdChangeTracker::IsInstancerDirty(*dirtyBits, id) ||
        HdChangeTracker::IsTransformDirty(*dirtyBits, id) ||
        HdChangeTracker::IsVisibilityDirty(*dirtyBits, id) ||
        (*dirtyBits & HdChangeTracker::DirtyMaterialId);

    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)) {
        VtValue value = sceneDelegate->Get(id, HdTokens->points);
        points = value.Get<VtVec3fArray>();

        _normalsValid = false;
    }

    if (!points.empty()) {
        if (HdChangeTracker::IsPrimvarDirty(
                *dirtyBits, id, HdTokens->normals) ||
            HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->widths) ||
            HdChangeTracker::IsPrimvarDirty(
                *dirtyBits, id, HdTokens->primvar)) {
            _UpdatePrimvarSources(sceneDelegate, *dirtyBits, renderParam);
        }

        if (HdChangeTracker::IsTopologyDirty(*dirtyBits, id)) {
            topology = GetMeshTopology(sceneDelegate);

            HdMeshUtil meshUtil(&topology, GetId());
            meshUtil.ComputeTriangleIndices(
                &triangulatedIndices, &trianglePrimitiveParams);

            auto& geom_subsets = topology.GetGeomSubsets();

            // Triangulate all FaceVarying primvars including normals
            for (auto& primvar : _primvarSourceMap) {
                if (primvar.second.interpolation ==
                    HdInterpolationFaceVarying) {
                    VtValue value = primvar.second.data;

                    if (value.IsArrayValued()) {
                        size_t original_size = value.GetArraySize();
                        size_t expected_face_vertex_size =
                            topology.GetFaceVertexIndices().size();

                        spdlog::info(
                            "Mesh {}: Triangulating FaceVarying primvar '{}' - "
                            "original size: {}, face vertex indices: {}",
                            GetId().GetText(),
                            primvar.first.GetText(),
                            original_size,
                            expected_face_vertex_size);

                        if (value.IsHolding<VtVec3fArray>()) {
                            if (original_size != expected_face_vertex_size) {
                                spdlog::error(
                                    "FaceVarying primvar size mismatch: {}, "
                                    "expected {}, have {}",
                                    primvar.first.GetText(),
                                    expected_face_vertex_size,
                                    original_size);
                            }
                            (void)
                                meshUtil.ComputeTriangulatedFaceVaryingPrimvar(
                                    value.Get<VtVec3fArray>().data(),
                                    value.GetArraySize(),
                                    HdTypeFloatVec3,
                                    &primvar.second.data);
                        }
                        else if (value.IsHolding<VtVec2fArray>()) {
                            if (original_size != expected_face_vertex_size) {
                                spdlog::error(
                                    "FaceVarying primvar size mismatch: {}, "
                                    "expected {}, have {}",
                                    primvar.first.GetText(),
                                    expected_face_vertex_size,
                                    original_size);
                            }
                            (void)
                                meshUtil.ComputeTriangulatedFaceVaryingPrimvar(
                                    value.Get<VtVec2fArray>().data(),
                                    value.GetArraySize(),
                                    HdTypeFloatVec2,
                                    &primvar.second.data);
                        }
                        else if (value.IsHolding<VtVec4fArray>()) {
                            if (original_size != expected_face_vertex_size) {
                                spdlog::error(
                                    "FaceVarying primvar size mismatch: {}, "
                                    "expected {}, have {}",
                                    primvar.first.GetText(),
                                    expected_face_vertex_size,
                                    original_size);
                            }
                            spdlog::info(
                                "Get a VtVec4fArray, named {}",
                                primvar.first.GetText());
                            (void)
                                meshUtil.ComputeTriangulatedFaceVaryingPrimvar(
                                    value.Get<VtVec4fArray>().data(),
                                    value.GetArraySize(),
                                    HdTypeFloatVec4,
                                    &primvar.second.data);
                        }

                        size_t triangulated_size =
                            primvar.second.data.GetArraySize();
                        size_t expected_triangulated_size =
                            triangulatedIndices.size() * 3;

                        if (triangulated_size != expected_triangulated_size) {
                            spdlog::error(
                                "FaceVarying primvar size mismatch after "
                                "triangulation: {}, "
                                "expected {}, have {}",
                                primvar.first.GetText(),
                                expected_triangulated_size,
                                triangulated_size);
                        }
                        else {
                            spdlog::info(
                                "Mesh {}: Successfully triangulated primvar "
                                "'{}' - "
                                "result size: {}",
                                GetId().GetText(),
                                primvar.first.GetText(),
                                triangulated_size);
                        }
                    }
                }
            }

            if (!geom_subsets.empty()) {
                std::unordered_map<int, int> subset_material_id_map;
                auto material_map =
                    static_cast<Hd_RUZINO_RenderParam*>(renderParam)
                        ->material_map;

                // Get mesh-level material for faces not in any subset
                auto mesh_material_id = GetMaterialId();
                int default_material_loc = -1;
                if (!mesh_material_id.IsEmpty()) {
                    auto p = material_map->find(mesh_material_id);
                    if (p != material_map->end() && (*p).second) {
                        default_material_loc =
                            (*p).second->GetMaterialLocation();
                    }
                }

                for (auto& subset : geom_subsets) {
                    auto face_ids = subset.indices;
                    auto p = material_map->find(subset.materialId);

                    if (p == material_map->end() || !(*p).second) {
                        spdlog::error(
                            "Material {} not found for subset in mesh {}. "
                            "Skipping subset.",
                            subset.materialId.GetText(),
                            id.GetText());
                        continue;
                    }

                    auto material_id = (*p).second->GetMaterialLocation();
                    for (auto face_id : face_ids) {
                        subset_material_id_map[face_id] = material_id;
                    }
                }

                // Initialize with max uint to indicate "use instance material"
                // (equivalent to -1 in signed)
                VtArray<unsigned> material_id_primvars;
                material_id_primvars.resize(
                    triangulatedIndices.size() * 3, static_cast<unsigned>(-1));

                assert(
                    triangulatedIndices.size() ==
                    trianglePrimitiveParams.size());

                for (int i = 0; i < triangulatedIndices.size(); i++) {
                    int face_index =
                        HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(
                            trianglePrimitiveParams[i]);

                    // Check if this face has a subset material
                    auto it = subset_material_id_map.find(face_index);
                    unsigned mat_id =
                        (it != subset_material_id_map.end())
                            ? static_cast<unsigned>(it->second)
                            : static_cast<unsigned>(default_material_loc);

                    material_id_primvars[i * 3] = mat_id;
                    material_id_primvars[i * 3 + 1] = mat_id;
                    material_id_primvars[i * 3 + 2] = mat_id;
                }

                _primvarSourceMap[TfToken("subset_material_id")] = {
                    VtValue(material_id_primvars), HdInterpolationFaceVarying
                };
            }

            _normalsValid = false;
            _adjacencyValid = false;
        }
        if (HdChangeTracker::IsInstancerDirty(*dirtyBits, id) ||
            HdChangeTracker::IsTransformDirty(*dirtyBits, id)) {
            // TODO: fill instance matrix buffe
            // r
            transform = GfMatrix4f(sceneDelegate->GetTransform(id));
        }

        if (!_normalsValid) {
            VtValue normal_primvar;
            HdInterpolation normal_interp = HdInterpolationVertex;

            if (_primvarSourceMap.find(HdTokens->normals) !=
                _primvarSourceMap.end()) {
                normal_primvar = _primvarSourceMap[HdTokens->normals].data;
                normal_interp =
                    _primvarSourceMap[HdTokens->normals].interpolation;
                spdlog::info(
                    "Mesh {}: Found normals in primvar map - size: {}, "
                    "interpolation: {}",
                    GetId().GetText(),
                    normal_primvar.GetArraySize(),
                    normal_interp == HdInterpolationFaceVarying ? "FaceVarying"
                                                                : "Vertex");
            }
            else {
                normal_primvar = GetNormals(sceneDelegate);
                spdlog::info(
                    "Mesh {}: GetNormals from sceneDelegate - size: {}",
                    GetId().GetAsString().c_str(),
                    normal_primvar.GetArraySize());
            }

            if (normal_primvar.IsEmpty() ||
                (normal_primvar.IsArrayValued() &&
                 normal_primvar.GetArraySize() == 1)) {
                // If there are no normals authored, we need to compute
                // them. This is the case for example when the normals
                // are not authored in the USD file, but are computed by
                // the renderer. We compute the normals here and store
                // them in the normals member variable.

                if (!_adjacencyValid) {
                    _adjacency.BuildAdjacencyTable(&topology);
                    _adjacencyValid = true;
                    // If we rebuilt the adjacency table, force a
                    // rebuild of normals.
                    _normalsValid = false;
                }
                normals = Hd_SmoothNormals::ComputeSmoothNormals(
                    &_adjacency, points.size(), points.cdata());
                assert(points.size() == normals.size());
                normal_interp = HdInterpolationVertex;
                spdlog::info(
                    "Mesh {}: Computed smooth normals - size: {}",
                    GetId().GetText(),
                    normals.size());
            }
            else {
                // If normals are authored, we use them.
                normals = normal_primvar.Get<VtVec3fArray>();

                // Handle FaceVarying normals - they should already have been
                // triangulated in the topology update loop above with other
                // primvars Check if they were already processed
                if (normal_interp == HdInterpolationFaceVarying) {
                    // The normals should have been triangulated when we
                    // processed all FaceVarying primvars above. Verify the
                    // size.
                    size_t expected_size = triangulatedIndices.size() * 3;

                    if (normals.size() == expected_size) {
                        // Already triangulated - good!
                        spdlog::info(
                            "Mesh {}: FaceVarying normals already triangulated "
                            "- "
                            "size: {}",
                            GetId().GetText(),
                            normals.size());
                    }
                    else if (
                        normals.size() ==
                        topology.GetFaceVertexIndices().size()) {
                        // Need to triangulate
                        spdlog::info(
                            "Mesh {}: Triangulating FaceVarying normals - "
                            "from size: {} to expected: {}",
                            GetId().GetText(),
                            normals.size(),
                            expected_size);

                        HdMeshUtil meshUtil(&topology, GetId());
                        VtValue triangulated_normals;
                        (void)meshUtil.ComputeTriangulatedFaceVaryingPrimvar(
                            normals.data(),
                            normals.size(),
                            HdTypeFloatVec3,
                            &triangulated_normals);
                        normals = triangulated_normals.Get<VtVec3fArray>();

                        spdlog::info(
                            "Mesh {}: Triangulated FaceVarying normals - "
                            "result size: {}",
                            GetId().GetText(),
                            normals.size());
                    }
                    else if (normals.size() == points.size()) {
                        // Size matches vertices - treat as Vertex interpolation
                        normal_interp = HdInterpolationVertex;
                        spdlog::info(
                            "Mesh {}: FaceVarying normals size matches "
                            "vertices, "
                            "treating as Vertex interpolation - size: {}",
                            GetId().GetText(),
                            normals.size());
                    }
                    else {
                        // Unexpected size
                        spdlog::error(
                            "Mesh {}: FaceVarying normals size mismatch - "
                            "normals: {}, faceVertexIndices: {}, triangulated "
                            "expected: {}, vertices: {}",
                            GetId().GetText(),
                            normals.size(),
                            topology.GetFaceVertexIndices().size(),
                            expected_size,
                            points.size());
                    }
                }
                else {
                    // Vertex interpolation - no triangulation needed
                    spdlog::info(
                        "Mesh {}: Using Vertex normals - size: {}",
                        GetId().GetText(),
                        normals.size());
                }
            }

            // Store normal interpolation type
            _normalInterp = normal_interp;
            _normalsValid = true;
        }

        // Compute tangents if we have normals and texture coordinates
        if (!normals.empty() && !points.empty() &&
            !triangulatedIndices.empty()) {
            // Get texture coordinates from primvar map
            VtVec2fArray texcoords;
            HdInterpolation texcoord_interp = HdInterpolationVertex;
            for (auto& pv : _primvarSourceMap) {
                if (pv.first == pxr::TfToken("UVMap") ||
                    pv.first == pxr::TfToken("st")) {
                    texcoords = pv.second.data.Get<VtVec2fArray>();
                    texcoord_interp = pv.second.interpolation;
                    break;
                }
            }

            if (!texcoords.empty()) {
                // Debug: log texcoord info
                spdlog::info(
                    "Mesh {}: Computing tangents - points: {}, triangles: {}, "
                    "texcoords: {}, interp: {}",
                    GetId().GetText(),
                    points.size(),
                    triangulatedIndices.size(),
                    texcoords.size(),
                    texcoord_interp == HdInterpolationFaceVarying
                        ? "FaceVarying"
                        : "Vertex");

                // Compute tangents with same interpolation as texcoords
                if (texcoord_interp == HdInterpolationFaceVarying) {
                    // FaceVarying: one tangent per triangle vertex (no
                    // accumulation at UV seams)
                    tangents.resize(triangulatedIndices.size() * 3);

                    for (size_t triIdx = 0; triIdx < triangulatedIndices.size();
                         triIdx++) {
                        uint32_t i0 = triangulatedIndices[triIdx][0];
                        uint32_t i1 = triangulatedIndices[triIdx][1];
                        uint32_t i2 = triangulatedIndices[triIdx][2];

                        GfVec3f v0 = points[i0];
                        GfVec3f v1 = points[i1];
                        GfVec3f v2 = points[i2];

                        GfVec2f uv0 = texcoords[triIdx * 3 + 0];
                        GfVec2f uv1 = texcoords[triIdx * 3 + 1];
                        GfVec2f uv2 = texcoords[triIdx * 3 + 2];

                        GfVec3f n0, n1, n2;
                        if (normals.size() == triangulatedIndices.size() * 3) {
                            // FaceVarying normals
                            n0 = normals[triIdx * 3 + 0];
                            n1 = normals[triIdx * 3 + 1];
                            n2 = normals[triIdx * 3 + 2];
                        }
                        else if (
                            normals.size() == points.size() &&
                            i0 < normals.size() && i1 < normals.size() &&
                            i2 < normals.size()) {
                            // Vertex normals - with bounds check
                            n0 = normals[i0];
                            n1 = normals[i1];
                            n2 = normals[i2];
                        }
                        else {
                            // Fallback: compute face normal
                            GfVec3f edge1 = v1 - v0;
                            GfVec3f edge2 = v2 - v0;
                            GfVec3f faceNormal = GfCross(edge1, edge2);
                            float len = faceNormal.GetLength();
                            if (len > 1e-6f) {
                                faceNormal = faceNormal / len;
                            }
                            else {
                                faceNormal = GfVec3f(0.0f, 1.0f, 0.0f);
                            }
                            n0 = n1 = n2 = faceNormal;
                        }

                        GfVec3f deltaPos1 = v1 - v0;
                        GfVec3f deltaPos2 = v2 - v0;
                        GfVec2f deltaUV1 = uv1 - uv0;
                        GfVec2f deltaUV2 = uv2 - uv0;

                        float det = deltaUV1[0] * deltaUV2[1] -
                                    deltaUV1[1] * deltaUV2[0];

                        GfVec3f tangent, bitangent;
                        if (std::abs(det) > 1e-10f) {
                            float r = 1.0f / det;
                            tangent = (deltaPos1 * deltaUV2[1] -
                                       deltaPos2 * deltaUV1[1]) *
                                      r;
                            bitangent = (deltaPos2 * deltaUV1[0] -
                                         deltaPos1 * deltaUV2[0]) *
                                        r;
                        }
                        else {
                            // Degenerate UV triangle - use arbitrary tangent
                            tangent = GfVec3f(1.0f, 0.0f, 0.0f);
                            bitangent = GfVec3f(0.0f, 1.0f, 0.0f);
                        }

                        // Process each triangle vertex independently
                        for (int vtxIdx = 0; vtxIdx < 3; vtxIdx++) {
                            GfVec3f n = (vtxIdx == 0)   ? n0
                                        : (vtxIdx == 1) ? n1
                                                        : n2;
                            n.Normalize();

                            // Gram-Schmidt orthogonalize
                            GfVec3f t = tangent - n * GfDot(n, tangent);
                            float tLen = t.GetLength();

                            if (tLen > 1e-6f) {
                                t = t / tLen;
                            }
                            else {
                                // Tangent parallel to normal - create
                                // perpendicular
                                if (std::abs(n[0]) < 0.9f) {
                                    t = GfVec3f(1.0f, 0.0f, 0.0f);
                                }
                                else {
                                    t = GfVec3f(0.0f, 1.0f, 0.0f);
                                }
                                t = t - n * GfDot(n, t);
                                t.Normalize();
                            }

                            // Calculate handedness
                            GfVec3f calculatedBitangent = GfCross(n, t);
                            float handedness =
                                GfDot(calculatedBitangent, bitangent) >= 0.0f
                                    ? 1.0f
                                    : -1.0f;

                            tangents[triIdx * 3 + vtxIdx] =
                                GfVec4f(t[0], t[1], t[2], handedness);
                        }
                    }
                }
                else {
                    // Vertex interpolation: accumulate per vertex
                    tangents.resize(points.size());
                    std::vector<GfVec3f> bitangents(points.size());

                    for (size_t i = 0; i < tangents.size(); i++) {
                        tangents[i] = GfVec4f(0.0f, 0.0f, 0.0f, 0.0f);
                        bitangents[i] = GfVec3f(0.0f, 0.0f, 0.0f);
                    }

                    for (size_t triIdx = 0; triIdx < triangulatedIndices.size();
                         triIdx++) {
                        uint32_t i0 = triangulatedIndices[triIdx][0];
                        uint32_t i1 = triangulatedIndices[triIdx][1];
                        uint32_t i2 = triangulatedIndices[triIdx][2];

                        if (texcoords.size() <= std::max({ i0, i1, i2 }))
                            continue;

                        GfVec3f v0 = points[i0];
                        GfVec3f v1 = points[i1];
                        GfVec3f v2 = points[i2];
                        GfVec2f uv0 = texcoords[i0];
                        GfVec2f uv1 = texcoords[i1];
                        GfVec2f uv2 = texcoords[i2];

                        GfVec3f deltaPos1 = v1 - v0;
                        GfVec3f deltaPos2 = v2 - v0;
                        GfVec2f deltaUV1 = uv1 - uv0;
                        GfVec2f deltaUV2 = uv2 - uv0;

                        float det = deltaUV1[0] * deltaUV2[1] -
                                    deltaUV1[1] * deltaUV2[0];
                        if (std::abs(det) < 1e-10f)
                            continue;

                        float r = 1.0f / det;
                        GfVec3f tangent = (deltaPos1 * deltaUV2[1] -
                                           deltaPos2 * deltaUV1[1]) *
                                          r;
                        GfVec3f bitangent = (deltaPos2 * deltaUV1[0] -
                                             deltaPos1 * deltaUV2[0]) *
                                            r;

                        // Weight by triangle area
                        GfVec3f edge1 = v1 - v0;
                        GfVec3f edge2 = v2 - v0;
                        float area = GfCross(edge1, edge2).GetLength() * 0.5f;

                        tangents[i0] += GfVec4f(
                            tangent[0] * area,
                            tangent[1] * area,
                            tangent[2] * area,
                            0.0f);
                        tangents[i1] += GfVec4f(
                            tangent[0] * area,
                            tangent[1] * area,
                            tangent[2] * area,
                            0.0f);
                        tangents[i2] += GfVec4f(
                            tangent[0] * area,
                            tangent[1] * area,
                            tangent[2] * area,
                            0.0f);

                        bitangents[i0] += bitangent * area;
                        bitangents[i1] += bitangent * area;
                        bitangents[i2] += bitangent * area;
                    }

                    // Normalize and orthogonalize
                    for (size_t i = 0; i < tangents.size(); i++) {
                        GfVec3f n = normals[i];
                        n.Normalize();

                        GfVec3f t = GfVec3f(
                            tangents[i][0], tangents[i][1], tangents[i][2]);
                        GfVec3f b = bitangents[i];

                        float tLen = t.GetLength();
                        if (tLen > 1e-6f) {
                            t = t / tLen;
                            t = t - n * GfDot(n, t);
                            float orthLen = t.GetLength();
                            if (orthLen > 1e-6f) {
                                t = t / orthLen;
                            }
                            else {
                                if (std::abs(n[0]) < 0.9f) {
                                    t = GfVec3f(1.0f, 0.0f, 0.0f);
                                }
                                else {
                                    t = GfVec3f(0.0f, 1.0f, 0.0f);
                                }
                                t = t - n * GfDot(n, t);
                                t.Normalize();
                            }

                            GfVec3f calculatedBitangent = GfCross(n, t);
                            float handedness =
                                GfDot(calculatedBitangent, b) >= 0.0f ? 1.0f
                                                                      : -1.0f;
                            tangents[i] = GfVec4f(t[0], t[1], t[2], handedness);
                        }
                        else {
                            if (std::abs(n[0]) < 0.9f) {
                                t = GfVec3f(1.0f, 0.0f, 0.0f);
                            }
                            else {
                                t = GfVec3f(0.0f, 1.0f, 0.0f);
                            }
                            t = t - n * GfDot(n, t);
                            t.Normalize();
                            tangents[i] = GfVec4f(t[0], t[1], t[2], 1.0f);
                        }
                    }
                }
            }
        }

        _UpdateComputedPrimvarSources(sceneDelegate, *dirtyBits);
        if (!points.empty()) {
            if (requires_rebuild_blas) {
                create_gpu_resources(
                    static_cast<Hd_RUZINO_RenderParam*>(renderParam));
            }

            if (requires_rebuild_tlas) {
                if (IsVisible()) {
                    updateTLAS(
                        static_cast<Hd_RUZINO_RenderParam*>(renderParam),
                        sceneDelegate,
                        dirtyBits);
                }
            }
        }

        *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
    }
    static_cast<Hd_RUZINO_RenderParam*>(renderParam)
        ->InstanceCollection->mark_geometry_dirty();
}

void Hd_RUZINO_Mesh::Finalize(HdRenderParam* renderParam)
{
    // Mark the geom flag as dirty
    auto render_param = static_cast<Hd_RUZINO_RenderParam*>(renderParam);
    render_param->InstanceCollection->mark_geometry_dirty();

    // Deregister from the emissive registry so stale entries don't linger.
    if (instanceBuffer) {
        render_param->InstanceCollection->emissive_registry.deregister_mesh(
            instanceBuffer->index());
    }

    vertexBuffer = nullptr;
    BLAS = nullptr;
    instanceBuffer = nullptr;
    rt_instanceBuffer = nullptr;
    mesh_desc_buffer = nullptr;
    draw_indirect = nullptr;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
