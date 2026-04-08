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
#ifndef PXR_IMAGING_PLUGIN_HD_EMBREE_RENDER_PARAM_H
#define PXR_IMAGING_PLUGIN_HD_EMBREE_RENDER_PARAM_H

#include "api.h"
#include "nodes/system/node_system.hpp"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/renderThread.h"
#include "pxr/pxr.h"

RUZINO_NAMESPACE_OPEN_SCOPE
class Hd_RUZINO_Material;
class Hd_RUZINO_Mesh;
class Hd_RUZINO_Light;
class Hd_RUZINO_Camera;
using namespace pxr;

///
/// \class Hd_RUZINO_RenderParam
///
/// The render delegate can create an object of type HdRenderParam, to pass
/// to each prim during Sync(). HdEmbree uses this class to pass top-level
/// embree state around.
///
class Hd_RUZINO_RenderParam final : public HdRenderParam {
   public:
    Hd_RUZINO_RenderParam(
        HdRenderThread* renderThread,
        std::atomic<int>* sceneVersion,
        pxr::VtArray<Hd_RUZINO_Light*>* lights,
        pxr::VtArray<Hd_RUZINO_Camera*>* cameras,
        pxr::VtArray<Hd_RUZINO_Mesh*>* meshes,
        pxr::TfHashMap<SdfPath, Hd_RUZINO_Material*, TfHash>* materials)
        : _renderThread(renderThread),
          _sceneVersion(sceneVersion),
          lights(lights),
          cameras(cameras),
          meshes(meshes),
          materials(materials)
    {
    }
    HdRenderThread* _renderThread = nullptr;

    NodeSystem* node_system;

    pxr::VtArray<Hd_RUZINO_Light*>* lights = nullptr;
    pxr::VtArray<Hd_RUZINO_Camera*>* cameras = nullptr;
    pxr::VtArray<Hd_RUZINO_Mesh*>* meshes = nullptr;
    pxr::TfHashMap<SdfPath, Hd_RUZINO_Material*, TfHash>* materials = nullptr;

   private:
    /// A handle to the global render thread.
    /// A version counter for edits to _scene.
    std::atomic<int>* _sceneVersion;
};

RUZINO_NAMESPACE_CLOSE_SCOPE

#endif  // PXR_IMAGING_PLUGIN_HD_EMBREE_RENDER_PARAM_H
