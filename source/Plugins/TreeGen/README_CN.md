# TreeGen 树木生成插件文档

## 概述

TreeGen 是一个基于程序化方法的树木生成系统，实现了 Stava et al. 2014 论文 "Inverse Procedural Modelling of Trees" 中描述的树木生长模型。该系统使用发育式生长方法（developmental model），通过模拟芽（bud）的生长和分化来生成逼真的树木结构。

## 理论基础

### 1. 树木生长的生物学原理

树木的生长是一个复杂的生物学过程，主要由以下几个因素控制：

#### 1.1 芽的类型
- **顶芽（Apical Bud）**：位于枝条顶端，向上生长，形成主干或主枝
- **侧芽（Lateral Bud）**：位于枝条侧面，向外生长，形成侧枝

#### 1.2 激素调控（Apical Dominance）
- **生长素（Auxin）**：由顶芽产生，向下运输
- **顶端优势**：生长素抑制侧芽生长，使顶芽优先发育
- **距离衰减**：生长素浓度随距离呈指数衰减

#### 1.3 环境因素
- **向光性（Phototropism）**：枝条向光源方向生长
- **向地性（Gravitropism）**：枝条受重力影响，主干向上，侧枝下垂
- **光照竞争**：上层枝叶遮挡下层，影响芽的萌发

### 2. 程序化生长模型

#### 2.1 生长周期（Growth Cycle）

每个生长周期包含以下步骤：

```
1. 更新芽状态（死亡、休眠）
2. 计算光照强度
3. 计算生长素水平
4. 决定芽的萌发
5. 从萌发的芽生长新枝条
6. 更新枝条半径（管道模型）
7. 应用结构弯曲
8. 修剪枝条
```

#### 2.2 芽的萌发概率

**顶芽萌发概率**：
```
P_apical = I^α
```
其中：
- I：光照强度（0-1）
- α：光照因子（apical_light_factor）

**侧芽萌发概率**：
```
P_lateral = I^β × exp(-A)
```
其中：
- I：光照强度
- β：侧芽光照因子（lateral_light_factor）
- A：生长素浓度

#### 2.3 生长素浓度计算

对于每个侧芽，其生长素浓度由所有位于其上方的芽贡献：

```
A = Σ D_base × exp(-d × D_dist) × D_age^t
```

其中：
- D_base：基础顶端优势（apical_dominance_base）
- d：芽间距离
- D_dist：距离衰减因子（apical_dominance_distance）
- D_age：年龄因子（apical_dominance_age）
- t：树龄

#### 2.4 节间长度计算

节间（internode）长度随树龄递减：

```
L = L_base × L_age^t
```

其中：
- L_base：基础节间长度（internode_base_length）
- L_age：长度衰减因子（internode_length_age_factor）
- t：树龄

#### 2.5 生长速率

每次萌发产生的节间数量受顶端控制：

```
N = G_rate / C^level
```

其中：
- G_rate：基础生长速率（growth_rate）
- C：顶端控制因子（apical_control）
- level：分支层级（0=主干）

#### 2.6 分支角度和叶序

**分支角度（Branching Angle）**：
- 从正态分布采样：N(mean, variance)
- mean：branching_angle_mean（默认45°）
- variance：branching_angle_variance（默认10°）

**旋转角度（Roll Angle / Phyllotaxis）**：
- 使用黄金角（137.5°）实现螺旋排列
- 第 i 个芽的旋转角：137.5° × i
- 这种排列最大化了空间利用和光照接收

#### 2.7 管道模型（Pipe Model）

枝条半径根据子枝条递归计算：

```
R_parent = √(Σ R_child²)
```

这基于管道模型理论：父枝的横截面积等于所有子枝横截面积之和。

### 3. 叶子生成

TreeGen实现了基于论文和生物学原理的叶子生成系统，参考了Livny et al. [LPC*11]的方法。

#### 3.1 叶子分布策略

