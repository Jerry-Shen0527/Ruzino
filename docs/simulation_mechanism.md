# Simulation 机制：流程与数据传递

记录 Ruzino 节点图里"仿真"是怎么跑起来的，以及跨帧状态怎么在节点之间传递。
这份文档只讲框架机制本身，不绑定任何具体仿真业务。

参考来源：`simulation_zone.cpp`、`animation.cpp`、`node_exec.hpp`、
`node.hpp`、`geom_payload.hpp`、`node_tree.cpp`。

---

## 1. 触发：谁来驱动仿真

仿真不是节点自己启动的，而是由 **Stage 动画系统**驱动的。
入口：`source/Runtime/stage/source/animation.cpp`。

每帧 `WithDynamicLogicPrim::update_*()` 会被 tick 一次（空格 / play 启动后），
里面做这几件事（`animation.cpp:169-193`）：

```cpp
auto& payload = node_tree_executor->get_global_payload<GeomPayload&>();
payload.delta_time = delta_time;
...
payload.has_simulation = false;
payload.is_simulating = simulation_begun;   // 关键
if (!simulation_begun) {
    simulation_begun = true;
}
node_tree_executor->execute(node_tree.get());
```

几个要点：
- `delta_time` 每帧写进全局 payload，节点通过 `get_global_payload<GeomPayload>()` 读。
- `simulation_begun` 是 `WithDynamicLogicPrim` 的成员，**跨帧持久**。
- **第一帧** execute 时 `simulation_begun` 还是 false → `is_simulating = false`（init 帧）；
  执行完才翻成 true。
- **之后每一帧** `is_simulating = true`。
- 何时重置回 false：图被重载 / modifier stack 变化（`animation.cpp:271`），同时仿真时间归零。

所以仿真生命周期是：

| 帧 | 进入 execute 时 `is_simulating` | 语义 |
|----|--------------------------------|------|
| 0 | false | init 帧：从真实输入初始化 |
| 1,2,3,… | true | 推进帧：用上一帧状态推进 |

---

## 2. 仿真区：`simulation_in` / `simulation_out`

源码：`source/Editor/geometry_nodes/simulation_zone.cpp`。

这两个节点是仿真区的边界，成对出现，中间夹着真正干活的节点。

### 2.1 声明

```cpp
NODE_DECLARATION_FUNCTION(simulation_in) {
    b.add_input_group("Simulation In");
    b.add_output_group("Simulation Out");
}
NODE_DECLARATION_FUNCTION(simulation_out) {
    b.add_input_group("Simulation In");
    b.add_output_group("Simulation Out");
}
```

socket group 是"可变数量输入/输出"的机制：连几根线就有几个 slot，
`get_input_group` / `set_output_group` 以 `vector<entt::meta_any>` 整组存取。

### 2.2 执行

```cpp
// simulation_in
NODE_EXECUTION_FUNCTION(simulation_in) {
    auto& global_payload = params.get_global_payload<GeomPayload&>();
    global_payload.has_simulation = true;

    if (!global_payload.is_simulating) {
        // init 帧：把上游真实输入原样转发进仿真区
        auto inputs = params.get_input_group("Simulation In");
        std::vector<entt::meta_any> outputs;
        for (auto& input : inputs) outputs.push_back((*input));
        params.set_output_group("Simulation Out", outputs);
    } else {
        // 推进帧：丢弃上游输入，回放 storage 里存的上一帧输出
        auto& outputs = params.get_storage<SimulationStorage&>().data;
        params.set_output_group("Simulation Out", outputs);
    }
    return true;
}

// simulation_out
NODE_EXECUTION_FUNCTION(simulation_out) {
    auto inputs = params.get_input_group("Simulation In");
    std::vector<entt::meta_any> outputs;
    for (auto& input : inputs) outputs.push_back((*input));
    params.get_storage<SimulationStorage&>().data = outputs;  // 存
    params.set_output_group("Simulation Out", outputs);
    return true;
}
```

