#pragma once
#include "pxr/base/tf/hash.h"
#include "pxr/base/tf/hashmap.h"
RUZINO_NAMESPACE_OPEN_SCOPE

class Hd_RUZINO_Light;
class Hd_RUZINO_Camera;
class Hd_RUZINO_Mesh;
class Hd_RUZINO_Material;

using LightArray = pxr::VtArray<Hd_RUZINO_Light*>;
using CameraArray = pxr::VtArray<Hd_RUZINO_Camera*>;
using MeshArray = pxr::VtArray<Hd_RUZINO_Mesh*>;
using MaterialMap =
    pxr::TfHashMap<pxr::SdfPath, Hd_RUZINO_Material*, pxr::TfHash>;

RUZINO_NAMESPACE_CLOSE_SCOPE
