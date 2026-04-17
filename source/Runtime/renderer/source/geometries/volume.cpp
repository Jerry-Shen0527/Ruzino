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

#include "volume.h"

#include <nanovdb/util/IO.h>
#include <nanovdb/util/OpenToNanoVDB.h>
#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>

#include "../renderParam.h"
#include "RHI/rhi.hpp"

// OpenVDB and NanoVDB - placeholder includes (adjust to your actual setup)

RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;

Hd_RUZINO_Volume::Hd_RUZINO_Volume(const SdfPath& id)
    : HdVolume(id),
      _fieldsLoaded(false),
      _boundingBoxValid(false)
{
    spdlog::info("Creating volume: %s", id.GetText());
}

Hd_RUZINO_Volume::~Hd_RUZINO_Volume()
{
}

HdDirtyBits Hd_RUZINO_Volume::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
           HdChangeTracker::DirtyPrimvar | HdChangeTracker::DirtyMaterialId |
           HdChangeTracker::AllDirty;
}

void Hd_RUZINO_Volume::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits,
    const TfToken& reprToken)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    const SdfPath& id = GetId();

    spdlog::info("Syncing volume: %s", id.GetText());

    if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id)) {
        _sharedData.visible = sceneDelegate->GetVisible(id);
    }

    if (HdChangeTracker::IsTransformDirty(*dirtyBits, id)) {
        _transform = GfMatrix4f(sceneDelegate->GetTransform(id));
    }
    // Debug: Output which dirty bits are set
    if (*dirtyBits != 0) {
        std::string dirtyBitNames;
        if (*dirtyBits & HdChangeTracker::DirtyVisibility)
            dirtyBitNames += "DirtyVisibility ";
        if (*dirtyBits & HdChangeTracker::DirtyTransform)
            dirtyBitNames += "DirtyTransform ";
        if (*dirtyBits & HdChangeTracker::DirtyPrimvar)
            dirtyBitNames += "DirtyPrimvar ";
        if (*dirtyBits & HdChangeTracker::DirtyMaterialId)
            dirtyBitNames += "DirtyMaterialId ";
        if (*dirtyBits & HdChangeTracker::DirtyVolumeField)
            dirtyBitNames += "DirtyVolumeField ";
        if (*dirtyBits & HdChangeTracker::DirtyTopology)
            dirtyBitNames += "DirtyTopology ";
        if (*dirtyBits & HdChangeTracker::DirtyPoints)
            dirtyBitNames += "DirtyPoints ";
        if (*dirtyBits & HdChangeTracker::DirtyNormals)
            dirtyBitNames += "DirtyNormals ";
        if (*dirtyBits & HdChangeTracker::DirtyWidths)
            dirtyBitNames += "DirtyWidths ";
        if (*dirtyBits & HdChangeTracker::DirtyInstancer)
            dirtyBitNames += "DirtyInstancer ";
        if (*dirtyBits & HdChangeTracker::DirtyInstanceIndex)
            dirtyBitNames += "DirtyInstanceIndex ";
        if (*dirtyBits & HdChangeTracker::DirtyRepr)
            dirtyBitNames += "DirtyRepr ";
        if (*dirtyBits & HdChangeTracker::DirtyRenderTag)
            dirtyBitNames += "DirtyRenderTag ";
        if (*dirtyBits & HdChangeTracker::DirtyCategories)
            dirtyBitNames += "DirtyCategories ";
        if (*dirtyBits & HdChangeTracker::AllDirty)
            dirtyBitNames += "AllDirty ";

        spdlog::info(
            "Volume %s dirty bits: 0x%x [%s]",
            id.GetText(),
            static_cast<unsigned int>(*dirtyBits),
            dirtyBitNames.c_str());
    }

    // Only load volume fields if they haven't been loaded yet or if primvars
    // are dirty

    _LoadVolumeFields(sceneDelegate);
    _fieldsLoaded = true;

    // Create GPU resources after loading
    if (auto* ruzinoRenderParam =
            static_cast<Hd_RUZINO_RenderParam*>(renderParam)) {
        CreateGPUResources(ruzinoRenderParam);
    }

    // Clear only the dirty bits that we actually handled
    *dirtyBits = HdChangeTracker::Clean;
}