**终端分支优先**：
- 叶子主要生成在终端分支（末梢枝条）上
- 通过 `leaves_on_terminal_only` 参数控制
- `leaf_terminal_levels` 定义了从树梢开始的几层分支会生成叶子
- 这符合真实树木的生长模式：叶子集中在树冠外围

**位置分布**：
- 叶子沿节间均匀分布，但添加了随机扰动
- 数量由 `leaves_per_internode` 控制
- 避免在枝条的起点和终点生成（保留0.1-0.9的范围）
- 位置添加10%的随机变化，使分布更自然

#### 3.2 叶序排列（Phyllotaxis）

使用黄金角（137.5°）实现螺旋排列：
- 第 i 个叶子的旋转角度：137.5° × i
- 这种排列在自然界中广泛存在
- 最大化空间利用和光照接收
- 避免上层叶子完全遮挡下层

#### 3.3 叶子朝向

**法向量计算**：
```
1. 基础方向：从枝条径向指出（perpendicular to branch）
2. 应用黄金角旋转：实现螺旋排列
3. 添加向光性：向光源方向弯曲
4. 添加向上偏置：避免叶片朝下
```

**向光性（Phototropism）**：
- 受 `leaf_phototropism` 参数控制（默认0.5）
- 叶片会向光源方向弯曲
- 模拟植物叶片的向光生长

**向上偏置**：
- 确保叶片不会指向地面
- 如果法向与上方夹角小于0.2，添加额外的向上分量
- 大多数植物的叶片都倾向于面向上方

#### 3.4 叶子姿态

**倾斜角（Inclination）**：
- 从水平面的角度（0°=水平，90°=垂直）
- 基础角度由 `leaf_inclination_mean` 控制（默认45°）
- 下层叶片更水平（接收更多光照）
- 上层叶片可以更倾斜
- 公式：`inclination = base + (-20° × (1 - height_factor))`

**旋转角（Rotation）**：
- 围绕法向的额外旋转
- 添加随机变化 `leaf_rotation_variance`（默认±30°）
- 使叶片排列更加自然

**弯曲度（Curvature）**：
- 模拟叶片的弯曲形态
- `leaf_curvature` 参数控制（0=平展，1=弯曲）
- 添加随机变化（±0.1）

#### 3.5 叶子尺寸

**基础尺寸**：
```
size = leaf_size_base × level_factor × terminal_bonus
```

**层级衰减**：
- 每增加一个分支层级，尺寸 × 0.85
- 从 `min_leaf_level` 开始计算
- 高层枝条的叶子更小

**终端奖励**：
- 终端分支的叶子增大30%（× 1.3）
- 模拟树木将资源集中在末梢的特性

**长宽比**：
- `leaf_aspect_ratio` 定义长宽比（默认2.0）
- length = size × aspect_ratio
- width = size
- 可模拟不同形状的叶片（椭圆、长条等）

**随机变化**：
- 每片叶子的尺寸有随机扰动
- 由 `leaf_size_variance` 控制（默认±0.03）
- 最小尺寸限制为0.01

#### 3.6 叶子坐标系

每片叶子有完整的局部坐标系：
- **Normal**：法向量，指向叶片表面
- **Tangent**：切向量，沿叶片长度方向
- **Binormal**：副法向量，沿叶片宽度方向

坐标系构建：
```
1. Normal: 通过phyllotaxis和phototropism计算
2. Tangent: 枝条方向投影到垂直于Normal的平面
3. Binormal: Normal × Tangent
4. 重新正交化Tangent: Binormal × Normal
```

这个右手坐标系确保：
- 叶片几何正确定向
- 纹理坐标正确映射
- 光照计算准确

#### 3.7 与论文的对应

**Stava et al. 2014，第4.2节（Foliage）**：
- ✅ 论文："叶子位于终端分支上"
  - 实现：`leaves_on_terminal_only` 和 `is_terminal()` 检查

- ✅ 论文："使用Livny et al.的程序化系统"
  - 实现：改进的叶序和朝向计算

- ✅ 论文："增加树冠密度，特别是扫描模型"
  - 实现：终端分支叶子尺寸奖励机制

