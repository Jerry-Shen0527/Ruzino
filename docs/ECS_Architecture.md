# ECS 场景管理架构

> **实现状态说明**：ECS 框架本体（components / systems / StageListener 回调）已落地并可用。
> 文中所有 **PhysX 相关内容（PhysicsSystem、SceneQuerySystem、PhysicsComponent）目前都是预留桩**，
> 实现里全是 `// TODO:`，PhysX SDK 尚未真正接入。涉及 PhysX 的接口签名可参考，但不要当作可用功能。
> EnTT 以源码形式内嵌在 `source/Core/rznode/ext/entt`，不是系统级依赖。

## 概述

本项目已经完全重构为使用 EnTT ECS 框架管理场景。新架构提供了更灵活、模块化的方式来处理场景中的实体，同时保持了与现有 USD 工作流的兼容性。StageListener 已完全重构以避免循环依赖，并通过回调机制与 ECS 系统无缝集成。

## 核心架构

### Components (组件)

所有组件定义在 [stage/ecs_components.hpp](source/Runtime/stage/include/stage/ecs_components.hpp):

1. **UsdPrimComponent** - 存储 USD prim 引用和时间状态
2. **AnimationComponent** - 存储节点树和动画执行逻辑
3. **TransformComponent** - 位置、旋转、缩放信息（支持 glm 变换矩阵）
4. **GeometryComponent** - 包装现有的 Geometry 系统
5. **MaterialComponent** - 材质引用
6. **PhysicsComponent** - PhysX 物理属性（**预留接口，未实现**）
7. **DirtyComponent** - 标记需要同步的 entity
8. **Tag Components** - AnimatableTag, RenderableTag, SimulationTag

### Systems (系统)

所有系统定义在 [stage/ecs_systems.hpp](source/Runtime/stage/include/stage/ecs_systems.hpp):

1. **AnimationSystem** - 更新所有动画 entity，执行节点树逻辑
2. **UsdSyncSystem** - 双向同步 ECS 和 USD，支持增量更新
3. **PhysicsSystem** - 物理模拟（**预留 PhysX 集成接口，目前是空桩**）
4. **SceneQuerySystem** - 场景查询（raycast, overlap 等，**预留接口，目前是空桩**）

### StageListener (重构后)

`StageListener` 已完全重构为：
- **无依赖设计**：只依赖 `pxr::UsdStagePtr`，不依赖 `Stage` 类
- **回调机制**：通过 `std::function` 回调通知 Stage 类 USD 变化
- **事件驱动**：监听 USD Notice 并触发相应回调
  - `PrimAddedCallback` - Prim 添加时调用
  - `PrimRemovedCallback` - Prim 删除时调用
  - `PrimChangedCallback` - Prim 属性变化时调用

### Stage 类

`Stage` 类现在持有：
- `entt::registry` - ECS registry
- 所有 systems 的实例
- Entity <-> SdfPath 的双向映射
- `StageListener` 实例
- 保留了原有的 USD 接口以保持向后兼容

## 架构优势

### 1. 解除循环依赖
- StageListener 不再依赖 Stage 完整定义
- 通过回调机制实现松耦合
- Stage 在私有链接中依赖 StageListener

### 2. 更好的性能
- EnTT 提供高性能的 ECS 实现
- 数据局部性优化（所有组件连续存储）
- 支持多线程更新（通过 entt groups）

### 3. 为 PhysX 集成做好准备（尚未实现）
- PhysicsComponent 和 PhysicsSystem 已定义接口（**实现为空桩**）
- SceneQuerySystem 预留 raycast/overlap 接口（**实现为空桩**）
- 变换同步机制已就绪

## 使用示例

### 1. 创建带动画的 Entity

```cpp
// 从 USD prim 创建 entity
auto prim = stage->get_usd_stage()->GetPrimAtPath(pxr::SdfPath("/MyObject"));
auto entity = stage->create_entity_from_prim(prim);

// 添加动画组件
auto& anim = stage->get_registry().emplace<ecs::AnimationComponent>(entity);
anim.node_tree = std::make_shared<NodeTree>(descriptor);
anim.node_tree_executor = create_node_tree_executor(exec_desc);

// 添加标签
stage->get_registry().emplace<ecs::AnimatableTag>(entity);
```

### 2. 添加物理组件（为 PhysX 预留，目前无实际效果）

```cpp
auto& physics = registry.emplace<ecs::PhysicsComponent>(entity);
physics.type = ecs::PhysicsComponent::Type::Dynamic;
physics.mass = 10.0f;

// ⚠️ PhysX 尚未集成，下面这行当前是空桩，不会产生物理效果
stage->get_physics_system()->add_physics_actor(stage->get_registry(), entity);
```