### 2.3 关键语义

- **init 帧**：`simulation_in` 把上游连线上的真实数据转发进区里。这时
  仿真区里的节点拿到的是"初始条件"。
- **推进帧**：`simulation_in` **不再**读上游，而是把它自己 storage 里
  存的、`simulation_out` 上一帧写进来的数据，重新 `set_output_group` 出去。
- `simulation_out` 永远干两件事：把区内的输出转发出去，**同时**存进自己的
  `SimulationStorage::data`，供下一帧的 `simulation_in` 用。

### 2.4 ⚠️ 一个重要的坑：storage 是 per-node 的

`Node::storage`（`node.hpp:61`，`entt::meta_any`）是**每个 Node 实例独有**
的。`simulation_in` 和 `simulation_out` 是两个不同的节点，它们的
`SimulationStorage::data` **互不相通**，框架不会自动把它们同步。

那 `simulation_out` 存的数据，`simulation_in` 下一帧怎么拿到？

按代码字面，推进帧的 `simulation_in` 读的是**它自己 storage**，而不是
`simulation_out` storage 传过来的线。要让这套闭环真正工作，要么靠框架在
帧间做了 storage 拷贝（目前代码里**没有**看到这个拷贝），要么靠
`simulation_out` 的输出线在下一帧物理上连回了 `simulation_in` 的输入。

**实务结论**：不要假设 `simulation_in.storage == simulation_out.storage`。
仿真区内部的节点如果要可靠的跨帧状态，应该用**自己的 `Node::storage`**，
不要依赖 zone 的 `SimulationStorage::data`。

### 2.5 `ALWAYS_DIRTY` 是必须的

```cpp
NODE_DECLARATION_ALWAYS_DIRTY(simulation_in);
NODE_DECLARATION_ALWAYS_DIRTY(simulation_out);
```

因为仿真的真正输入（`delta_time`、`is_simulating`）走的是**全局 payload**，
不是图上的 socket。dirty 传播是基于 socket 连线的，如果不开 ALWAYS_DIRTY，
这两个节点 cook 一次就缓存住了，之后每帧都不会重新执行，仿真直接死掉。
**仿真区内部的所有节点同理，都必须 ALWAYS_DIRTY。**

---

## 3. 跨帧状态：`Node::storage` 机制

这是仿真节点存跨帧数据的主力通道。源码：
`source/Core/rznode/core/include/nodes/core/node_exec.hpp`。

### 3.1 存取 API

```cpp
// node.hpp
struct Node {
    mutable std::string storage_info;   // 序列化字符串
    mutable entt::meta_any storage;     // 运行期对象
    ...
};

// node_exec.hpp
template<typename T>
T get_storage() {
    if (!node_.storage) {
        node_.storage = get_socket_type<T>().construct();   // 懒构造
        if constexpr (std::decay_t<T>::has_storage) {
            if (!node_.storage_info.empty()) {
                node_.storage.cast<T&>().deserialize(node_.storage_info);
            }
        }
    }
    return node_.storage.cast<T>();
}

template<typename T>
void set_storage(T&& value) {
    node_.storage.cast<T&>() = value;
    if constexpr (std::decay_t<T>::has_storage) {
        node_.storage_info = value.serialize();
    }
}
```

### 3.2 跨帧原理

`Node` 实例的生存期 = 节点图的生存期。只要 modifier graph 不被重载，
同一个 `Node` 对象每帧被复用，所以它的 `storage` 字段**天然跨帧**。
仿真节点就是靠这个把上一帧的物理状态带到下一帧。

### 3.3 `has_storage` 标志

```cpp
struct MyStorage {
    static constexpr bool has_storage = true;  // 或 false
    std::string serialize() const;
    void deserialize(const std::string&);
};
```