- ✅ 论文："在生长周期结束时生成叶子"
  - 实现：在 `grow_one_cycle` 最后调用 `create_leaves`

## 代码结构

### 参数详解

#### 叶子参数组

| 参数名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `generate_leaves` | bool | true | 是否生成叶子 |
| `leaves_on_terminal_only` | bool | true | 仅在终端分支生成叶子 |
| `leaf_terminal_levels` | int | 3 | 定义终端分支的层级深度 |
| `leaves_per_internode` | int | 4 | 每个节间的叶子数量 |
| `leaf_size_base` | float | 0.15 | 叶子基础尺寸 |
| `leaf_size_variance` | float | 0.03 | 叶子尺寸随机变化 |
| `leaf_aspect_ratio` | float | 2.0 | 长宽比（length/width） |
| `min_leaf_level` | int | 1 | 最小生成叶子的分支层级 |
| `leaf_rotation_variance` | float | 30° | 围绕法向的旋转变化 |
| `leaf_phyllotaxis_angle` | float | 137.5° | 黄金角，叶序螺旋排列 |
| `leaf_inclination_mean` | float | 45° | 平均倾斜角（从水平） |
| `leaf_inclination_variance` | float | 15° | 倾斜角随机变化 |
| `leaf_curvature` | float | 0.2 | 叶片弯曲程度（0-1） |
| `leaf_phototropism` | float | 0.5 | 叶片向光性强度 |

**使用建议**：
- 针叶树：`aspect_ratio=5.0`, `size=0.05`, `curvature=0.0`
- 阔叶树：`aspect_ratio=1.5-2.5`, `size=0.15-0.25`, `curvature=0.2-0.4`
- 柳树：`aspect_ratio=4.0`, `inclination_mean=60°`, `curvature=0.3`
- 橡树：`aspect_ratio=1.8`, `inclination_mean=40°`, `leaves_per_internode=5`

### 核心类

#### TreeParameters
定义所有生长参数：
- 几何参数（角度、长度、生长速率）
- 芽命运参数（萌发概率、死亡率）
- 环境参数（光照、重力）
- 叶子参数（数量、大小）

#### TreeStructure
树木的数据结构：
- `TreeBranch`：枝条（节间）
  - 起点、终点、方向、长度、半径
  - 父枝条、子枝条、侧芽、叶子
- `TreeBud`：芽
  - 类型（顶芽/侧芽）、状态（活跃/休眠/死亡）
  - 位置、方向、光照、生长素浓度
- `TreeLeaf`：叶子
  - 位置、法向、切向
  - 大小、旋转

#### TreeGrowth
生长系统的主要逻辑：
- `initialize_tree()`：初始化主干
- `grow_one_cycle()`：执行一个生长周期
- `grow_tree()`：执行多个生长周期

### 几何节点

#### tree_generate
主要的树木生成节点：
- 输入：生长参数（年份、角度、光照等）
- 输出：树枝的曲线几何（Curve Geometry）
- 将树木结构转换为可视化的曲线

#### tree_to_mesh
将曲线转换为网格：
- 输入：树枝曲线、径向分段数
- 输出：圆柱形网格
- 为每个枝条创建带半径变化的圆柱体

#### tree_to_leaves
生成叶子几何：
- 输入：树木结构、叶子尺寸
- 输出：叶子网格
- 创建简单的菱形叶片模板

## 使用方法

### 基本使用

```python
from ruzino_graph import RuzinoGraph

# 创建图
g = RuzinoGraph("TreeTest")

# 加载TreeGen节点
g.loadConfiguration("Plugins/TreeGen_geometry_nodes.json")

# 创建节点
tree_gen = g.createNode("tree_generate", name="tree")
to_mesh = g.createNode("tree_to_mesh", name="mesh")
write_usd = g.createNode("write_usd", name="writer")

# 连接节点
g.addEdge(tree_gen, "Tree Branches", to_mesh, "Tree Branches")
g.addEdge(to_mesh, "Mesh", write_usd, "Geometry")

# 设置参数
inputs = {
    (tree_gen, "Growth Years"): 5,
    (tree_gen, "Branch Angle"): 45.0,
    (tree_gen, "Generate Leaves"): True,
    (to_mesh, "Radial Segments"): 8,
}

# 执行
g.prepare_and_execute(inputs, required_node=write_usd)
```

