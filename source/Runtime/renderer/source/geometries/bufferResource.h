//
// Copyright 2017 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#ifndef PXR_IMAGING_HD_ST_BUFFER_RESOURCE_H
#define PXR_IMAGING_HD_ST_BUFFER_RESOURCE_H

#include <memory>
#include <utility>
#include <vector>

#include "api.h"
#include "pxr/base/tf/token.h"
#include "pxr/imaging/hd/types.h"
#include "pxr/imaging/hgi/buffer.h"
#include "pxr/pxr.h"

RUZINO_NAMESPACE_OPEN_SCOPE

using HdStBufferResourceSharedPtr = std::shared_ptr<class HdStBufferResource>;

using HdStBufferResourceNamedPair =
    std::pair<pxr::TfToken, HdStBufferResourceSharedPtr>;
using HdStBufferResourceNamedList = std::vector<HdStBufferResourceNamedPair>;

/// \class HdStBufferResource
///
/// A GPU resource contained within an underlying HgiBuffer.
///
class HdStBufferResource final {
   public:
    HD_RUZINO_API
    HdStBufferResource(
        pxr::TfToken const& role,
        pxr::HdTupleType tupleType,
        int offset,
        int stride);

    HD_RUZINO_API
    ~HdStBufferResource();

    /// Returns the role of the data in this resource.
    pxr::TfToken const& GetRole() const
    {
        return _role;
    }

    /// Returns the size (in bytes) of the data.
    size_t GetSize() const
    {
        return _size;
    }

    /// Data type and count
    pxr::HdTupleType GetTupleType() const
    {
        return _tupleType;
    }

    /// Returns the interleaved offset (in bytes) of the data.
    int GetOffset() const
    {
        return _offset;
    }

    /// Returns the stride (in bytes) between data elements.
    int GetStride() const
    {
        return _stride;
    }

    /// Sets the HgiBufferHandle for this resource and its size.
    HD_RUZINO_API
    void SetAllocation(pxr::HgiBufferHandle const& handle, size_t size);

    /// Returns the HgiBufferHandle for this GPU resource.
    pxr::HgiBufferHandle& GetHandle()
    {
        return _handle;
    }

   private:
    HdStBufferResource(const HdStBufferResource&) = delete;
    HdStBufferResource& operator=(const HdStBufferResource&) = delete;

    pxr::HgiBufferHandle _handle;
    size_t _size;

    pxr::TfToken const _role;
    pxr::HdTupleType const _tupleType;
    int const _offset;
    int const _stride;
};

RUZINO_NAMESPACE_CLOSE_SCOPE

#endif  // PXR_IMAGING_HD_ST_BUFFER_RESOURCE_H