- `has_storage = false`（默认/常见）：状态只在**进程内**跨帧存活。
  重启程序、重载图 → 状态丢失。绝大多数仿真节点（mass_spring、
  neo_hookean 等）用这个。
- `has_storage = true`：`set_storage` 会额外调 `serialize()` 把状态变成
  字符串存进 `storage_info`，`get_storage` 在懒构造时调 `deserialize()` 还原。

### 3.4 ⚠️ `has_storage = true` 的 JSON 持久化目前是坏的

完整链条应该是：
```
set_storage → serialize() → storage_info
              → Node::serialize() 把 storage_info 写进图 JSON（写盘）
              → 程序重启 → node_tree.cpp 读 JSON 的 "storage_info" 字段
              → get_storage 时 deserialize() 还原
```

但实测：`Node::serialize()`（`source/Core/rznode/core/node.cpp:129-154`）
**根本没有**把 `storage_info` 字段写进图 JSON。grep 全仓 `storage_info`，
写盘方向上没有任何 JSON 写入点（只有 `node_tree.cpp:1377` 一处读盘）。

后果：即便 struct 声明 `has_storage = true`，`set_storage` 把字符串写进
`storage_info` 内存字段了，**重启后这个字段还是空的**，`get_storage`
永远走不到 `deserialize` 分支。状态照样丢。

绕过办法：如果某个节点确实需要跨进程持久化，只能在 `serialize()`/
`deserialize()` 里直接读写自己的文件（固定名或带 Node ID），完全不依赖
`storage_info` 的 JSON 往返。

---

## 4. 三套跨帧/帧内通道对比

| 机制 | 通道 | 跨帧 | 跨进程重启 | 用在哪 |
|------|------|------|-----------|--------|
| **`Node::storage`** | `get_storage`/`set_storage` | ✅（Node 复用） | ❌（除非 has_storage=true 且 JSON 通路修好，目前是坏的） | 仿真节点的主力状态 |
| **`SimulationStorage::data`** | socket group 线 + `simulation_in/out` storage | ✅（设计意图，见 §2.4 的坑） | ❌ | 仿真区边界的数据接力 |
| 全局 payload | `get_global_payload` | ❌（每帧重写） | ❌ | 帧级触发量（dt、is_simulating 等） |

---

## 5. 写一个仿真节点的检查清单

- [ ] storage struct 用 `get_storage<T&>()` / `set_storage(storage)` 存跨帧状态。
- [ ] struct 里 `static constexpr bool has_storage = false;`（除非要做跨进程
      持久化，且愿意自己绕过 §3.4 的坏 JSON 通路）。
- [ ] 节点声明 `NODE_DECLARATION_ALWAYS_DIRTY(name);` —— 仿真真正输入
      在 payload 里，不靠 socket 传播 dirty。
- [ ] init 帧 / 推进帧的分支判断：用 `payload.is_simulating`。init 帧
      （第一帧）做分配/初始化，推进帧跑物理。
- [ ] 跨帧状态走**自己的 `Node::storage`**，不要依赖 zone 的
      `SimulationStorage::data`（见 §2.4）。

---

## 6. 相关文件

| 文件 | 内容 |
|------|------|
| `source/Editor/geometry_nodes/simulation_zone.cpp` | `simulation_in` / `simulation_out` 实现 |
| `source/Core/rznode/core/include/nodes/core/node_exec.hpp` | `get_storage` / `set_storage` / `get_input_group` / `set_output_group` |
| `source/Core/rznode/core/include/nodes/core/node.hpp` | `Node::storage` / `Node::storage_info` 字段定义 |
| `source/Core/rznode/core/node.cpp` | `Node::serialize`（注意：不写 storage_info，见 §3.4） |
| `source/Runtime/stage/source/animation.cpp` | `is_simulating` 翻转、`delta_time` 注入、图重载时重置 |
| `source/Editor/geometry/include/GCore/geom_payload.hpp` | `GeomPayload`（is_simulating / has_simulation / delta_time） |
