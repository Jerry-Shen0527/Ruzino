//
// Copyright 2017 Pixar
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
#pragma once
#include <future>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "DescriptorTableManager.h"
#include "RHI/rhi.hpp"
#include "api.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/renderThread.h"
#include "pxr/pxr.h"
#include "renderTLAS.h"

namespace Ruzino {
class Hd_RUZINO_Material;
class NodeSystem;
class LensSystem;
using MaterialMap = pxr::TfHashMap<SdfPath, Hd_RUZINO_Material*, TfHash>;
}  // namespace Ruzino

namespace Ruzino {
struct RenderGlobalPayload;
}

RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;

///
/// \class Hd_RUZINO_RenderParam
///
/// The render delegate can create an object of type HdRenderParam, to pass
/// to each prim during Sync(). Hd_RUZINO uses this class to pass top-level
/// embree state around.
///
class Hd_RUZINO_RenderParam final : public HdRenderParam {
   public:
    Hd_RUZINO_RenderParam(
        HdRenderThread* renderThread,
        std::atomic<int>* sceneVersion,
        NodeSystem* node_system,
        MaterialMap* m)
        : _renderThread(renderThread),
          material_map(m),
          _sceneVersion(sceneVersion),
          node_system(node_system)

    {
        InstanceCollection =
            std::make_unique<Hd_RUZINO_RenderInstanceCollection>();
    }

    ~Hd_RUZINO_RenderParam()
    {
    }

    HdRenderThread* _renderThread = nullptr;

    MaterialMap* material_map;

    NodeSystem* node_system;
    std::unique_ptr<Hd_RUZINO_RenderInstanceCollection> InstanceCollection;

    // Support multiple named output textures from present nodes
    std::map<std::string, nvrhi::TextureHandle> presented_textures;

    // Legacy: name of default texture in presented_textures (for backward
    // compatibility) This avoids duplication - just stores which texture is the
    // default one
    std::string default_texture_name;

    LensSystem* lens_system = nullptr;

    // Per-instance version tracking for dirty detection.
    // Moved from static locals in renderer.cpp to fix multi-instance bug.
    uint32_t last_material_version = 0;
    uint32_t last_geometry_version = 0;
    uint32_t last_light_version = 0;

    std::vector<std::thread> texture_loading_threads;
    std::vector<std::thread> material_loading_threads;

   private:
    /// A handle to the global render thread.
    /// A version counter for edits to _scene.
    std::atomic<int>* _sceneVersion;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