### 参数调节指南

#### 树形控制

**主干粗壮、分支少**：
- 增大 `Apical Control`（2.0 → 4.0）
- 增大 `Apical Dominance`（1.0 → 2.0）
- 减少 `Lateral Buds`（4 → 2）

**分支繁茂、灌木状**：
- 减小 `Apical Control`（2.0 → 1.0）
- 减小 `Apical Dominance`（1.0 → 0.5）
- 增加 `Lateral Buds`（4 → 6）

#### 分支角度

**直立树形（如松树）**：
- 减小 `Branch Angle`（45° → 25°）
- 增大 `Gravitropism`（0.2 → 0.4）

**开展树形（如橡树）**：
- 增大 `Branch Angle`（45° → 65°）
- 减小 `Gravitropism`（0.2 → 0.1）

#### 树木密度

**稀疏**：
- 减小 `Growth Rate`（3.0 → 2.0）
- 减小 `Internode Length`（0.3 → 0.5）

**密集**：
- 增大 `Growth Rate`（3.0 → 5.0）
- 减小 `Internode Length`（0.3 → 0.15）

#### 叶子调节

**浓密树冠**：
- 开启 `Leaves On Terminal Only` = True
- 增加 `Leaves Per Internode`（4 → 6）
- 增大 `Leaf Size`（0.15 → 0.25）
- 增大 `Leaf Terminal Levels`（3 → 4）

**稀疏树冠**：
- 减少 `Leaves Per Internode`（4 → 2）
- 减小 `Leaf Size`（0.15 → 0.08）
- 减小 `Leaf Terminal Levels`（3 → 2）

**更向光的叶片**：
- 增大 `Leaf Phototropism`（0.5 → 0.8）
- 减小 `Leaf Inclination Mean`（45° → 30°）

**更自然的叶片**：
- 增大 `Leaf Rotation Variance`（30° → 45°）
- 增大 `Leaf Curvature`（0.2 → 0.4）
- 增大 `Leaf Size Variance`（0.03 → 0.05）

**不同叶型**：
- 宽叶：`Leaf Aspect Ratio` = 1.5（接近圆形）
- 窄叶：`Leaf Aspect Ratio` = 3.0（长条形）
- 针叶：`Leaf Aspect Ratio` = 5.0 + 减小 `Leaf Size`

## 实现细节与论文对照

### 已实现的特性

✅ **芽的生长模型**
- 顶芽和侧芽的区分
- 芽的萌发概率计算（基于光照和生长素）
- 芽的死亡和休眠状态

✅ **顶端优势（Apical Dominance）**
- 生长素的产生和传播
- 距离衰减和年龄因子
- 对侧芽萌发的抑制效果

✅ **光照模型**
- 简化的基于高度的光照计算
- 光照对萌发概率的影响

✅ **向性生长（Tropisms）**
- 向光性（phototropism）
- 向地性（gravitropism）

✅ **几何参数**
- 分支角度的正态分布
- 黄金角叶序（phyllotaxis）
- 节间长度的年龄衰减

✅ **结构模型**
- 管道模型（pipe model）计算半径
- 分支层级系统

✅ **叶子生成**
- 基于叶序的螺旋排列（黄金角137.5°）
- 尺寸随层级衰减，终端分支奖励
- 向外向上的朝向，带向光性
- 完整的局部坐标系（法向、切向、副法向）
- 倾斜角随高度变化
- 长宽比控制叶片形状
- 弯曲度模拟
- 终端分支优先策略

### 简化的特性

⚠️ **光照计算**
- 论文：完整的阴影投射和遮挡计算
- 当前：基于高度的简化模型