void Hd_RUZINO_Volume::_LoadVolumeFields(HdSceneDelegate* sceneDelegate)
{
    const SdfPath& id = GetId();

    // Get volume field descriptors from USD
    auto fieldDescriptors = sceneDelegate->GetVolumeFieldDescriptors(id);

    spdlog::info(
        "Loading %zu volume fields for %s",
        fieldDescriptors.size(),
        id.GetText());

    _fields.clear();
    for (const auto& fieldDesc : fieldDescriptors) {
        VolumeFieldData fieldData;
        fieldData.name = fieldDesc.fieldName.GetString();
        fieldData.fieldPrimType = fieldDesc.fieldPrimType;

        spdlog::info(
            "Processing field: %s (type: %s)",
            fieldData.name.c_str(),
            fieldData.fieldPrimType.GetText());

        // Get the field prim path and query its properties
        if (!fieldDesc.fieldId.IsEmpty()) {
            // Get file path - this is typically stored as "filePath" primvar
            VtValue filePathValue =
                sceneDelegate->Get(fieldDesc.fieldId, TfToken("filePath"));
            if (!filePathValue.IsEmpty() &&
                filePathValue.IsHolding<SdfAssetPath>()) {
                SdfAssetPath assetPath = filePathValue.Get<SdfAssetPath>();
                fieldData.filePath = assetPath.GetResolvedPath();
                if (fieldData.filePath.empty()) {
                    fieldData.filePath = assetPath.GetAssetPath();
                }

                spdlog::info("Field file path: %s", fieldData.filePath.c_str());

                // For OpenVDB, get the grid name
                VtValue gridNameValue =
                    sceneDelegate->Get(fieldDesc.fieldId, TfToken("fieldName"));
                if (!gridNameValue.IsEmpty() &&
                    gridNameValue.IsHolding<TfToken>()) {
                    fieldData.gridName =
                        gridNameValue.Get<TfToken>().GetString();
                }
                else if (
                    !gridNameValue.IsEmpty() &&
                    gridNameValue.IsHolding<std::string>()) {
                    fieldData.gridName = gridNameValue.Get<std::string>();
                }
                if (fieldData.gridName.empty()) {
                    // Default grid names for common field purposes
                    if (fieldData.fieldPrimType == TfToken("OpenVDBAsset") ||
                        fieldData.name.find("density") != std::string::npos) {
                        // Try common density grid names
                        fieldData.gridName = "density";
                    }
                    else if (fieldData.name.find("sdf") != std::string::npos) {
                        fieldData.gridName = "sdf";
                    }
                    else if (
                        fieldData.name.find("temperature") !=
                        std::string::npos) {
                        fieldData.gridName = "temperature";
                    }
                    else if (
                        fieldData.name.find("velocity") != std::string::npos) {
                        fieldData.gridName = "velocity";
                    }
                    else {
                        // Try density first, then sdf as fallback
                        fieldData.gridName = "density";
                    }
                }

                // Load the volume data
                if (!fieldData.filePath.empty() &&
                    std::filesystem::exists(fieldData.filePath)) {
                    VolumeFormat format =
                        _DetectVolumeFormat(fieldData.filePath);
                    bool loaded = false;

                    switch (format) {
                        case VolumeFormat::OpenVDB:
                            loaded = _LoadOpenVDB(
                                fieldData.filePath,
                                fieldData.gridName,
                                fieldData);
                            break;
                        case VolumeFormat::Field3D:
                            loaded = _LoadField3D(
                                fieldData.filePath,
                                fieldData.gridName,
                                fieldData);
                            break;
                        default:
                            spdlog::error(
                                "Unsupported volume format for file: %s",
                                fieldData.filePath.c_str());
                            break;
                    }

                    if (loaded) {
                        spdlog::info(
                            "Successfully loaded field %s: %dx%dx%d voxels",
                            fieldData.name.c_str(),
                            fieldData.dimensions[0],
                            fieldData.dimensions[1],
                            fieldData.dimensions[2]);
                    }
                }
            }
        }

        _fields[fieldDesc.fieldName] = std::move(fieldData);
    }

    _UpdateBoundingBox();
    _PrepareGPUData();
}

