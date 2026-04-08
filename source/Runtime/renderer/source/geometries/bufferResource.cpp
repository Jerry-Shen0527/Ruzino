//
// Copyright 2017 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//

#include "bufferResource.h"

RUZINO_NAMESPACE_OPEN_SCOPE

HdStBufferResource::HdStBufferResource(
    pxr::TfToken const& role,
    pxr::HdTupleType tupleType,
    int offset,
    int stride)
    : _size(0),
      _role(role),
      _tupleType(tupleType),
      _offset(offset),
      _stride(stride)
{
}

HdStBufferResource::~HdStBufferResource() = default;

void HdStBufferResource::SetAllocation(
    pxr::HgiBufferHandle const& handle,
    size_t size)
{
    _handle = handle;
    _size = size;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
