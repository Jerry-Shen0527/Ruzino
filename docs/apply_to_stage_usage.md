# RuzinoGraph.apply_to_stage() API

## 概述

`apply_to_stage()` 是 RuzinoGraph 类的一个便捷方法，用于将节点图应用到 USD stage 的指定 prim 上。这个方法封装了以下标准流程：

1. **设置 socket 默认值**（如果提供了 inputs）
2. 序列化节点图为 JSON（**包含所有默认值**）
3. 保存到 prim 的 `node_json` 属性
4. 创建 GeomPayload
5. 设置为全局参数

**重要改进**：现在 `inputs` 参数会被设置为 socket 的**默认值**，这意味着它们会被序列化到节点图中，并在 UI 中恢复！

## 新增API：设置 Socket 默认值

### setSocketDefault()

设置单个 socket 的默认值（会被序列化）：

```python
g.setSocketDefault(tree_node, "Growth Years", 5)
g.setSocketDefault("tree_0", "Branch Angle", 45.0)
```

### setSocketDefaults()

批量设置多个 socket 的默认值：

```python
g.setSocketDefaults({
    (tree, "Growth Years"): 5,
    (tree, "Branch Angle"): 45.0,
    (output, "Sub Path"): "geometry",
})
```

**区别说明：**
- `setInput()` / `setInputs()` - 设置临时执行值，**不会**被序列化
- `setSocketDefault()` / `setSocketDefaults()` - 设置默认值，**会被**序列化到节点图JSON

## 使用方法

### 完整示例

```python
from ruzino_graph import RuzinoGraph
from stage_py import Stage
from pxr import UsdGeom

# 1. 创建节点图
g = RuzinoGraph("MyTree")
g.loadConfiguration("geometry_nodes.json")
g.loadConfiguration("Plugins/TreeGen_geometry_nodes.json")

# 2. 创建节点和连接
tree = g.createNode("tree_generate", name="tree")
to_mesh = g.createNode("tree_to_mesh", name="to_mesh")
write = g.createNode("write_usd", name="write")

g.addEdge(tree, "Tree Branches", to_mesh, "Tree Branches")
g.addEdge(tree, "Leaves", to_mesh, "Leaves")
g.addEdge(to_mesh, "Branch Mesh", write, "Geometry")

# 3. 定义输入参数（会被保存到节点图中！）
inputs = {
    (tree, "Growth Years"): 5,
    (tree, "Branch Angle"): 45.0,
    (write, "Sub Path"): "geometry",
}

# 4. 创建 Stage 和 prim
stage = Stage("output.usdc")
UsdGeom.Mesh.Define(stage.get_pxr_stage(), "/my_tree")

# 5. 应用节点图到 prim（inputs 会被保存！）
g.apply_to_stage(stage, "/my_tree", inputs=inputs)

# 6. 执行并保存
g.prepare_and_execute(inputs, required_node=write)
stage.save()
```

## 在 UI 中恢复节点图

当在 UI 中打开 USD 文件时，可以从 prim 的 `node_json` 属性读取并恢复节点图（**包含所有参数**）：

```python
from ruzino_graph import RuzinoGraph
from stage_py import Stage

# 打开 stage
stage = Stage("output.usdc")
pxr_stage = stage.get_pxr_stage()

# 读取节点图 JSON
prim = pxr_stage.GetPrimAtPath("/my_tree")
node_json_attr = prim.GetAttribute("node_json")
node_graph_json = node_json_attr.Get()

# 恢复节点图（包含所有参数值！）
g = RuzinoGraph("MyTree")
g.loadConfiguration("geometry_nodes.json")
g.deserialize(node_graph_json)

# 所有参数都已恢复，可以直接在 UI 中编辑！
# Growth Years = 5
# Branch Angle = 45.0
# Sub Path = "geometry"
```

## 参数说明

```python
def apply_to_stage(
    self,
    stage: Any,
    prim_path: str,
    inputs: Optional[Dict[Tuple[Any, str], Any]] = None
) -> "RuzinoGraph":
    """
    Apply this node graph to a USD stage prim.
    
    Args:
        stage: stage_py.Stage 对象
        prim_path: USD prim 路径 (例如: "/tree", "/geometry")
        inputs: 可选的输入值字典，会被设置为 socket 的默认值并保存
        
    Returns:
        self for method chaining
    """
```

## 注意事项

1. **必须先创建 prim**：调用 `apply_to_stage()` 之前，必须确保 prim 已经存在
2. **自动保存 inputs**：如果提供了 `inputs` 参数，它们会被设置为 socket 默认值并序列化
3. **链式调用**：返回 self，支持链式调用

## 相关方法

- `serialize()` - 序列化节点图为 JSON 字符串（包含默认值）
- `deserialize(json_str)` - 从 JSON 字符串恢复节点图（恢复默认值）
- `setSocketDefault(node, socket_name, value)` - 设置单个 socket 默认值
- `setSocketDefaults(default_values)` - 批量设置 socket 默认值
- `setInput(node, socket_name, value)` - 设置临时执行值（不序列化）
- `setInputs(input_values)` - 批量设置临时执行值（不序列化）