VolumeFormat Hd_RUZINO_Volume::_DetectVolumeFormat(const std::string& filePath)
{
    std::filesystem::path path(filePath);
    std::string extension = path.extension().string();

    // Convert to lowercase for comparison
    std::transform(
        extension.begin(), extension.end(), extension.begin(), ::tolower);

    if (extension == ".vdb") {
        return VolumeFormat::OpenVDB;
    }
    else if (extension == ".f3d") {
        return VolumeFormat::Field3D;
    }
    else if (extension == ".raw" || extension == ".vol") {
        return VolumeFormat::Raw;
    }

    return VolumeFormat::Unknown;
}

bool Hd_RUZINO_Volume::_LoadOpenVDB(
    const std::string& filePath,
    const std::string& gridName,
    VolumeFieldData& fieldData)
{
    spdlog::info(
        "Loading OpenVDB file: %s, grid: %s",
        filePath.c_str(),
        gridName.c_str());
    try {
        openvdb::initialize();
        openvdb::io::File file(filePath);
        file.open();

        // First, let's see what grids are available in the file
        spdlog::info("Examining OpenVDB file: %s", filePath.c_str());

        openvdb::GridBase::Ptr baseGrid;

        // Try to read the specific grid first
        try {
            baseGrid = file.readGrid(gridName);
            if (baseGrid) {
                spdlog::info(
                    "Successfully found grid '%s' in file", gridName.c_str());
            }
        }
        catch (const std::exception& e) {
            spdlog::info(
                "Failed to read grid '%s': %s", gridName.c_str(), e.what());
        }
        // If we couldn't find the specific grid, try to get all grids
        if (!baseGrid) {
            spdlog::info(
                "Grid '%s' not found, looking for available grids",
                gridName.c_str());

            try {
                auto gridPtrs = file.getGrids();
                if (gridPtrs && !gridPtrs->empty()) {
                    spdlog::info("Found %zu grids in file:", gridPtrs->size());

                    // List all available grids
                    for (size_t i = 0; i < gridPtrs->size(); ++i) {
                        auto grid = (*gridPtrs)[i];
                        if (grid) {
                            std::string name = grid->getName();
                            spdlog::info(
                                "  Grid %zu: name='%s', type=%s",
                                i,
                                name.c_str(),
                                grid->type().c_str());
                        }
                    }

                    // Use the first grid
                    baseGrid = (*gridPtrs)[0];
                    spdlog::info(
                        "Using first grid: %s", baseGrid->getName().c_str());
                }
                else {
                    spdlog::error(
                        "No grids found in file '%s'", filePath.c_str());
                }
            }
            catch (const std::exception& e) {
                spdlog::error(
                    "Failed to enumerate grids in file '%s': %s",
                    filePath.c_str(),
                    e.what());
            }

            if (!baseGrid) {
                spdlog::error(
                    "No valid grids found in file '%s'", filePath.c_str());
                return false;
            }
        }  // Cast to float grid (most common case)
        openvdb::FloatGrid::Ptr grid =
            openvdb::gridPtrCast<openvdb::FloatGrid>(baseGrid);
        if (!grid) {
            spdlog::error(
                "Failed to cast grid to FloatGrid. Grid type: %s",
                baseGrid ? baseGrid->type().c_str() : "null");
            return false;
        }

        auto bbox = grid->evalActiveVoxelBoundingBox();
        fieldData.dimensions =
            GfVec3i(bbox.dim().x(), bbox.dim().y(), bbox.dim().z());
        fieldData.voxelSize = GfVec3f(
            grid->voxelSize().x(),
            grid->voxelSize().y(),
            grid->voxelSize().z());

        // Convert bounding box to world space
        auto worldBBox = grid->transform().indexToWorld(bbox);
        GfVec3d minPt(
            worldBBox.min().x(), worldBBox.min().y(), worldBBox.min().z());
        GfVec3d maxPt(
            worldBBox.max().x(), worldBBox.max().y(), worldBBox.max().z());
        fieldData.boundingBox =
            GfBBox3d(GfRange3d(minPt, maxPt));  // Convert OpenVDB to NanoVDB
        auto handle = nanovdb::tools::openToNanoVDB(baseGrid);
        if (!handle) {
            spdlog::error("Failed to convert OpenVDB grid to NanoVDB");
            return false;
        }

        // Serialize NanoVDB to binary data
        fieldData.nanoVDBData.resize(handle.size());
        std::memcpy(fieldData.nanoVDBData.data(), handle.data(), handle.size());
        fieldData.dataSize = handle.size();

        file.close();

        fieldData.isLoaded = true;

        spdlog::info(
            "OpenVDB placeholder loaded (replace with actual implementation): "
            "%zu bytes",
            fieldData.dataSize);

        return true;
    }
    catch (const std::exception& e) {
        spdlog::error(
            "Error loading OpenVDB file '%s': %s", filePath.c_str(), e.what());
        return false;
    }
}

