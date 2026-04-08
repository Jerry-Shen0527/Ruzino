//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef HD_RUZINO_MESH_TOPOLOGY_H
#define HD_RUZINO_MESH_TOPOLOGY_H

#include <memory>

#include "api.h"
#include "pxr/imaging/hd/bufferSource.h"
#include "pxr/imaging/hd/meshTopology.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/types.h"
#include "pxr/imaging/hd/version.h"
#include "pxr/pxr.h"

RUZINO_NAMESPACE_OPEN_SCOPE
class HdSt_Subdivision;

using HdBufferArrayRangeSharedPtr = std::shared_ptr<class HdBufferArrayRange>;

using HdStComputationSharedPtr = std::shared_ptr<class HdStComputation>;

using HdSt_AdjacencyBuilderComputationPtr =
    std::weak_ptr<class HdSt_AdjacencyBuilderComputation>;

using HdSt_QuadInfoBuilderComputationPtr =
    std::weak_ptr<class HdSt_QuadInfoBuilderComputation>;
using HdSt_QuadInfoBuilderComputationSharedPtr =
    std::shared_ptr<class HdSt_QuadInfoBuilderComputation>;

using HdSt_MeshTopologySharedPtr = std::shared_ptr<class HdSt_MeshTopology>;

/// \class HdSt_MeshTopology
///
/// Storm implementation for mesh topology.
///
class HdSt_MeshTopology final : public pxr::HdMeshTopology {
   public:
    /// Specifies how subdivision mesh topology is refined.
    enum RefineMode { RefineModeUniform = 0, RefineModePatches };

    /// Specifies whether quads are triangulated or untriangulated.
    enum QuadsMode { QuadsTriangulated = 0, QuadsUntriangulated };

    /// Specifies type of interpolation to use in refinement
    enum Interpolation {
        INTERPOLATE_VERTEX,
        INTERPOLATE_VARYING,
        INTERPOLATE_FACEVARYING,
    };

    HD_RUZINO_API
    static HdSt_MeshTopologySharedPtr New(
        const HdMeshTopology& src,
        int refineLevel,
        RefineMode refineMode = RefineModeUniform,
        QuadsMode quadsMode = QuadsUntriangulated);

    HD_RUZINO_API
    virtual ~HdSt_MeshTopology();

    /// Equality check between two mesh topologies.
    HD_RUZINO_API
    bool operator==(HdSt_MeshTopology const& other) const;

    /// \name Triangulation
    /// @{

    /// Returns the triangle indices (for drawing) buffer source computation.
    HD_RUZINO_API
    pxr::HdBufferSourceSharedPtr GetTriangleIndexBuilderComputation(
        pxr::SdfPath const& id);

    /// Returns the CPU face-varying triangulate computation
    HD_RUZINO_API
    pxr::HdBufferSourceSharedPtr GetTriangulateFaceVaryingComputation(
        pxr::HdBufferSourceSharedPtr const& source,
        pxr::SdfPath const& id);

    /// @}

    ///
    /// \name Quadrangulation
    /// @{

    /// Returns the quads mode (triangulated or untriangulated).
    QuadsMode GetQuadsMode() const
    {
        return _quadsMode;
    }

    /// Helper function returns whether quads are triangulated.
    bool TriangulateQuads() const
    {
        return _quadsMode == QuadsTriangulated;
    }

    /// Returns the quadinfo computation for the use of primvar
    /// quadrangulation.
    /// If gpu is true, the quadrangulate table will be transferred to GPU
    /// via the resource registry.
    HD_RUZINO_API
    HdSt_QuadInfoBuilderComputationSharedPtr GetQuadInfoBuilderComputation(
        pxr::SdfPath const& id);

    /// Returns the quad indices (for drawing) buffer source computation.
    HD_RUZINO_API
    pxr::HdBufferSourceSharedPtr GetQuadIndexBuilderComputation(
        pxr::SdfPath const& id);

    /// Returns the CPU quadrangulated buffer source.
    HD_RUZINO_API
    pxr::HdBufferSourceSharedPtr GetQuadrangulateComputation(
        pxr::HdBufferSourceSharedPtr const& source,
        pxr::SdfPath const& id);

    /// Returns the CPU face-varying quadrangulate computation
    HD_RUZINO_API
    pxr::HdBufferSourceSharedPtr GetQuadrangulateFaceVaryingComputation(
        pxr::HdBufferSourceSharedPtr const& source,
        pxr::SdfPath const& id);

    /// Sets the quadrangulation struct. HdMeshTopology takes an
    /// ownership of quadInfo (caller shouldn't free)
    HD_RUZINO_API
    void SetQuadInfo(pxr::HdQuadInfo const* quadInfo);

    /// Returns the quadrangulation struct.
    pxr::HdQuadInfo const* GetQuadInfo() const
    {
        return _quadInfo.get();
    }

    /// @}

    ///
    /// \name Points
    /// @{

    /// Returns the point indices buffer source computation.
    HD_RUZINO_API
    pxr::HdBufferSourceSharedPtr GetPointsIndexBuilderComputation();

    /// @}

    ///
    /// \name Subdivision
    /// @{

    /// Returns the subdivision struct.
    HdSt_Subdivision const* GetSubdivision() const
    {
        return _subdivision.get();
    }

