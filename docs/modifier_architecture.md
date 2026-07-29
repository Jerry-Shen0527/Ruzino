# Modifier Architecture

> **实现状态**：本架构已落地。`write_geometry_as_over_spec()`、`GeomPayload::is_modifier_mode`、
> `node_input_geometry` / `node_write_usd` 的 modifier 分支都已实现
> （见 `source/Editor/geometry/usd_extension/usd_extension.cpp`、
> `source/Editor/geometry_nodes/node_input_geometry.cpp`、`node_write_usd.cpp`）。
> `animation.cpp` 里 `is_modifier_mode` 现在恒为 true（始终走 modifier 层）。
> 本文 originally 是设计提案，下面保留原 "Current vs Target" 对比作为背景说明，
> 但请以「Target 架构 = 当前实际架构」来理解。

## Overview

This document describes the modifier-based architecture that leverages USD's layer composition
capabilities for non-destructive geometry editing. The architecture is implemented; the
"Current (Direct Write)" flow below describes the legacy behavior that has been superseded.

## Legacy vs Current Architecture

### Legacy (Direct Write) Architecture — 已废弃
```
┌─────────────────────────────────────────────────────────────┐
│                   Current Flow                                  │
├─────────────────────────────────────────────────────────────┤
│  UI/AnimationSystem                                             │
│      ↓                                                          │
│  GeomPayload (stage, prim_path, current_time)              │
│      ↓                                                          │
│  Node Graph Execution                                            │
│      ↓                                                          │
│  write_usd node: Directly OVERWRITES prim data               │
│      write_geometry_to_usd(geom, stage, prim_path, time)      │
│      ↓                                                          │
│  ❌ Original data is lost, no non-destructive editing           │
└─────────────────────────────────────────────────────────────┘
```

### Current (Modifier Layer) Architecture — 已实现
```
┌─────────────────────────────────────────────────────────────┐
│                   New Flow                                     │
├─────────────────────────────────────────────────────────────┤
│  UI/AnimationSystem                                             │
│      ↓                                                          │
│  GeomPayload (stage, prim_path, current_time, modifier_stack)  │
│      ↓                                                          │
│  Node Graph Execution                                            │
│      ↓                                                          │
│  write_modifier node: Writes to OVER SPEC in modifier layer   │
│      ↓                                                          │
│  USD Layer Composition                                             │
│  Root Layer (original) ← Modifier Layer (over spec) = Final     │
│      ↓                                                          │
│  ✅ Non-destructive, lever-based composition, full USD advantages    │
└─────────────────────────────────────────────────────────────┘
```

## Data Structures

### 1. Modifier Info Structure

```cpp
struct ModifierInfo {
    std::string name;              // e.g., "Subdivision", "Deform"
    int order;                 // Execution order in stack
    bool enabled;
    pxr::SdfPath prim_path;      // Path to the prim being modified
 e.g., "/geometry/mesh_a"
    std::string node_graph_json;  // Node graph definition stored in prim attribute
 e.g., "modifier_0_json"
    pxr::SdfPath output_path;     // Path where modifier output is written
 e.g., "/geometry/mesh_a/modifiers/modifier_0"
};

struct ModifierStack {
    std::vector<ModifierInfo> modifiers;
    std::string storage_mode;  // "prim_attribute", "child_prims", "external_file"
};
```

### 2. GeomPayload Extension

```cpp
struct GeomPayload {
    // ... existing fields ...
    
    // Modifier support
    ModifierStack* modifier_stack;           // Active modifier stack for this prim
    int current_modifier_index;              // Which modifier in stack we executing
    pxr::SdfPath modifier_output_path;      // Where current modifier writes to

    // Layer management
    pxr::SdfPath modifier_layer_path;       // Path to modifier layer (e.g., session or external)
    bool is_modifier_mode;                 // true = modifier mode, false = direct write
 };
```

## Key Modifications

### 1. GeomPayload Changes

**File**: `source/Editor/geometry/include/GCore/geom_payload.hpp`

```cpp
struct GeomPayload {
#ifdef GEOM_USD_EXTENSION
    pxr::UsdStageRefPtr stage;
    pxr::UsdTimeCode current_time = pxr::UsdTimeCode(0);
    pxr::SdfPath prim_path;
    
    // Modifier support
    ModifierStack* modifier_stack = nullptr;
    int current_modifier_index = -1;
    pxr::SdfPath modifier_output_path;
    pxr::SdfPath modifier_layer_path;
    bool is_modifier_mode = false;
#endif

    // ... existing fields ...
};
```

