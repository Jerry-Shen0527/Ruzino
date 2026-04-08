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

#include "field.h"

#include <spdlog/spdlog.h>

#include "../renderParam.h"
#include "pxr/imaging/hd/tokens.h"

RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;

Hd_RUZINO_Field::Hd_RUZINO_Field(const SdfPath& id)
    : HdField(id),
      _isLoaded(false)
{
    spdlog::info("Creating field: {}", id.GetText());
}

Hd_RUZINO_Field::~Hd_RUZINO_Field()
{
}

HdDirtyBits Hd_RUZINO_Field::GetInitialDirtyBitsMask() const
{
    return HdField::DirtyTransform | HdField::DirtyParams | HdField::AllDirty;
}

void Hd_RUZINO_Field::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits)
{
    HD_TRACE_FUNCTION();
    HF_MALLOC_TAG_FUNCTION();

    const SdfPath& id = GetId();

    spdlog::info("Syncing field: {}", id.GetText());

    // Get field file path
    if (*dirtyBits & HdField::DirtyParams) {
        VtValue filePathValue = sceneDelegate->Get(id, HdFieldTokens->filePath);
        if (!filePathValue.IsEmpty() &&
            filePathValue.IsHolding<SdfAssetPath>()) {
            SdfAssetPath assetPath = filePathValue.Get<SdfAssetPath>();
            _filePath = assetPath.GetResolvedPath();
            if (_filePath.empty()) {
                _filePath = assetPath.GetAssetPath();
            }
            spdlog::info("Field file path: {}", _filePath.c_str());
        }

        // Get field name
        VtValue fieldNameValue =
            sceneDelegate->Get(id, HdFieldTokens->fieldName);
        if (!fieldNameValue.IsEmpty() && fieldNameValue.IsHolding<TfToken>()) {
            _fieldName = fieldNameValue.Get<TfToken>().GetString();
        }
        else if (
            !fieldNameValue.IsEmpty() &&
            fieldNameValue.IsHolding<std::string>()) {
            _fieldName = fieldNameValue.Get<std::string>();
        }

        if (_fieldName.empty()) {
            _fieldName = "density";  // Default field name
        }

        spdlog::info("Field name: {}", _fieldName.c_str());

        // Mark as loaded (placeholder - actual loading would happen here)
        _isLoaded = true;
    }

    *dirtyBits &= ~HdField::AllDirty;
}

void Hd_RUZINO_Field::Finalize(HdRenderParam* renderParam)
{
    // Reset state
    _filePath.clear();
    _fieldName.clear();
    _isLoaded = false;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
