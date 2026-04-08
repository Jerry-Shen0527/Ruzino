//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef HD_RUZINO_TRIANGULATE_H
#define HD_RUZINO_TRIANGULATE_H

#include "../api.h"
#include "pxr/imaging/hd/bufferSource.h"
#include "pxr/imaging/hd/version.h"
#include "pxr/pxr.h"
#include "pxr/usd/sdf/path.h"

RUZINO_NAMESPACE_OPEN_SCOPE

class HdSt_MeshTopology;

/// \class HdSt_TriangleIndexBuilderComputation
///
/// Triangle indices computation CPU.
///

// Index triangulation generates a mapping from triangle ID to authored
// face index domain, called primitiveParams. The primitive params are stored
// alongside topology index buffers, so that the same aggregation locators can
// be used for such an additional buffer as well. This change transforms index
// buffer from int array to int[3] array or int[4] array at first. Thanks to
// the heterogenius non-interleaved buffer aggregation ability in hd,
// we'll get this kind of buffer layout:
//
// ----+--------+--------+------
// ... |i0 i1 i2|i3 i4 i5| ...   index buffer (for triangles)
// ----+--------+--------+------
// ... |   m0   |   m1   | ...   primitive param buffer (coarse face index)
// ----+--------+--------+------

class HdSt_TriangleIndexBuilderComputation
    : public pxr::HdComputedBufferSource {
   public:
    HdSt_TriangleIndexBuilderComputation(
        HdSt_MeshTopology* topology,
        pxr::SdfPath const& id);
    virtual void GetBufferSpecs(pxr::HdBufferSpecVector* specs) const override;
    virtual bool Resolve() override;

    virtual bool HasChainedBuffer() const override;
    virtual pxr::HdBufferSourceSharedPtrVector GetChainedBuffers()
        const override;

   protected:
    virtual bool _CheckValid() const override;

   private:
    pxr::SdfPath const _id;
    HdSt_MeshTopology* _topology;
    pxr::HdBufferSourceSharedPtr _primitiveParam;
    pxr::HdBufferSourceSharedPtr _trianglesEdgeIndices;
};

//

/// \class HdSt_TriangulateFaceVaryingComputation
///
/// CPU face-varying triangulation.
///
class HdSt_TriangulateFaceVaryingComputation
    : public pxr::HdComputedBufferSource {
   public:
    HdSt_TriangulateFaceVaryingComputation(
        HdSt_MeshTopology* topolgoy,
        pxr::HdBufferSourceSharedPtr const& source,
        pxr::SdfPath const& id);

    virtual void GetBufferSpecs(pxr::HdBufferSpecVector* specs) const override;
    virtual bool Resolve() override;

   protected:
    virtual bool _CheckValid() const override;

   private:
    pxr::SdfPath const _id;
    HdSt_MeshTopology* _topology;
    pxr::HdBufferSourceSharedPtr _source;
};

RUZINO_NAMESPACE_CLOSE_SCOPE

#endif  // HdSt_TRIANGULATE_H