bool Hd_RUZINO_Volume::_LoadField3D(
    const std::string& filePath,
    const std::string& fieldName,
    VolumeFieldData& fieldData)
{
    spdlog::info(
        "Loading Field3D file: %s, field: %s",
        filePath.c_str(),
        fieldName.c_str());

    // TODO: Implement Field3D loading
    // This would require Field3D library integration

    // Placeholder implementation
    fieldData.dimensions = GfVec3i(32, 32, 32);
    fieldData.voxelSize = GfVec3f(0.2f, 0.2f, 0.2f);
    fieldData.boundingBox = GfBBox3d(GfRange3d(GfVec3d(-3.2), GfVec3d(3.2)));
    fieldData.isLoaded = true;
    fieldData.dataSize = 32 * 32 * 32 * sizeof(float);

    spdlog::info(
        "Field3D placeholder loaded (implement actual Field3D support)");
    return true;
}

bool Hd_RUZINO_Volume::_LoadRawVolume(
    const std::string& filePath,
    const GfVec3i& dimensions,
    VolumeFieldData& fieldData)
{
    spdlog::info(
        "Loading raw volume: %s (%dx%dx%d)",
        filePath.c_str(),
        dimensions[0],
        dimensions[1],
        dimensions[2]);

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        spdlog::error("Failed to open raw volume file: %s", filePath.c_str());
        return false;
    }

    fieldData.dimensions = dimensions;
    fieldData.dataSize =
        dimensions[0] * dimensions[1] * dimensions[2] * sizeof(float);

    // TODO: Load actual data into a buffer for later GPU upload
    // std::vector<float> volumeData(fieldData.dataSize / sizeof(float));
    // file.read(reinterpret_cast<char*>(volumeData.data()),
    // fieldData.dataSize);

    fieldData.voxelSize = GfVec3f(
        1.0f / dimensions[0], 1.0f / dimensions[1], 1.0f / dimensions[2]);
    fieldData.boundingBox = GfBBox3d(GfRange3d(
        GfVec3d(0), GfVec3d(dimensions[0], dimensions[1], dimensions[2])));
    fieldData.isLoaded = true;

    file.close();
    return true;
}

