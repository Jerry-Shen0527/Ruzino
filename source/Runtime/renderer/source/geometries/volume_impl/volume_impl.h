// Abstract base class for volume rendering strategies (the "pimpl impl").
//
// Hd_RUZINO_Volume is the single Hydra rprim for the `volume` token. At Sync
// time it reads the `volumeType` primvar and selects a concrete VolumeImpl
// (WetbrushVolumeImpl / CloudVolumeImpl / future FileVolumeImpl). The impl owns
// everything type-specific: the primvar parsing, the density/texture resource
// build, the VolumeDesc fields, the hit-group index. The Hd_RUZINO_Volume shell
// owns everything shared: the AABB/BLAS, the pool handles, the TLAS instance
// writing. This is the pimpl + strategy pattern (the private-inheritance idea,
// expressed as composition): one stable Hydra interface, multiple independent
// implementation files.
//
// Adding a new volume type = new subclass in its own .h/.cpp + a case in
// VolumeImpl::create(). No changes to Hd_RUZINO_Volume or the TLAS code.
#pragma once

// volume.h establishes the shared include environment this header needs:
// the slang SceneTypes (VolumeDesc/VolumeKind + the float3/float2 host aliases
// it pulls in), the pxr gf/hd types (GfVec3f, HdSceneDelegate, HdDirtyBits),
// nvrhi, and DeviceMemoryPool.hpp. Re-declaring those here independently led
// to a fragile ordering where GfVec3f failed to resolve when this header was
// the first include of a TU; leaning on volume.h's proven include sequence
// avoids that. (volume.h forward-declares VolumeImpl, so no cycle.)
#include <memory>
#include <string>

#include "../volume.h"

RUZINO_NAMESPACE_OPEN_SCOPE

class Hd_RUZINO_RenderParam;

/// Strategy interface for a volume rprim's type-specific behaviour.
///
/// The Hd_RUZINO_Volume shell calls these methods during Sync /
/// create_gpu_resources:
///   - kind()/hitGroupIndex()/debugName(): identity + routing (called every
///   frame)
///   - boundsMin/Max(): the world AABB (called from create_gpu_resources)
///   - parsePrimvars(): pull type-specific primvars; return true if GPU
///   resources
///     must be rebuilt this Sync
///   - buildDensityResource(): create/refresh the type-specific density/texture
///     resource (e.g. the wetbrush Float4 buffer). Return false if resources
///     are invalid and the volume should be skipped this frame (no TLAS entry).
///   - fillVolumeDesc(): fill the type-specific VolumeDesc fields (the shell
///   fills
///     boundsMin/boundsMax, the impl fills the rest)
///
/// Id (GetId) is passed by value of the SdfPath because the impl does not hold
/// a back-pointer to the rprim (avoids a reference cycle + keeps the impl
/// testable).
class VolumeImpl {
   public:
    virtual ~VolumeImpl() = default;

    /// Discriminator matching the shader-side VolumeKind enum.
    virtual VolumeKind kind() const = 0;
    /// TLAS hit-group slot for radiance/shadow rays (wetbrush=4, cloud=6).
    virtual uint32_t hitGroupIndex() const = 0;
    /// Debug name for the AABB buffer (e.g. "cloudVolumeAABB").
    virtual std::string debugName() const = 0;

    /// World-space AABB min/max (used for the procedural BLAS + shader slab
    /// test).
    virtual GfVec3f boundsMin() const = 0;
    virtual GfVec3f boundsMax() const = 0;

    /// Pull type-specific primvars from the scene delegate.
    /// \return true if GPU resources must be rebuilt this Sync.
    virtual bool parsePrimvars(
        HdSceneDelegate* sceneDelegate,
        const SdfPath& id,
        HdDirtyBits* dirtyBits) = 0;

    /// Build/refresh the type-specific density or texture resource.
    /// \return false if resources are invalid (skip TLAS registration this
    /// frame).
    virtual bool buildDensityResource(Hd_RUZINO_RenderParam* render_param) = 0;

    /// Fill the type-specific fields of the VolumeDesc. The shell has already
    /// filled boundsMin/boundsMax before calling this; the impl fills the rest
    /// (grid metadata for wetbrush, cloud shaping params for cloud, etc.).
    virtual void fillVolumeDesc(VolumeDesc& vd) const = 0;

    /// Factory: read the `volumeType` primvar and select/keep a concrete impl.
    /// `previous` is the owning unique_ptr (passed by reference so the factory
    /// can reuse-or-replace it in place): if the existing impl is already the
    /// right type, it is kept (cached primvars / registry versions survive); a
    /// type switch releases it and constructs a fresh one. Pass an empty ptr on
    /// first construction.
    static void resolve(
        HdSceneDelegate* sceneDelegate,
        const SdfPath& id,
        std::unique_ptr<VolumeImpl>& previous);
};

/// Build a single-AABB procedural BLAS, shared by all volume types.
///
/// Creates-or-reuses `aabbBuffer` (one nvrhi::rt::GeometryAABB), uploads the
/// given bounds, and builds the BLAS once (on first call, when BLAS is null).
/// On subsequent calls only the AABB buffer contents are refreshed; the accel
/// struct is not rebuilt (matches the existing wetbrush/cloud behaviour — the
/// TLAS only needs the bounds at build time, and the buffer contents stay
/// fresh).
///
/// This centralizes the ~45 lines that were duplicated between the cloud and
/// wetbrush branches of the old Hd_RUZINO_WetbrushVolume, and fixes an
/// inconsistency: the old BLAS-build submission was NOT under
/// execution_launch_mutex, unlike every other rprim (mesh/points/light). This
/// helper holds the lock across the upload + the BLAS build, matching the
/// points.cpp/mesh.cpp/light.cpp convention.
void buildSingleAabbBlas(
    nvrhi::IDevice* device,
    GfVec3f boundsMin,
    GfVec3f boundsMax,
    nvrhi::BufferHandle& aabbBuffer,
    nvrhi::rt::AccelStructHandle& BLAS,
    nvrhi::CommandListHandle& command_list,
    nvrhi::CommandListHandle& copy_commandlist,
    const std::string& debugName);

RUZINO_NAMESPACE_CLOSE_SCOPE
