//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef HD_RUZINO_SUBDIVISION_H
#define HD_RUZINO_SUBDIVISION_H

#include <opensubdiv/far/patchTable.h>
#include <opensubdiv/far/stencilTable.h>

#include <memory>
#include <mutex>

#include "computation.h"
#include "meshTopology.h"
#include "pxr/base/tf/token.h"
#include "pxr/imaging/hd/bufferSource.h"
#include "pxr/pxr.h"
#include "pxr/usd/sdf/path.h"

RUZINO_NAMESPACE_OPEN_SCOPE

/// \class Hd_Subdivision
///
/// Subdivision struct holding subdivision tables and patch tables.
///
/// This single struct can be used for cpu and gpu subdivision at the same time.
///
class HdSt_Subdivision final {
   public:
    using StencilTable = OpenSubdiv::Far::StencilTable;
    using PatchTable = OpenSubdiv::Far::PatchTable;

    HdSt_Subdivision(bool adaptive, int refineLevel);
    ~HdSt_Subdivision();

    bool IsAdaptive() const
    {
        return _adaptive;
    }

    int GetRefineLevel() const
    {
        return _refineLevel;
    }

    int GetNumVertices() const;
    int GetNumVarying() const;
    int GetNumFaceVarying(int channel) const;
    int GetMaxNumFaceVarying() const;

    pxr::VtIntArray GetRefinedFvarIndices(int channel) const;

    void RefineCPU(
        pxr::HdBufferSourceSharedPtr const& source,
        std::vector<float>* primvarBuffer,
        HdSt_MeshTopology::Interpolation interpolation,
        int fvarChannel = 0);

    // computation factory methods
    pxr::HdBufferSourceSharedPtr CreateTopologyComputation(
        HdSt_MeshTopology* topology,
        pxr::SdfPath const& id);

    pxr::HdBufferSourceSharedPtr CreateIndexComputation(
        HdSt_MeshTopology* topology,
        pxr::HdBufferSourceSharedPtr const& osdTopology);

    pxr::HdBufferSourceSharedPtr CreateFvarIndexComputation(
        HdSt_MeshTopology* topology,
        pxr::HdBufferSourceSharedPtr const& osdTopology,
        int channel);

    pxr::HdBufferSourceSharedPtr CreateRefineComputationCPU(
        HdSt_MeshTopology* topology,
        pxr::HdBufferSourceSharedPtr const& source,
        pxr::HdBufferSourceSharedPtr const& osdTopology,
        HdSt_MeshTopology::Interpolation interpolation,
        int fvarChannel = 0);

    pxr::HdBufferSourceSharedPtr CreateBaseFaceToRefinedFacesMapComputation(
        pxr::HdBufferSourceSharedPtr const& osdTopology);

    /// Returns true if the subdivision for \a scheme generates triangles,
    /// instead of quads.
    static bool RefinesToTriangles(pxr::TfToken const& scheme);

    /// Returns true if the subdivision for \a scheme generates bspline patches.
    static bool RefinesToBSplinePatches(pxr::TfToken const& scheme);

    /// Returns true if the subdivision for \a scheme generates box spline
    /// triangle patches.
    static bool RefinesToBoxSplineTrianglePatches(pxr::TfToken const& scheme);

    /// Takes ownership of stencil tables and patch table
    void SetRefinementTables(
        std::unique_ptr<StencilTable const>&& vertexStencils,
        std::unique_ptr<StencilTable const>&& varyingStencils,
        std::vector<std::unique_ptr<StencilTable const>>&& faceVaryingStencils,
        std::unique_ptr<PatchTable const>&& patchTable);

    StencilTable const* GetStencilTable(
        HdSt_MeshTopology::Interpolation interpolation,
        int fvarChannel) const;

    PatchTable const* GetPatchTable() const
    {
        return _patchTable.get();
    }

   private:
    std::unique_ptr<StencilTable const> _vertexStencils;
    std::unique_ptr<StencilTable const> _varyingStencils;
    std::vector<std::unique_ptr<StencilTable const>> _faceVaryingStencils;
    std::unique_ptr<PatchTable const> _patchTable;

    bool const _adaptive;
    int const _refineLevel;
    int _maxNumFaceVarying;  // calculated during SetRefinementTables()

    std::mutex _gpuStencilMutex;
};

// ---------------------------------------------------------------------------
/// \class Hd_OsdRefineComputation
///
/// OpenSubdiv CPU Refinement.
/// This class isn't inherited from HdComputedBufferSource.
/// GetData() returns the internal buffer to skip unecessary copy.
///
class HdSt_OsdRefineComputationCPU final : public pxr::HdBufferSource {
   public:
    HdSt_OsdRefineComputationCPU(
        HdSt_MeshTopology* topology,
        pxr::HdBufferSourceSharedPtr const& source,
        pxr::HdBufferSourceSharedPtr const& osdTopology,
        HdSt_MeshTopology::Interpolation interpolation,
        int fvarChannel = 0);
    ~HdSt_OsdRefineComputationCPU() override;

    pxr::TfToken const& GetName() const override;
    size_t ComputeHash() const override;
    void const* GetData() const override;
    pxr::HdTupleType GetTupleType() const override;
    size_t GetNumElements() const override;
    void GetBufferSpecs(pxr::HdBufferSpecVector* specs) const override;
    bool Resolve() override;
    bool HasPreChainedBuffer() const override;
    pxr::HdBufferSourceSharedPtr GetPreChainedBuffer() const override;
    HdSt_MeshTopology::Interpolation GetInterpolation() const;

   protected:
    bool _CheckValid() const override;

   private:
    HdSt_MeshTopology* _topology;
    pxr::HdBufferSourceSharedPtr _source;
    pxr::HdBufferSourceSharedPtr _osdTopology;
    std::vector<float> _primvarBuffer;
    HdSt_MeshTopology::Interpolation _interpolation;
    int _fvarChannel;
};

RUZINO_NAMESPACE_CLOSE_SCOPE

#endif  // HD_RUZINO_SUBDIVISION_H
