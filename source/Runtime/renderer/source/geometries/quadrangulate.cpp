//
// Copyright 2016 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "quadrangulate.h"

#include "bufferResource.h"
#include "meshTopology.h"
#include "pxr/imaging/hd/bufferArrayRange.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/perfLog.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/types.h"
#include "pxr/imaging/hd/vtBufferSource.h"
#include "pxr/imaging/hgi/computePipeline.h"
#include "pxr/imaging/hgi/hgi.h"
#include "pxr/imaging/hio/glslfx.h"
#include "pxr/pxr.h"

RUZINO_NAMESPACE_OPEN_SCOPE

enum {
    BufferBinding_Uniforms,
    BufferBinding_Primvar,
    BufferBinding_Quadinfo,
};

HdSt_QuadInfoBuilderComputation::HdSt_QuadInfoBuilderComputation(
    HdSt_MeshTopology* topology,
    pxr::SdfPath const& id)
    : _id(id),
      _topology(topology)
{
}

bool HdSt_QuadInfoBuilderComputation::Resolve()
{
    if (!_TryLock())
        return false;

    pxr::HdQuadInfo* quadInfo = new pxr::HdQuadInfo();
    HdMeshUtil meshUtil(_topology, _id);
    meshUtil.ComputeQuadInfo(quadInfo);

    // Set quadinfo to topology
    // topology takes ownership of quadinfo so no need to free.
    _topology->SetQuadInfo(quadInfo);

    _SetResolved();
    return true;
}

bool HdSt_QuadInfoBuilderComputation::_CheckValid() const
{
    return true;
}

// ---------------------------------------------------------------------------

HdSt_QuadIndexBuilderComputation::HdSt_QuadIndexBuilderComputation(
    HdSt_MeshTopology* topology,
    HdSt_QuadInfoBuilderComputationSharedPtr const& quadInfoBuilder,
    pxr::SdfPath const& id)
    : _id(id),
      _topology(topology),
      _quadInfoBuilder(quadInfoBuilder)
{
}

void HdSt_QuadIndexBuilderComputation::GetBufferSpecs(
    pxr::HdBufferSpecVector* specs) const
{
    if (_topology->TriangulateQuads()) {
        specs->emplace_back(HdTokens->indices, HdTupleType{ HdTypeInt32, 6 });
    }
    else {
        specs->emplace_back(HdTokens->indices, HdTupleType{ HdTypeInt32, 4 });
    }
    // coarse-quads uses int2 as primitive param.
    specs->emplace_back(
        HdTokens->primitiveParam, HdTupleType{ HdTypeInt32, 1 });
    // 2 edge indices per quad
    specs->emplace_back(
        HdTokens->edgeIndices, HdTupleType{ HdTypeInt32Vec2, 1 });
}

bool HdSt_QuadIndexBuilderComputation::Resolve()
{
    // quadInfoBuilder may or may not exists, depending on how we switched
    // the repr of the mesh. If it exists, we have to wait.
    if (_quadInfoBuilder && !_quadInfoBuilder->IsResolved())
        return false;

    if (!_TryLock())
        return false;

    HD_TRACE_FUNCTION();

    // generate quad index buffer
    VtIntArray quadsFaceVertexIndices;
    VtIntArray primitiveParam;
    VtVec2iArray quadsEdgeIndices;
    HdMeshUtil meshUtil(_topology, _id);
    if (_topology->TriangulateQuads()) {
        meshUtil.ComputeTriQuadIndices(
            &quadsFaceVertexIndices, &primitiveParam, &quadsEdgeIndices);
    }
    else {
        meshUtil.ComputeQuadIndices(
            &quadsFaceVertexIndices, &primitiveParam, &quadsEdgeIndices);
    }

    if (_topology->TriangulateQuads()) {
        _SetResult(
            std::make_shared<HdVtBufferSource>(
                HdTokens->indices, VtValue(quadsFaceVertexIndices), 6));
    }
    else {
        _SetResult(
            std::make_shared<HdVtBufferSource>(
                HdTokens->indices, VtValue(quadsFaceVertexIndices), 4));
    }

    _primitiveParam.reset(new HdVtBufferSource(
        HdTokens->primitiveParam, VtValue(primitiveParam)));

    _quadsEdgeIndices.reset(
        new HdVtBufferSource(HdTokens->edgeIndices, VtValue(quadsEdgeIndices)));

    _SetResolved();
    return true;
}

bool HdSt_QuadIndexBuilderComputation::HasChainedBuffer() const
{
    return true;
}

HdBufferSourceSharedPtrVector
HdSt_QuadIndexBuilderComputation::GetChainedBuffers() const
{
    return { _primitiveParam, _quadsEdgeIndices };
}