void Hd_RUZINO_Volume::_UpdateBoundingBox()
{
    if (_fields.empty()) {
        _boundingBox = GfBBox3d();
        _boundingBoxValid = false;
        return;
    }

    // Combine bounding boxes from all fields
    bool first = true;
    for (const auto& fieldPair : _fields) {
        const VolumeFieldData& field = fieldPair.second;
        if (field.isLoaded) {
            if (first) {
                _boundingBox = field.boundingBox;
                first = false;
            }
            else {
                GfRange3d combined = _boundingBox.GetRange();
                combined.UnionWith(field.boundingBox.GetRange());
                _boundingBox = GfBBox3d(combined);
            }
        }
    }

    _boundingBoxValid = true;

    spdlog::info(
        "Volume bounding box: min(%.2f, %.2f, %.2f) max(%.2f, %.2f, %.2f)",
        _boundingBox.GetRange().GetMin()[0],
        _boundingBox.GetRange().GetMin()[1],
        _boundingBox.GetRange().GetMin()[2],
        _boundingBox.GetRange().GetMax()[0],
        _boundingBox.GetRange().GetMax()[1],
        _boundingBox.GetRange().GetMax()[2]);
}

void Hd_RUZINO_Volume::_PrepareGPUData()
{
    _gpuData.transform = _transform;
    _gpuData.boundingBox = _boundingBox;
    _gpuData.fieldCount = static_cast<uint32_t>(_fields.size());

    // Find indices for common field types
    _gpuData.densityFieldIndex = UINT32_MAX;
    _gpuData.temperatureFieldIndex = UINT32_MAX;
    _gpuData.velocityFieldIndex = UINT32_MAX;
    uint32_t fieldIndex = 0;
    std::string primaryFieldType = "unknown";
    for (const auto& fieldPair : _fields) {
        const VolumeFieldData& field = fieldPair.second;

        // Classify fields based on name and type
        if (field.name.find("density") != std::string::npos ||
            field.fieldPrimType == TfToken("OpenVDBAsset")) {
            _gpuData.densityFieldIndex = fieldIndex;
            _gpuData.dimensions = field.dimensions;
            _gpuData.voxelSize = field.voxelSize;
            primaryFieldType = "density";
            spdlog::info("Found density field at index %d", fieldIndex);
        }
        else if (field.name.find("sdf") != std::string::npos) {
            // SDF can also be used as a density-like field for rendering
            if (_gpuData.densityFieldIndex == UINT32_MAX) {
                _gpuData.densityFieldIndex = fieldIndex;
                _gpuData.dimensions = field.dimensions;
                _gpuData.voxelSize = field.voxelSize;
                primaryFieldType = "sdf";
                spdlog::info(
                    "Found SDF field at index %d, using as density field",
                    fieldIndex);
            }
        }
        else if (field.name.find("temperature") != std::string::npos) {
            _gpuData.temperatureFieldIndex = fieldIndex;
            spdlog::info("Found temperature field at index %d", fieldIndex);
        }
        else if (field.name.find("velocity") != std::string::npos) {
            _gpuData.velocityFieldIndex = fieldIndex;
            spdlog::info("Found velocity field at index %d", fieldIndex);
        }

        fieldIndex++;
    }

    // If no density field was found, use the first available field
    if (_gpuData.densityFieldIndex == UINT32_MAX && !_fields.empty()) {
        const auto& firstField = _fields.begin()->second;
        _gpuData.densityFieldIndex = 0;
        _gpuData.dimensions = firstField.dimensions;
        _gpuData.voxelSize = firstField.voxelSize;
        primaryFieldType = "generic";
        spdlog::info(
            "No density or sdf field found, using first field as density");
    }

    spdlog::info(
        "GPU data prepared - %d fields, primary field type: %s at index %d",
        _gpuData.fieldCount,
        primaryFieldType.c_str(),
        _gpuData.densityFieldIndex);
}

