# TreeGen Plugin - Procedural Tree Generation

基于论文 **"Inverse Procedural Modelling of Trees"** (Stava et al., 2014) 的树木过程式建模实现。

## 概述

本插件实现了一个生物学启发的树木生长模型，能够生成多样化的树形结构。实现了论文中描述的前向模型（Forward Model），包括：

- **芽的生长和状态管理**（Bud growth and state management）
- **顶端优势和激素模拟**（Apical dominance and auxin simulation）
- **向光性和向地性**（Phototropism and gravitropism）
- **分支结构生成**（Branching structure generation）

## 核心组件

### 1. 数据结构

#### `TreeParameters.h`
定义所有24个论文参数：

- **几何参数**：控制分支角度、节间长度、叶序等
  - `apical_angle_variance`: 顶端角度变异
  - `num_lateral_buds`: 侧芽数量
  - `branching_angle_mean/variance`: 分支角度均值和方差
  - `roll_angle_mean/variance`: 旋转角度（黄金角137.5°用于叶序）
  - `growth_rate`: 生长速率（每次生长周期的节间数）
  - `internode_base_length`: 节间基础长度
  - `apical_control`: 顶端控制（主干优势）

- **芽命运参数**：控制芽的生长、休眠和死亡
  - `apical_dominance_base/distance/age`: 顶端优势因子
  - `apical/lateral_light_factor`: 光照对芽生长的影响
  - `apical/lateral_bud_death`: 芽的死亡概率

- **环境参数**：环境对树的影响
  - `phototropism`: 向光性强度
  - `gravitropism`: 向地性强度
  - `pruning_factor`: 修剪因子
  - `low_branch_pruning_factor`: 低枝修剪高度

#### `TreeStructure.h`
树的数据结构：

```cpp
struct TreeBud {
    BudType type;           // Apical（顶芽）或 Lateral（侧芽）
    BudState state;         // Active, Dormant, Dead
    glm::vec3 position;
    glm::vec3 direction;
    float illumination;     // 光照强度 [0,1]
    float auxin_level;      // 激素浓度
    int level;              // 分支层级（0=主干）
};

struct TreeBranch {
    glm::vec3 start_position, end_position;
    float length, radius;
    TreeBranch* parent;
    std::vector<TreeBranch*> children;
    std::vector<TreeBud*> lateral_buds;
    TreeBud* apical_bud;
};
```

### 2. 生长算法 (`TreeGrowth.h/cpp`)

实现论文中的生长周期算法：

```
For each growth cycle:
  1. 更新芽状态（死亡、休眠）
  2. 计算光照分布（简化模型）
  3. 计算激素（auxin）浓度
  4. 判断哪些芽会萌发
  5. 从萌发的芽生长新枝条
  6. 更新结构属性（半径、弯曲）
  7. 修剪分支
```

#### 关键方法

**芽萌发概率**（论文公式 2-3）：
```cpp
// 顶芽: P(F) = I^α_apical
float calculate_apical_flush_probability(const TreeBud& bud);

// 侧芽: P(F) = I^α_lateral * exp(-Σ auxin)
float calculate_lateral_flush_probability(const TreeBud& bud);
```

**生长速率**（论文公式 1）：
```cpp
// 基于顶端控制的生长速率调整
float calculate_growth_rate(int branch_level, int tree_age);
```

**向性模拟**：
```cpp
// 向光弯曲
glm::vec3 apply_phototropism(const glm::vec3& direction, float strength);

// 向地弯曲
glm::vec3 apply_gravitropism(const glm::vec3& direction, float strength);
```

## 节点系统

### Geometry Nodes

#### 2. `tree_generate`
主要的树生成节点

**输入**:
- `Growth Years`: 生长年数 (1-50)
- `Random Seed`: 随机种子
- `Apical Angle Variance`: 顶端角度变异
- `Lateral Buds`: 侧芽数量
- `Branch Angle`: 分支角度
- `Growth Rate`: 生长速率
- `Internode Length`: 节间长度
- `Apical Control`: 顶端控制
- `Apical Dominance`: 顶端优势
- `Light Factor`: 光照因子
- `Phototropism`: 向光性
- `Gravitropism`: 向地性

**输出**:
- `Tree Branches`: 树枝曲线集合

#### 3. `tree_to_mesh`
将树枝曲线转换为网格

**输入**:
- `Tree Branches`: 树枝曲线
- `Radial Segments`: 径向细分数

**输出**:
- `Mesh`: 网格几何体