    /// Returns the subdivision struct (non-const).
    HdSt_Subdivision* GetSubdivision()
    {
        return _subdivision.get();
    }

    /// Returns true if the subdivision on this mesh produces
    /// triangles (otherwise quads)
    HD_RUZINO_API
    bool RefinesToTriangles() const;

    /// Returns true if the subdivision of this mesh produces bspline patches
    HD_RUZINO_API
    bool RefinesToBSplinePatches() const;

    /// Returns true if the subdivision of this mesh produces box spline
    /// triangle patches
    HD_RUZINO_API
    bool RefinesToBoxSplineTrianglePatches() const;

    /// Returns the subdivision topology computation. It computes
    /// far mesh and produces refined quad-indices buffer.
    HD_RUZINO_API
    pxr::HdBufferSourceSharedPtr GetOsdTopologyComputation(
        pxr::SdfPath const& debugId);

    /// Returns the refined indices builder computation.
    /// This just returns index and primitive buffer, and should be preceded by
    /// topology computation.
    HD_RUZINO_API
    pxr::HdBufferSourceSharedPtr GetOsdIndexBuilderComputation();

    /// Returns the refined face-varying indices builder computation.
    /// This just returns the index and patch param buffer for a face-varying
    /// channel, and should be preceded by topology computation.
    HD_RUZINO_API
    pxr::HdBufferSourceSharedPtr GetOsdFvarIndexBuilderComputation(int channel);

    /// Returns the subdivision primvar refine computation on CPU.
    HD_RUZINO_API
    pxr::HdBufferSourceSharedPtr GetOsdRefineComputation(
        pxr::HdBufferSourceSharedPtr const& source,
        Interpolation interpolation,
        int fvarChannel = 0);

    /// @}

    ///
    /// \name Geom Subsets
    /// @{

    /// Processes geom subsets to remove those with empty indices or empty
    /// material id. Will initialize _nonSubsetFaces if there are geom subsets.
    HD_RUZINO_API
    void SanitizeGeomSubsets();

    /// Returns the indices subset computation for unrefined indices.
    HD_RUZINO_API
    pxr::HdBufferSourceSharedPtr GetIndexSubsetComputation(
        pxr::HdBufferSourceSharedPtr indexBuilderSource,
        pxr::HdBufferSourceSharedPtr faceIndicesSource);

    /// Returns the indices subset computation for refined indices.
    HD_RUZINO_API
    pxr::HdBufferSourceSharedPtr GetRefinedIndexSubsetComputation(
        pxr::HdBufferSourceSharedPtr indexBuilderSource,
        pxr::HdBufferSourceSharedPtr faceIndicesSource);

    /// Returns the triangulated/quadrangulated face indices computation.
    HD_RUZINO_API
    pxr::HdBufferSourceSharedPtr GetGeomSubsetFaceIndexBuilderComputation(
        pxr::HdBufferSourceSharedPtr geomSubsetFaceIndexHelperSource,
        pxr::VtIntArray const& faceIndices);

    /// Returns computation creating buffer sources used in mapping authored
    /// face indices to triangulated/quadrangulated face indices.
    HD_RUZINO_API
    pxr::HdBufferSourceSharedPtr GetGeomSubsetFaceIndexHelperComputation(
        bool refined,
        bool quadrangulated);

    /// @}

    ///
    /// \name Face-varying Topologies
    /// @{
    /// Returns the face indices of faces not used in any geom subsets.
    std::vector<int> const* GetNonSubsetFaces() const
    {
        return _nonSubsetFaces.get();
    }

    /// Sets the face-varying topologies.
    void SetFvarTopologies(std::vector<pxr::VtIntArray> const& fvarTopologies)
    {
        _fvarTopologies = fvarTopologies;
    }

    /// Returns the face-varying topologies.
    std::vector<pxr::VtIntArray> const& GetFvarTopologies()
    {
        return _fvarTopologies;
    }

    /// @}

   private:
    QuadsMode _quadsMode;

    // quadrangulation info on CPU
    std::unique_ptr<pxr::HdQuadInfo const> _quadInfo;

    HdSt_QuadInfoBuilderComputationPtr _quadInfoBuilder;

    // OpenSubdiv
    RefineMode _refineMode;
    std::unique_ptr<HdSt_Subdivision> _subdivision;
    pxr::HdBufferSourceWeakPtr _osdTopologyBuilder;
    pxr::HdBufferSourceWeakPtr _osdBaseFaceToRefinedFacesMap;

    std::vector<pxr::VtIntArray> _fvarTopologies;

    // When using geom subsets, the indices of faces that are not contained
    // within the geom subsets. Populated by SanitizeGeomSubsets().
    std::unique_ptr<std::vector<int>> _nonSubsetFaces;

    // Must be created through factory
    explicit HdSt_MeshTopology(
        const HdMeshTopology& src,
        int refineLevel,
        RefineMode refineMode,
        QuadsMode quadsMode);

    // No default construction or copying.
    HdSt_MeshTopology() = delete;
    HdSt_MeshTopology(const HdSt_MeshTopology&) = delete;
    HdSt_MeshTopology& operator=(const HdSt_MeshTopology&) = delete;
};

RUZINO_NAMESPACE_CLOSE_SCOPE

#endif  // HD_RUZINO_MESH_TOPOLOGY_H