void Hd_RUZINO_Volume::Finalize(HdRenderParam* renderParam)
{
    spdlog::info("Finalizing volume: %s", GetId().GetText());

    // Clean up GPU resources properly
    auto* ruzinoRenderParam = static_cast<Hd_RUZINO_RenderParam*>(renderParam);
    auto device = RHI::get_device();

    if (device) {
        // Wait for any pending GPU operations to complete
        device->waitForIdle();

        for (auto& fieldPair : _fields) {
            VolumeFieldData& field = fieldPair.second;

            // Release GPU buffer if it exists
            if (field.gpuBuffer) {
                spdlog::info(
                    "Releasing GPU buffer for field: %s", field.name.c_str());
                field.gpuBuffer = nullptr;
            }

            // Release 3D texture if it exists
            if (field.texture3D) {
                spdlog::info(
                    "Releasing 3D texture for field: %s", field.name.c_str());
                field.texture3D = nullptr;
            }

            // Clear CPU data
            field.nanoVDBData.clear();
            field.nanoVDBData.shrink_to_fit();
            field.isLoaded = false;
            field.dataSize = 0;
        }
    }
    else {
        spdlog::warn("Device not available during volume finalization");
        // Still clean up what we can
        for (auto& fieldPair : _fields) {
            VolumeFieldData& field = fieldPair.second;
            field.gpuBuffer = nullptr;
            field.texture3D = nullptr;
            field.nanoVDBData.clear();
            field.nanoVDBData.shrink_to_fit();
            field.isLoaded = false;
            field.dataSize = 0;
        }
    }

    // Clear all fields and reset state
    _fields.clear();
    _fieldsLoaded = false;
    _boundingBoxValid = false;
    _boundingBox = GfBBox3d();

    // Reset GPU data
    _gpuData = VolumeGPUData{};

    spdlog::info("Volume finalization complete: %s", GetId().GetText());
}

void Hd_RUZINO_Volume::_InitRepr(
    const TfToken& reprToken,
    HdDirtyBits* dirtyBits)
{
    // Volume representation initialization
}

HdDirtyBits Hd_RUZINO_Volume::_PropagateDirtyBits(HdDirtyBits bits) const
{
    return bits;
}

void Hd_RUZINO_Volume::CreateGPUResources(Hd_RUZINO_RenderParam* renderParam)
{
    auto device = RHI::get_device();

    spdlog::info("Creating GPU resources for volume");

    for (auto& fieldPair : _fields) {
        VolumeFieldData& field = fieldPair.second;

        if (!field.isLoaded || field.nanoVDBData.empty()) {
            continue;
        }

        // Create GPU buffer for NanoVDB data
        nvrhi::BufferDesc bufferDesc =
            nvrhi::BufferDesc{}
                .setByteSize(field.dataSize)
                .setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setCpuAccess(nvrhi::CpuAccessMode::None)
                .setCanHaveRawViews(true)
                .setKeepInitialState(true)
                .setDebugName("VolumeNanoVDBBuffer_" + field.name);

        field.gpuBuffer = device->createBuffer(bufferDesc);
        field.gpuBufferHandle.Reset();
        field.gpuBufferHandle =
            renderParam->InstanceCollection->bindlessData
                .bufferDescriptorTableManager->CreateDescriptorHandle(
                    nvrhi::BindingSetItem::RawBuffer_SRV(0, field.gpuBuffer));

        // Upload NanoVDB data to GPU
        auto commandList = device->createCommandList();
        commandList->open();
        commandList->writeBuffer(
            field.gpuBuffer, field.nanoVDBData.data(), field.dataSize, 0);
        commandList->close();
        device->executeCommandList(commandList);

        spdlog::info(
            "Created GPU buffer for field %s: %zu bytes",
            field.name.c_str(),
            field.dataSize);
    }

    spdlog::info(
        "GPU resources created for volume with %zu fields", _fields.size());
}

RUZINO_NAMESPACE_CLOSE_SCOPE