## 使用示例

### 基础用法

```python
# 创建节点图
graph = RuzinoGraph("TreeDemo")
graph.loadConfiguration("TreeGen_geometry_nodes.json")

# 创建树生成节点
tree_gen = graph.createNode("tree_generate", name="MyTree")

# 设置参数
inputs = {
    (tree_gen, "Growth Years"): 15,
    (tree_gen, "Lateral Buds"): 4,
    (tree_gen, "Branch Angle"): 45.0,
    (tree_gen, "Apical Control"): 2.5,
    (tree_gen, "Random Seed"): 42
}

# 执行生成
graph.prepare_and_execute(inputs, required_node=tree_gen)

# 获取结果
tree_curves = graph.getOutput(tree_gen, "Tree Branches")
```

### 转换为网格

```python
# 创建转换节点
to_mesh = graph.createNode("tree_to_mesh", name="TreeMesh")

# 连接并执行
inputs = {
    (to_mesh, "Tree Branches"): tree_curves,
    (to_mesh, "Radial Segments"): 8
}

graph.prepare_and_execute(inputs, required_node=to_mesh)
mesh = graph.getOutput(to_mesh, "Mesh")
```

## 参数调节指南

### 创建不同树形

**高大直立的树（如松树）**:
```
Apical Control: 3.0-4.0
Branch Angle: 30-40°
Growth Rate: 4-6
Apical Dominance: 2.0-3.0
```

**球形树冠（如橡树）**:
```
Apical Control: 1.0-1.5
Branch Angle: 45-60°
Growth Rate: 2-3
Apical Dominance: 0.5-1.0
```

**柳树形态**:
```
Apical Control: 1.5-2.0
Branch Angle: 35-50°
Gravitropism: 0.4-0.6 (下垂)
Growth Rate: 3-5
```

## 算法细节

### 生长周期伪代码

```
function grow_one_cycle(tree):
    // 1. 芽状态更新
    for each bud in tree.all_buds:
        if random() < death_probability:
            bud.state = Dead

    // 2. 光照计算（简化：基于高度）
    max_height = max(bud.position.y for all buds)
    for each bud:
        bud.illumination = 0.3 + 0.7 * (bud.height / max_height)

    // 3. 激素计算
    for each lateral_bud:
        auxin = 0
        for each bud_above:
            distance = ||bud_above.pos - lateral_bud.pos||
            auxin += exp(-distance * α_distance) * α_base
        lateral_bud.auxin = auxin

    // 4. 判断萌发
    for each bud:
        if bud.type == Apical:
            P_flush = bud.illumination^α_light
        else:  // Lateral
            P_flush = bud.illumination^α_light * exp(-bud.auxin)

        if random() > P_flush:
            bud.state = Dormant

    // 5. 生长新枝
    for each active_bud:
        n_internodes = round(growth_rate / apical_control^level)
        for i in 1..n_internodes:
            create_internode()
            create_lateral_buds()
            create_apical_bud()
```

### 叶序（Phyllotaxis）

使用黄金角（137.5°）实现自然的叶序排列：

```cpp
float roll_angle = 137.5° * bud_index + random_variance
```

这创建了类似斐波那契螺旋的排列模式。

## 未来改进

目前实现的是基础的前向模型。可以添加：

1. **更精确的光照模拟**: 使用光线追踪或体素化
2. **叶片生成**: 在末端枝条添加叶片
3. **结构弯曲**: 基于重力的枝条下垂
4. **管道模型**: 更精确的半径计算
5. **环境响应**: 障碍物避让、风的影响
6. **逆向建模**: 从输入树模型反推参数（论文的主要内容）

## 参考文献

O. Stava, S. Pirk, J. Kratt, B. Chen, R. Měch, O. Deussen, and B. Benes,
"Inverse Procedural Modelling of Trees,"
Computer Graphics Forum, vol. 33, no. 6, pp. 118-131, 2014.

## 技术细节

- **语言**: C++17
- **依赖**: GLM (OpenGL Mathematics)
- **数据结构**: 基于指针的树形结构
- **随机数**: Mersenne Twister (`std::mt19937`)
- **几何表示**: Curve Component (可转换为Mesh)

## 编译

确保项目中包含GLM库，然后在主CMakeLists中添加：

```cmake
add_subdirectory(source/Plugins/TreeGen)
```

构建系统会自动扫描`geometry_nodes`目录中的`node_*.cpp`文件并创建相应的节点。