### 3. 查询和遍历

```cpp
// 遍历所有可动画的 entity
auto& registry = stage->get_registry();
auto view = registry.view<ecs::AnimationComponent, ecs::UsdPrimComponent>();
for (auto entity : view) {
    auto& anim = view.get<ecs::AnimationComponent>(entity);
    auto& usd_prim = view.get<ecs::UsdPrimComponent>(entity);
    // 处理...
}

// 查找特定路径的 entity
auto entity = stage->find_entity_by_path(pxr::SdfPath("/MyObject"));
if (entity != entt::null) {
    // 使用 entity...
}
```

### 4. 同步到 USD

```cpp
// 标记 entity 为脏
auto& dirty = registry.emplace<ecs::DirtyComponent>(entity);
dirty.needs_geometry_update = true;
dirty.needs_transform_update = true;

// 同步所有脏 entity 到 USD
stage->sync_entities_to_usd();
```

### 5. 从 USD 加载到 ECS

```cpp
// 加载所有 USD prim 到 ECS
stage->load_prims_to_ecs();
```

## 场景更新流程

每帧的更新顺序：

```cpp
void Stage::tick(float delta_time) {
    // 1. 更新动画系统
    animation_system_->update(registry_, delta_time);

    // 2. 更新物理系统
    physics_system_->update(registry_, delta_time);

    // 3. 同步到 USD（在需要时）
    sync_entities_to_usd();

    // 4. 更新全局时间
    current_time_code += delta_time;
}
```

## StageListener 集成

`StageListener` 现在会：
- 监听 USD 变化
- 自动创建/删除对应的 entity
- 标记变化的 entity 为脏

## 向后兼容

为了保持向后兼容性：
1. 所有原有的 USD 接口都保留
2. 旧的 `animatable_prims` map 仍然存在并工作
3. 可以逐步将现有代码迁移到 ECS 架构

## PhysX 集成准备（尚未实现）

系统已经预留了 PhysX 集成的接口，但 `PhysicsSystem` / `SceneQuerySystem` 的方法体目前全是 `// TODO:` 空桩，PhysX SDK 还没有真正接入。下面的接口签名是设计目标，不代表当前能跑：

```cpp
class PhysicsSystem {
    // 初始化 PhysX SDK
    bool initialize();

    // 更新物理模拟
    void update(entt::registry& registry, float delta_time);

    // 添加/移除物理 actor
    void add_physics_actor(entt::registry& registry, entt::entity entity);
    void remove_physics_actor(entt::registry& registry, entt::entity entity);
};

class SceneQuerySystem {
    // Raycast
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, ...);

    // Overlap test
    bool overlap_sphere(const glm::vec3& center, float radius, ...);
};
```

## 下一步（待办，非现状）

1. **集成 PhysX**（当前未开始，接口已预留）:
   - 在 `PhysicsSystem::initialize()` 中初始化 PhysX SDK
   - 实现 `add_physics_actor()` 创建 PxRigidActor
   - 在 `update()` 中同步 PhysX 变换到 TransformComponent

2. **优化性能**:
   - 使用 entt 的 groups 优化查询
   - 实现多线程更新系统

3. **扩展功能**:
   - 添加更多 Components（Audio, AI, etc.）
   - 实现场景序列化/反序列化

## API 参考

### Stage ECS 接口

```cpp
class Stage {
    // 获取 registry
    entt::registry& get_registry();

    // Entity 创建和查询
    entt::entity create_entity_from_prim(const pxr::UsdPrim& prim);
    pxr::UsdPrim get_prim_from_entity(entt::entity entity);
    entt::entity find_entity_by_path(const pxr::SdfPath& path);

    // 同步
    void sync_entities_to_usd();
    void load_prims_to_ecs();

    // 获取 systems
    ecs::AnimationSystem* get_animation_system();
    ecs::UsdSyncSystem* get_usd_sync_system();
    ecs::PhysicsSystem* get_physics_system();
};
```

## 迁移指南

如果你有现有代码使用旧的 API：

### 旧代码
```cpp
auto prim = stage->create_sphere(path);
// 直接操作 prim...
```

### 新代码（推荐）
```cpp
auto prim = stage->create_sphere(path);
auto entity = stage->create_entity_from_prim(prim);

// 添加组件
auto& transform = registry.emplace<ecs::TransformComponent>(entity);
transform.translation = glm::vec3(0, 1, 0);

// 标记为脏
auto& dirty = registry.emplace<ecs::DirtyComponent>(entity);
dirty.needs_transform_update = true;

// 同步
stage->sync_entities_to_usd();
```

这种方式提供了更好的数据局部性、更容易的多线程支持，以及更清晰的系统边界。