⚠️ **结构弯曲**
- 论文：基于重量的物理弯曲模拟
- 当前：占位符实现（未完全实现）

⚠️ **修剪**
- 论文：基于光照竞争的动态修剪
- 当前：简化的基于高度和光照的标记

### 修正的问题

🔧 **分支创建逻辑**
- 原问题：简化的父节点查找不正确
- 修正：正确查找 shared_ptr 父节点

🔧 **叶序角度**
- 原问题：roll angle 基于索引而非累积
- 修正：使用累积的黄金角（137.5° × i）

🔧 **生长方向归一化**
- 原问题：某些向量叉积未归一化
- 修正：确保所有方向向量归一化

## 测试

TreeGen插件包含多个测试用例来验证功能：

### 运行测试

```bash
cd source/Plugins/TreeGen/tests
pytest -v
```

### 测试文件说明

1. **test_treegen.py** - 基础功能测试
   - 树木生成
   - 参数变化
   - 叶子系统基础功能

2. **test_leaf_system.py** - 叶子系统专项测试（新增）
   - 终端分支叶子生成验证
   - 叶子参数变化测试（窄叶、宽叶、弯曲、向光性）
   - 叶子密度对比测试

3. **test_full_tree.py** - 完整流程测试
   - 生成 → 转网格 → 导出USD

4. **test_tree_export.py** - USD导出测试
   - 完整树木导出为USD格式
   - 可用于Houdini/usdview查看

5. **test_tree_grid.py** - 批量生成测试
   - 5×5×5 = 125棵树的参数网格
   - X轴：生长年份
   - Y轴：分支角度
   - Z轴：顶端控制

### 叶子系统测试用例

新的 `test_leaf_system.py` 包含以下测试：

**测试1：终端叶子模式**
```python
# 仅在末端2层分支生成叶子
"Terminal Leaves Only": True
"Leaf Terminal Levels": 2
"Leaves Per Internode": 5
```

**测试2：叶子形态变化**
- 窄叶（柳树型）：`Aspect Ratio: 4.0, Inclination: 60°`
- 宽叶（橡树型）：`Aspect Ratio: 1.5, Inclination: 30°`
- 弯曲叶：`Curvature: 0.6, Aspect Ratio: 2.5`
- 向光叶：`Phototropism: 0.9, Inclination: 20°`

**测试3：密度对比**
- 稀疏：2叶/节间，2层终端
- 中等：4叶/节间，3层终端
- 密集：6叶/节间，4层终端

输出文件位于 `Binaries/Debug/` 目录，可直接在USD查看器中打开。

## 扩展建议

### 短期改进

1. **完整的光照计算**
   - 实现射线投射检测遮挡
   - 考虑天空光和直射光
   - 添加自遮挡计算

2. **物理弯曲**
   - 计算枝条重量
   - 模拟重力导致的下垂
   - 添加风力影响

3. **更丰富的叶子**
   - 多种叶片形状（椭圆、掌状、羽状）
   - 叶片簇（compound leaves）
   - 季节变化（颜色、脱落）

4. **纹理坐标**
   - 为树干和树枝生成 UV
   - 支持树皮纹理映射

### 长期扩展

1. **逆向建模**
   - 实现论文中的 MCMC 优化
   - 从输入网格反推参数
   - 参数自动调优

2. **环境交互**
   - 避障生长
   - 多棵树的竞争
   - 地形适应

3. **LOD 系统**
   - 多层次细节生成
   - 距离-based 简化
   - 实例化优化

4. **动画**
   - 生长动画
   - 风力摆动
   - 季节变化

## 参考文献

Stava, O., Pirk, S., Kratt, J., Chen, B., Měch, R., Deussen, O., & Benes, B. (2014). Inverse Procedural Modelling of Trees. *Computer Graphics Forum*, 33(6), 118-131.

## 许可

本插件遵循项目主仓库的许可证。

---

**作者**：TreeGen Plugin Team
**版本**：1.0
**最后更新**：2024