bool HdSt_QuadIndexBuilderComputation::_CheckValid() const
{
    return true;
}

// ---------------------------------------------------------------------------

HdSt_QuadrangulateComputation::HdSt_QuadrangulateComputation(
    HdSt_MeshTopology* topology,
    pxr::HdBufferSourceSharedPtr const& source,
    pxr::HdBufferSourceSharedPtr const& quadInfoBuilder,
    pxr::SdfPath const& id)
    : _id(id),
      _topology(topology),
      _source(source),
      _quadInfoBuilder(quadInfoBuilder)
{
}

bool HdSt_QuadrangulateComputation::Resolve()
{
    if (!TF_VERIFY(_source))
        return false;
    if (!_source->IsResolved())
        return false;
    if (_quadInfoBuilder && !_quadInfoBuilder->IsResolved())
        return false;

    if (!_TryLock())
        return false;

    HD_TRACE_FUNCTION();

    HD_PERF_COUNTER_INCR(HdPerfTokens->quadrangulateCPU);

    HdQuadInfo const* quadInfo = _topology->GetQuadInfo();
    if (!TF_VERIFY(quadInfo))
        return true;

    // If the topology is all quads, just return source.
    // This check is needed since if the topology changes, we don't know
    // whether the topology is all-quads or not until the quadinfo computation
    // is resolved. So we conservatively register primvar quadrangulations
    // on that case, it hits this condition. Once quadinfo resolved on the
    // topology, HdSt_MeshTopology::GetQuadrangulateComputation returns null
    // and nobody calls this function for all-quads prims.
    if (quadInfo->IsAllQuads()) {
        _SetResult(_source);
        _SetResolved();
        return true;
    }

    VtValue result;
    HdMeshUtil meshUtil(_topology, _id);
    if (meshUtil.ComputeQuadrangulatedPrimvar(
            quadInfo,
            _source->GetData(),
            _source->GetNumElements(),
            _source->GetTupleType().type,
            &result)) {
        HD_PERF_COUNTER_ADD(
            HdPerfTokens->quadrangulatedVerts, quadInfo->numAdditionalPoints);

        _SetResult(
            std::make_shared<HdVtBufferSource>(_source->GetName(), result));
    }
    else {
        _SetResult(_source);
    }

    _SetResolved();
    return true;
}

void HdSt_QuadrangulateComputation::GetBufferSpecs(
    pxr::HdBufferSpecVector* specs) const
{
    // produces same spec buffer as source
    _source->GetBufferSpecs(specs);
}

HdTupleType HdSt_QuadrangulateComputation::GetTupleType() const
{
    return _source->GetTupleType();
}

bool HdSt_QuadrangulateComputation::_CheckValid() const
{
    return (_source->IsValid());
}

bool HdSt_QuadrangulateComputation::HasPreChainedBuffer() const
{
    return true;
}

HdBufferSourceSharedPtr HdSt_QuadrangulateComputation::GetPreChainedBuffer()
    const
{
    return _source;
}

// ---------------------------------------------------------------------------

HdSt_QuadrangulateFaceVaryingComputation::
    HdSt_QuadrangulateFaceVaryingComputation(
        HdSt_MeshTopology* topology,
        HdBufferSourceSharedPtr const& source,
        SdfPath const& id)
    : _id(id),
      _topology(topology),
      _source(source)
{
}

bool HdSt_QuadrangulateFaceVaryingComputation::Resolve()
{
    if (!TF_VERIFY(_source))
        return false;
    if (!_source->IsResolved())
        return false;

    if (!_TryLock())
        return false;

    HD_TRACE_FUNCTION();
    HD_PERF_COUNTER_INCR(HdPerfTokens->quadrangulateFaceVarying);

    // XXX: we could skip this if the mesh is all quads, like above in
    // HdSt_QuadrangulateComputation::Resolve()...

    VtValue result;
    HdMeshUtil meshUtil(_topology, _id);
    if (meshUtil.ComputeQuadrangulatedFaceVaryingPrimvar(
            _source->GetData(),
            _source->GetNumElements(),
            _source->GetTupleType().type,
            &result)) {
        _SetResult(
            std::make_shared<HdVtBufferSource>(_source->GetName(), result));
    }
    else {
        _SetResult(_source);
    }

    _SetResolved();
    return true;
}

void HdSt_QuadrangulateFaceVaryingComputation::GetBufferSpecs(
    pxr::HdBufferSpecVector* specs) const
{
    // produces same spec buffer as source
    _source->GetBufferSpecs(specs);
}

bool HdSt_QuadrangulateFaceVaryingComputation::_CheckValid() const
{
    return (_source->IsValid());
}

RUZINO_NAMESPACE_CLOSE_SCOPE