### 2. Input Geometry Node (复用Group In)

**File**: `source/Editor/geometry_nodes/node_input_geometry.cpp` (NEW)

```cpp
NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(input_geometry)
{
    b.add_input<pxr::SdfPath>("Prim Path");
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(input_geometry)
{
    auto prim_path = params.get_input<pxr::SdfPath>("Prim Path");
    auto& global_payload = params.get_global_payload<GeomPayload&>();
    
    if (!global_payload.stage) {
        spdlog::error("No stage in payload");
        return false;
    }
    
    auto prim = global_payload.stage->GetPrimAtPath(prim_path);
    if (!prim) {
        spdlog::error("Prim not found: {}", prim_path.GetString());
        return false;
    }
    
    Geometry geometry;
    if (!read_geometry_from_usd(geometry, prim, global_payload.current_time)) {
        spdlog::error("Failed to read geometry from prim");
        return false;
    }
    
    params.set_output("Geometry", geometry);
    return true;
}

NODE_DECLARATION_REQUIRED(input_geometry);
NODE_DECLARATION_UI(input_geometry);

NODE_DEF_CLOSE_SCOPE
```

### 3. Modified write_usd Node

**File**: `source/Editor/geometry_nodes/node_write_usd.cpp`

Current implementation writes directly to the prim. In modifier mode it writes as an over spec instead (see `node_write_usd.cpp`):

```cpp
NODE_EXECUTION_FUNCTION(write_usd)
{
    auto& global_payload = params.get_global_payload<GeomPayload&>();
    auto geometry = params.get_input<Geometry>("Geometry");
    
    // ... existing code ...
    
    // NEW: Check modifier mode
    if (global_payload.is_modifier_mode) {
        // Write to over spec instead of direct write
        auto output_path = global_payload.modifier_output_path;
        return write_geometry_as_over_spec(
            geometry, 
            global_payload.stage, 
            output_path, 
            global_payload.current_time
        );
    } else {
        // Legacy: Direct write (backward compatible)
        return write_geometry_to_usd(
            geometry, 
            global_payload.stage, 
            sdf_path, 
            global_payload.current_time
        );
    }
}
```

### 4. USD Extension Functions

**File**: `source/Editor/geometry/usd_extension/include/GCore/usd_extension.h`

Add new functions for modifier mode:

```cpp
// Existing: Direct write (overwrites data)
bool GEOMETRY_API write_geometry_to_usd(
    const Geometry& geometry,
    pxr::UsdStageRefPtr stage,
    const pxr::SdfPath& sdf_path,
    pxr::UsdTimeCode time);

// NEW: Write as over spec (non-destructive)
bool GEOMETRY_API write_geometry_as_over_spec(
    const Geometry& geometry,
    pxr::UsdStageRefPtr stage,
    const pxr::SdfPath& sdf_path,
    pxr::UsdTimeCode time);

// NEW: Create over spec in modifier layer
bool GEOMETRY_API create_over_spec_for_modifier(
    pxr::UsdStageRefPtr stage,
    const pxr::SdfPath& prim_path,
    const pxr::SdfPath& modifier_layer_path);

// NEW: Clear over spec (remove modifier)
bool GEOMETRY_API clear_over_spec(
    pxr::UsdStageRefPtr stage,
    const pxr::SdfPath& sdf_path);
```

### 5. Animation System Changes

**File**: `source/Runtime/stage/source/animation.cpp`

Support for modifier stack execution:

```cpp
void WithDynamicLogicPrim::update(float delta_time) const
{
    // ... existing code ...
    
    // NEW: Load modifier stack from prim
    auto modifier_stack = load_modifier_stack(prim);
    
    auto& payload = node_tree_executor->get_global_payload<GeomPayload&>();
    payload.modifier_stack = &modifier_stack;
    payload.is_modifier_mode = !modifier_stack.modifiers.empty();
    
    // NEW: Execute modifiers in order
    if (payload.is_modifier_mode) {
        for (int i = 0; i < modifier_stack.modifiers.size(); ++i) {
            auto& mod = modifier_stack.modifiers[i];
            if (!mod.enabled) continue;
            
            payload.current_modifier_index = i;
            payload.modifier_output_path = GetModifierOutputPath(mod);
            
            // Execute this modifier's node graph
            node_tree_executor->execute(node_tree.get());
        }
    } else {
        // Legacy: Execute single node graph
        node_tree_executor->execute(node_tree.get());
    }
}
```

