#pragma once

// Centralized names for viewport-input events that flow on the
// window->events() broadcast bus (emit_any / subscribe_any).
//
// These events are emitted by the viewport widget (UsdviewEngine) and
// consumed by any number of subscribers (e.g. one per open geometry editor).
// Using the broadcast bus instead of destroy-on-read `consume_*` polling
// guarantees every subscriber sees every event — fixing the multi-editor
// starvation bug where the first editor to call consume_* stole the event.
//
// Canonical payload types (used with std::any_cast):
//   BRUSH_STATE     -> ViewportBrushState (see GCore/geom_payload.hpp)
//   PICK_EVENT      -> std::shared_ptr<PickEvent>  (see GCore/geom_payload.hpp)
//   EDITOR_CREATION -> pxr::SdfPath

namespace ViewportEvents {
// Brush stroke input — emitted on mouse drag (pen-down + move) and pen-up.
// Payload: ViewportBrushState.
inline constexpr const char* BRUSH_STATE = "viewport_brush_state";

// Pick — emitted on a successful viewport intersection click.
// Payload: std::shared_ptr<PickEvent>.
inline constexpr const char* PICK_EVENT = "viewport_pick_event";

// Editor creation request — emitted from the prim context-menu "Edit" action.
// Payload: pxr::SdfPath (the prim to open an editor for).
inline constexpr const char* EDITOR_CREATION = "viewport_editor_creation";
}  // namespace ViewportEvents
