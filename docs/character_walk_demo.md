# Character Walking Simulator Demo

A game-engine-style third-person walking demo: launch Ruzino.exe with the
generated USD scene, WASD to walk a humanoid BOT on a flat checkerboard
ground under the Hosek-Wilkie sky, with path-traced sun shadows and a smooth
follow camera.

```
python scripts/gen_character_walk_scene.py
Binaries/Release/Ruzino.exe Binaries/Release/demo_scenes/character_walk.usda
```

Controls: `W/A/S/D` move (camera-relative), `Shift` run, middle-drag orbit,
wheel zoom, `Free Camera > Follow Character` toggles the follow lock.
**Note:** the viewport defaults to the HdStorm renderer — switch to
`Renderer > Select Renderer > Hd_RUZINO_RendererPlugin` for the path-traced
sky/shadows look this scene is authored for.

## Architecture

Two new Runtime modules plus small, principled wiring into the existing
input/camera/stage paths. Nothing hacks around existing systems.

```
GLFW events ──> DeviceManager ──> DockingImguiRenderer (window.cpp)
                                        │  publishes raw key/mouse (filtered
                                        │  by ImGui WantCapture*)
                                        v
                              input::InputState            (Runtime/input)
                                        ^  set_view_frame() published by
                                        │  UsdviewEngine each frame
Stage::tick ──> character::CharacterControllerSystem      (Runtime/character)
                     │ discovers prims with character:controller=true
                     │ input -> kinematic move -> procedural gait (gait.cpp)
                     v
        matrix xformOp overs authored in the session (modifier) layer
                     │
                     v
        UsdImagingDelegate/Hydra picks them up -> renderer (zero changes)
```

### `source/Runtime/input` — InputState

Process-wide input snapshot: held/pressed key sets, mouse deltas, a
normalized WASD move axis, an axis override for headless control, and the
ground-projected camera view frame. The window layer publishes; gameplay
consumes. `begin_frame()` (registered in Ruzino.cpp ahead of `Stage::tick`)
marks frame boundaries. This extracts what used to be an implicit
render-pass → widget → camera chain into a first-class module any consumer
can read.

### `source/Runtime/character` — skeleton + gait + controller system

* **Skeleton** (`skeleton.cpp`): the rig is authored directly in USD — one
  Xform prim per joint (`Hips`, `Spine`, `Chest`, `Neck`, `Head`,
  `L/R_UpperArm/Forearm/Hand`, `L/R_Thigh/Shin/Foot`), limb meshes parented
  under their joint. Discovery walks the prim subtree and captures rest
  poses. `evaluate_fk()` exposes a world matrix palette for a future GPU
  skinning pipeline (the renderer's unused `SkinningVertexData` structs are
  the intended consumer) without changing how scenes are authored.
* **Gait** (`gait.cpp`): pure procedural math — sinusoidal walk cycle
  (hip/knee/arm swing, pelvis twist, bob, sway) blended with an idle
  breathing pose by a walk weight derived from ground speed.
* **CharacterControllerSystem**: entt ECS system called from `Stage::tick`.
  Discovers characters via the custom `character:controller = true` marker
  attribute (same discovery pattern as the legacy `Animatable` marker), reads
  tunables (`character:moveSpeed`, `character:runMultiplier`,
  `character:strideLength`), integrates kinematic motion (flat-ground
  constraint, velocity/heading smoothing), and authors **single matrix
  xformOps** (`xformOp:transform`, R then T in Gf's row-vector convention)
  into the **session layer** — the non-destructive path the modifier system
  uses, so the scene file stays pristine. No-op writes are suppressed
  so a standing character does not reset the path tracer's accumulation.

Movement is camera-relative: the viewport publishes its ground-projected
forward/right into `InputState::set_view_frame()` every frame.

### Camera follow (`free_camera.cpp`, `usdview_widget.cpp`)

`ThirdPersonCamera` gained a follow mode: the orbit target tracks a prim's
world position with exponential smoothing (orbit/zoom still work on top).
The target is USD-driven — the scene authors
`third_person:followTarget = "/Character"` on `/FreeCamera`; the viewport
reads it each frame, and the menu item toggles the same attribute.

### Scene (`scripts/gen_character_walk_scene.py`)

Everything is in the USD file (Z-up, meters): checkerboard ground (new
`callables/demo_ground.slang`, human-scale 2 m cells, ground normals +Z),
DistantLight sun + Hosek dome, three UsdPreviewSurface materials, the
16-joint character tree with capsule/box/sphere meshes, and the
follow-camera state.

### Headless driving / tests

`stage_py.Stage.set_move_input(x, y)` injects the move axis without a
window (used by `source/tests/test_character_walk.py`). The character system
runs on every `tick()` unconditionally — it is *not* gated by the
`render_time >= current_time` simulation rule, since gameplay advances with
wall clock. Two behaviors worth knowing:

* `Stage(path)` reloads the `<stem>_modifiers.usda` sidecar into the session
  layer, and the controller *resumes* from the last authored root transform —
  reopening a stage continues where you left off. Delete the sidecar to reset
  the walk. Tests generate fresh scenes per test for this reason.
* Spec-level readback uses fully-qualified property paths
  (`layer.GetPropertyAtPath("/Character.xformOp:transform")`); the colon in
  namespaced attribute names does not parse as a standalone SdfPath.

Offline visual check: `source/tests/render_character_walk.py` renders the
rest pose and a mid-stride frame (session pose baked into vertex arrays) to
PNGs under `Binaries/Release/test_output/character_walk/`.

## Renderer limitations discovered (filed for future work)

These are pre-existing offline-render-path bugs this Z-up scene exposed;
the interactive viewport (UsdImagingGLEngine) is unaffected:

1. **Dome transforms are ignored.** The Hosek sky callable evaluates in a
   fixed Y-up frame (`light.cpp` stores the dome prim transform but the
   sampling convention never changes the dome's zenith axis — 24 axis-aligned
   dome rotations rendered identically). This scene compensates by authoring
   `sunDirection` in the baked Y-up frame `(x, z, y)` and boosting dome
   intensity for the grazing sky light a Z-up ground receives.
2. **Prim transforms are ignored by the offline HydraRenderer.** Geometry
   renders from authored vertex positions; hierarchy/mesh xformOps never
   reach the instance placement (which is why cloud/wetbrush scenes move
   geometry by writing vertex arrays). The offline walk render works around
   it by baking the posed joint matrices into each mesh's points/normals
   **and its extent** (stale extents cull the mesh).
3. **Some camera parameterizations render an empty noise frame.** The
   offline path has frustum-dependent failures unrelated to scene content
   (a second HydraRenderer in one process also returns the first render's
   stale accumulation — the script renders each frame in a subprocess).
4. **Multi-op CommonAPI xform stacks mis-evaluate** (translate+rotateXYZ
   renders unlit/black), and plain translate ops mis-render in some paths;
   single matrix ops work everywhere. The controller and scene generator
   author matrix ops for this reason.

## Notes / limitations

* Shadows are ray-traced hard shadows from the path tracer; the raster path
  has no shadows, so use the Hd_RUZINO renderer for this scene.
* While the character moves, progressive accumulation restarts each frame
  (geometry changes); the image converges once you stop — the expected
  behavior for a path-traced viewport.
* The gait is analytic (no foot IK): on perfectly flat ground it reads
  correctly; slopes would need a ground-height query (PhysicsSystem's
  SceneQuerySystem stub is the natural future home).