### 6. Stage Class Extensions

**File**: `source/Runtime/stage/include/stage/stage.hpp`

Add modifier management:

```cpp
class Stage {
public:
    // ... existing methods ...
    
    // NEW: Modifier management
    ModifierStack load_modifier_stack(const pxr::SdfPath& prim_path);
    void save_modifier_stack(const pxr::SdfPath& prim_path, const ModifierStack& stack);
    
    pxr::SdfPath get_modifier_layer_path(const pxr::SdfPath& prim_path, int modifier_index);
    pxr::SdfPath get_modifier_output_path(const pxr::SdfPath& prim_path, int modifier_index);
    
private:
    std::map<pxr::SdfPath, ModifierStack> modifier_stacks_;
};
```

## Implementation Details

### Over Spec Mechanism

USD's SdfPrimSpec allows layer-specific opinions that don't affect other layers:

```cpp
// Create over spec in modifier layer
bool write_geometry_as_over_spec(
    const Geometry& geometry,
    pxr::UsdStageRefPtr stage,
    const pxr::SdfPath& sdf_path,
    pxr::UsdTimeCode time)
{
    auto root_layer = stage->GetRootLayer();
    auto edit_layer = stage->GetEditTarget().GetLayer();
    
    // Get or create the prim spec in edit layer (not root layer!)
    auto prim_spec = edit_layer->GetPrimAtPath(sdf_path);
    if (!prim_spec) {
        prim_spec = SdfCreatePrimInLayer(edit_layer, sdf_path);
 
    // Set opinions (attributes) in edit layer
    // These will override root layer values but won't modify root layer
 auto mesh = geometry.get_component<MeshComponent>();
    if (mesh) {
        auto points_attr = prim_spec->GetAttribute(TfToken("points"));
        points_attr->SetDefaultValue(VtValue(mesh->vertices));
        // ... set other attributes
 auto prim_spec = edit_layer->GetPrimAtPath(sdf_path);
    if (!prim_spec) {
        prim_spec = SdfCreatePrimInLayer(edit_layer, sdf_path);
    }
 
    // Set opinions (attributes) in edit layer
    // These will override root layer values but won't modify root layer
 auto prim_spec = edit_layer->GetPrimAtPath(sdf_path);
    if (!prim_spec) {
        prim_spec = SdfCreatePrimInLayer(edit_layer, sdf_path);
    }
 
    // Set opinions (attributes) in edit layer
    // These will override root layer values but won't modify root layer
 auto mesh = geometry.get_component<MeshComponent>();
    if (mesh) {
        auto points_attr = prim_spec->GetAttribute(TfToken("points"));
        points_attr->SetDefaultValue(VtValue(mesh->vertices));
        // ... set other attributes
    }
    
    return true;
}
```

### Modifier Storage Options

Modifiers can be stored in three ways:

1. **Prim Attribute (Single Modifier)**:
   ```json
   {
     "modifiers": [
       {
         "name": "Subdivision",
         "order": 0,
         "enabled": true,
         "node_graph_json": "{...}"
       }
     ]
   }
   ```

2. **Child Prims (Multiple Modifiers)**:
   ```
   /geometry/mesh_a               (original)
   /geometry/mesh_a/modifiers/modifier_0  (over spec)
   /geometry/mesh_a/modifiers/modifier_1  (over spec)
   ```

3. **External Layer File (Shared Modifier Layer)**:
   ```
   Stage
   ├── Root Layer (original.usda)
   └── Modifier Layer (modifiers.usda)  ← all modifier outputs go here
   ```

## Benefits

1. **Non-destructive Editing**: Original data preserved, modifiers can be toggled/disabled
2. **Layer Composition**: Full USD composition support (LIVRPS, references)
3. **Modifier Stack**: Multiple modifiers per prim, reorderable
4. **Version Control**: Each modifier can have its own layer
5. **Collaboration**: Different users can work on different layers
