# 材质管线（MaterialX → Slang generator → 路径追踪）

本文档记录 Ruzino 的材质系统架构、MaterialX Slang generator 的工作方式、
以及 2026-08 的一次重构（P0：删 generator 字符串手术 + 主循环 opacity 穿透）。

## 架构总览

材质从 USDA/mtlx 到最终渲染经过四个阶段：

```
USD Material prim
    │  (HdMaterialNetwork2, FetchNetInterface)
    ▼
MaterialX 文档 (HdMtlxCreateMtlxDocumentFromHdNetworkFast)
    │  (mx::SlangShaderGenerator::generate)
    ▼
Slang fetch_shader_data / fetch_shader_opacity callable
    │  (materialX.cpp 拼 BindlessContext 数据代码)
    ▼
[shader("callable")] fetch_<material> + eval_<standard|preview|fallback>
    │  (path_tracing.slang 主循环: fetch → opacity test → eval)
    ▼
屏幕像素
```

### 三类材质 dispatch 路径

generator 对每种材质生成一个 `fetch_shader_data`，运行时设
`shader_type_id`，路径追踪器用它 dispatch 到对应的 eval callable：

| 材质类型 | `shader_type_id` | eval callable | 触发条件 |
|---|---|---|---|
| `standard_surface` | 0 | `eval_standard_surface.slang` | `info:id = ND_standard_surface_surfaceshader` |
| `UsdPreviewSurface` | 1 | `eval_preview_surface.slang` | `info:id = UsdPreviewSurface` |
| fallback | 2 | `eval_fallback.slang` | Material 无 surface terminal |
| 自定义 slang | 3+ | （按 `config:shader_path`，注：当前 renderer 未 forward `config:` 命名空间，见已知问题） | `MaterialXConfigAPI` + `outputs:mtlx:surface` |

特化逻辑在 `ClosureCompoundNodeSlang.cpp`：对 `standard_surface` /
`UsdPreviewSurface`，generator emit 参数打包代码（`packStandardSurfaceMaterialParams`
或 `PreviewSurfaceMaterialParams`），其余材质走通用 closure emit。

### Bindless binding（自定义层）

项目的 binding 模型是 **bindless**，与官方 MaterialX 1.39.5 的 `cbuffer + slang-rhi`
完全不同，必须保留：

- `BindlessContext`（`bindlessContext.cpp`）在**生成期**把 uniform **值**序列化进
  `MaterialDataBlob`（字节 blob），记录偏移映射；
- 贴图预留 uint 槽存纹理 ID，emit `Texture2D = t_BindlessTextures[asuint(data.data[loc])]`；
- 生成的 shader 入口是 callable 风格 `fetch_shader_data(...)`；
- 运行时后端是 **nvrhi**，不是 slang-rhi。

`SlangResourceBindingContext`（register 风格）是死代码，生产中不实例化。

## 2026-08 重构记录

### P0：generator 直接生成 opacity 函数（删除字符串手术）

**动机**：`materialX.cpp::ensure_shader_ready` 里有 ~280 行字符串手术——
从生成的 `fetch_shader_data` 函数体里**正则抠出**纹理采样代码和 opacity 表达式，
塞进 `fetch_shader_opacity` 的占位符（`$TextureSamplingForOpacity` /
`$OpacityComputation`）。其中 opacity 用了魔数 38（数到
`packStandardSurfaceMaterialParams` 的第 39 个参数）、靠"空行+`;`"定位代码块边界，
极其脆弱，任何 generator 输出格式变动都是地雷。

**改动**（3 commits）：

1. `ClosureCompoundNodeSlang.cpp` — 重写 `emitOpacityFetchFunctionDefinition`：
   统一按 opacity 输入名 + 类型提取（color3 做 luminance，float 直接用），
   靠 `emitInput` 递归 emit 上游节点（含纹理）。`emitFunctionDefinition`
   改为调用它，不再生成占位符壳。
2. `materialX.cpp` — 删除 282 行字符串手术，只保留 `$BindlessDataLoading`
   替换（bindless 数据加载仍来自 BindlessContext）。
3. `path_tracing.slang` — 主循环加 opacity presence 穿透决策（见下）。

净减 279 行。渲染结果与重构前一致（standard_surface mean 0.6584 → 0.6581）。

### transmission BSDF bug 修复

`mx_dielectric_bsdf.slang` 的 early-return（`:23-26`）没设 throughput，
保留了 BSDF 默认的 `throughput=1.0`。`NG_standard_surface` 反射块里的
transmission dielectric 调用命中此 early-return，污染了下游 `mx_mix_bsdf`，
导致玻璃球能量/噪点异常。修复：early-return 时显式置
`bsdf.throughput = 0.0; bsdf.response = float3(0.0)`。

### 主循环 opacity 穿透决策

`path_tracing.slang` 主循环在调 eval callable 之前，先 fetch opacity，
按 `random_float(seed) > opacity` 概率让光线**直通穿过表面**（沿原方向
继续追踪，看背后几何）。这是 path tracing 里 opacity<1 的标准
cutout/presence 处理。

修复前：`UsdPreviewSurface` 的 `opacity=0.4` 视觉上完全不透明
（opacity 只在 shadow ray 用了 `< 0.25` 二值判断，主 radiance 路径完全不处理）。
修复后：黄板能透出背后的棋盘格。

### BSDF 内 opacity 双重衰减清理（2026-08-03）

主循环直通（上面那段）接通后，BSDF 内部仍残留两处 opacity 处理，与主循环
重复衰减能量：硬截断（`opacity<0.25 → return transparent`）和末尾
`color *= surfaceOpacity`。这次清理掉这两处（UsdPreviewSurface +
StandardSurface 各删 3 点：eval 硬截断、末尾乘法、sample 早返回），让
opacity 的几何存在性判定**只发生在主循环**，BSDF eval 一旦被调到就按完整
表面求值。

**关键教训**：原 TODO-1 建议把 `transmission_mix = (threshold>0)?1:opacity`
也删掉是**错的**——那是 PreviewSurface 的折射模型（无 transmission 参数，
opacity 兼任 dielectric mix 权重），不是 presence 衰减。首版照做导致
opacity=1 的材质也崩溃（mix 从 1.0 → 0.0，折射层消失，transmission 场景
mean 0.647 → 0.347）。区分"几何存在性（presence）"和"表面折射模型"是关键。

### shadow ray opacity 概率化（2026-08-03）

主循环直通是概率性的（`random > opacity`），但 shadow ray 一直是**硬二值
截断** `if (opacity < 0.25)`——opacity 在 [0.25, 1.0] 的半透明表面会投射
**完全不透明的硬阴影**，与 radiance 侧的软透射矛盾。半透明黄板（opacity=0.4）
原本投的是和实心板一样的深阴影。

**修复**：把 4 个 shadow hit shader（`path_tracing.slang` + `wetbrush_render.slang`
的 `ShadowHit` + `SphereShadowHit`）的 `if (opacity < 0.25)` 改成与主循环
对称的概率测试 `if (random_float(payload.seed) >= opacity)`。opacity=0.4 的
表面有 40% 概率挡住 shadow ray → 投射淡阴影。基础设施本就齐备（opacity
callable 返回连续 float、payload 带 seed 且在 continuation ray 间传递、
`MAX_SHADOW_OPACITY=4` 递归上限），只改判定逻辑。

**顺带修的 bug**：triangle shadow 里原有 `float opacity_threshold =
random_float(payload.seed)` 算了随机数却**从没用在比较里**，白白消耗 seed
污染下游 RNG 序列；sphere shadow 连这行都没有。概率化后这个随机数真正
参与判定，dead code 也清掉了。

**边界修复**：原代码 `opacity<0.25` 且剩余距离 ≤0.001 时会落到块外执行
`isVisible=false`（透明表面却挡光，逻辑矛盾）。新版此时正确返回
`isVisible=true`（距离太短说明已接近光源，无遮挡）。

**验证**（5 测试全 PASSED，`conservative` vs `shadow_fix` 像素级对比）：
- opacity=1 材质**精确零回归**：transmission 全图 delta ±0.0000。
- preview_surface 半透明黄板（opacity=0.4）阴影区变亮 +0.06~0.07（4×4
  网格右上象限），正是阴影投射位置。
- standard_surface 绿板（opacity=0.6）变化被噪声淹没（~-0.002~0.009）。

### 染色 opacity（color3 presence 染色直通，2026-08-03）

`standard_surface.opacity` 本身是 `color3`（三通道），但 opacity fetch
callable 把它 **luminance 降级成标量** 塞进 `material_params_index`，导致主
循环直通时透过光**不带颜色**（红滤光片透出的光是白的）。这次让 fetch 同时
返回 color3，主循环穿过时对 throughput 做无偏染色。

**改动**（7 文件）：
1. `callable_data.slang` + `material.cpp`：`FetchCallableData` 加
   `float3 opacityColor` 字段（CallShader payload，双向同步）。
2. `ClosureCompoundNodeSlang.cpp::emitOpacityFetchFunctionDefinition`：签名
   加 `inout float3 opacityColor`；color3 分支传原色，float 分支标量化，
   默认 `(1,1,1)`。
3. `material.cpp`：fallback `fetch_shader_opacity` + `$getOpacity` wrapper
   更新。
4. `path_tracing.cpp` + `wetbrush_render.cpp`：custom shader 的
   `fetch_<name>_opacity` wrapper 设 `opacityColor = (1,1,1)`。
5. `path_tracing.slang` 主循环：直通分支从 `throughput 不变` 改成
   `throughput *= (1 - opacityColor) / (1 - surviveProb)`（无偏）。
6. shadow hits（path_tracing + wetbrush 各 2 处）：初始化 `opacityColor`
   字段（shadow 本身不染色，只读标量 opacity 做概率测试）。

**无偏性**：生存概率 P = luminance(opacityColor)（标量），穿透时 throughput
乘 `(1 - opacityColor) / (1 - P)`。分母补偿概率，保证每通道期望无偏。对纯
标量 opacity（UsdPreviewSurface），opacityColor=(v,v,v)，P=v，
`(1-v)/(1-v)=1`，throughput 不变 → 和旧行为一致（零回归）。

**验证**：
- 5 个现有测试全 PASSED，transmission 球 mean=0.6470 精确回归。
- 新场景 `mat_tinted_opacity.usda`：红滤光片 opacity=(0.1,1,1)（R 透明、
  G/B 挡）透过光 RGB=(0.900,0.829,0.830) R>>G,B ✓ 染色生效；灰对照
  opacity=(0.3,0.3,0.3) RGB=(0.748,0.748,0.748) 无染色 ✓。

**opacity color3 语义提醒**：`opacity=(0.1, 1, 1)` 表示 **R 通道半透明**
（红光透过）、G/B 通道不透明（挡住），所以是**透红滤光片**，不是"红色板"。
"红色半透明板"（板本身是红色 + 半透明）需要用 `base_color` 设红色 +
`opacity` 设标量。

### 三种"透明"机制对比（当前实现状态）

| 机制 | 适用 | 方向 | 染色 | 折射 | 状态 |
|---|---|---|---|---|---|
| **opacity（presence）** | 树叶/铁丝网/cutout | 主循环直通 | ✓（color3，本次加） | ✗ | ✅ 完成 |
| **transmission（折射）** | 玻璃球/水（封闭体） | BSDF Snell 折射 | ✓（transmission_color） | ✓ | ✅ 完成（球场景验证） |
| **thin_walled transmission（薄壁）** | 薄片/玻璃纸（单面开放） | 应直通不折射 | ✓ | ✗ | ⚠️ WIP（sample/eval 方向问题未解） |

## 材质测试基线

`source/tests/test_material_coverage.py` + `source/tests/data/scenes/materials/`：
5 个渲染测试覆盖三类材质路径 + transmission 折射 + opacity 半透明：

| 测试 | 场景 | 验证 |
|---|---|---|
| `test_material_fallback` | 单块板无材质 | fallback 路径 (shader_type_id=2) |
| `test_material_standard_surface` | 红/金属金/半透明绿 + 棋盘格 | standard_surface + transmission |
| `test_material_preview_surface` | 红/蓝金属/半透明黄 + 棋盘格 | UsdPreviewSurface + opacity 半透明 |
| `test_material_mixed` | fallback/SS/PS 同框 | dispatch 路由共存 |
| `test_material_transmission` | 三玻璃球 + 8x6 棋盘格 | dielectric 折射 + eta_flipped |

**运行方式**（HD renderer plugin 是进程级单例，测试需单独跑或接受多测试时的单例污染）：

```bash
cd Binaries/Release
python -m pytest ../../source/tests/test_material_coverage.py -v
# 单个测试（推荐，避免单例问题）：
python -m pytest ../../source/tests/test_material_coverage.py::test_material_transmission -v
```

输出 PNG 在 `Binaries/Release/test_output/material_coverage/`。

**Python 环境**：项目用 `SDK/python/python.exe`（3.13），但需要 pytest/numpy/PIL，
用 `C:\Users\Jerry\scoop\apps\python313\current\python.exe`（装了这些包，能 import hd_RUZINO_py）。

## 已知问题与 TODO

### TODO-1：opacity BSDF 内双重衰减（已完成 ✅，2026-08-03）

**原描述**（已过时，见下方"修正"）：`UsdPreviewSurface.slang:138-141` 用 opacity
做 diffuse↔dielectric mix 权重，与主循环 opacity presence 穿透语义重叠。

**修正后的正确判断**：原 TODO 把 opacity→transmission_mix 映射也当成"重叠"是
**错的**。实际上 BSDF 内部有**三类** opacity 处理点，只有两类是真重叠：

| 处理点 | 性质 | 是否删 |
|---|---|---|
| 硬截断 `if(opacity<0.25) return transparent` | 与主循环直通**真重叠** | ✅ 删 |
| 末尾 `color *= surfaceOpacity` | 与主循环直通**真重叠**（双重扣减能量） | ✅ 删 |
| `transmission_mix = (threshold>0)?1:opacity` | **PreviewSurface 折射模型本身**（无 transmission 参数，opacity 兼任） | ❌ 保留 |

把第三类也删掉（首版尝试）会让 opacity=1 的不透明材质也崩溃——因为它把
`transmission_mix` 从 1.0（有 dielectric 折射层）变成 0.0（纯 diffuse），
彻底移除了 PreviewSurface 唯一的透射能力。实测 transmission 测试场景 mean
0.6470 → 0.3472（棋盘格背景 CheckLight/CheckDark 是 UsdPreviewSurface）。

**最终改动**（2 文件，净减 8 行）：
- `UsdPreviewSurface.slang`：删 eval 硬截断 + 末尾 `color *= surfaceOpacity` +
  sample 早返回；**保留** `transmission_mix` 映射。
- `StandardSurface.slang`：同样删硬截断 + 末尾乘法 + sample 早返回（它本来
  就没有 opacity→mix 映射，用 `transmission` 参数）。

**验证**（5 个材质测试全 PASSED，与改动前 baseline 像素级对比）：
- opacity=1 材质**零回归**：fallback 0.0% 像素变化、transmission 1.5%、
  mixed 2.0%（后两者是蒙特卡洛噪声边界）。
- opacity<1 材质**集中变化**：preview_surface 30.7% 像素变化
  （黄板 opacity=0.4 区域变亮，去掉双重扣减）、standard_surface 7.0%
  （绿板 opacity=0.6）。
- 改动只影响 opacity<1 的表面，符合预期。

### TODO-2：DomeLight `inputs:shader_path` 未生效（低优先级）

`light.cpp:694` 读 `GetLightParamValue(id, TfToken("shader_path"))`，但 USDA
里写 `inputs:shader_path`（带 `inputs:` 前缀），USD forward 时没 strip 前缀，
导致 procedural sky shader 永远不启用，DomeLight fallback 到纯色填充。

**修复方向**：在 light.cpp 同时读 `inputs:shader_path`，或确认 USD 的
light 参数 forward 是否需要特殊 apiSchema。当前 transmission 测试用纯色
dome（0.6 灰）已足够，procedural sky 是美化项。

### TODO-3：Points prim（球几何）BLAS 构建 buffer flag bug（中优先级）

`geometries/points.cpp` 构建球 AABB buffer 时未设
`isAccelStructBuildInput` flag，nvrhi 报错：
`sphere_aabbs which does not have the isAccelStructBuildInput flag set`。
导致 Points prim（球几何）BLAS 构建失败 → 球不可见。

transmission 测试当前用 **mesh 球**（UV sphere）绕开此 bug。修 points.cpp
的 buffer flag 后可切回 Points prim。

### TODO-4：HD renderer 进程级单例导致测试隔离问题（低优先级）

HD renderer plugin (`Hd_RUZINO_RendererPlugin`) 和全局 `GPUSceneAssember`
是进程级单例，连续构造多个 `HydraRenderer` 会污染状态（`test_material_fallback`
在连续跑 5 个测试时失败 `Failed to get output texture`，但单独跑通过）。

`test_render_materials.py` 已用 module-scoped prober 缓解，`test_material_coverage.py`
暂未处理。`source/tests/_render_worker.py` 是 subprocess 方案的雏形（每个场景
独立进程渲染），但未集成进 pytest。

### TODO-5：巨型节点拆分（低优先级，纯重构）

`node_lpm.cpp`(742行)、`path_tracing.cpp`(605行)、
`material_brdf_analyzer.cpp`(546行)、`wetbrush_render.cpp`(540行) 较长，
可评估拆成子节点 + 内部 helper。不影响正确性。

## 官方 MaterialX 1.39.5 评估结论（不升级）

2026-08 评估了官方 MaterialX 1.39.5 的 Slang generator 能否替代自研。
**结论：不能。** 两者根本路线不同：

| 维度 | 自研（当前） | 官方 1.39.5 |
|---|---|---|
| 路线 | 路径追踪 + callable 两段式分发 | HW 光栅化（继承 HwShaderGenerator） |
| shader 入口 | `fetch_shader_data` callable | `main()` + `[shader("vertex"/"fragment")]` |
| sampling | 手写 callable (GGX VNDF / MIS / 光源) | 光栅化 light loop，无 BRDF 重要性采样 |
| binding | 自定义 BindlessContext + nvrhi | `cbuffer` + slang-rhi ShaderCursor |
| 材质特化 | standard_surface/UsdPreviewSurface 劫持 | 完全通用，无特化 |

官方 generator 覆盖不了 sampling（不在 generator 里）、binding（nvrhi 无法用
slang-rhi）、材质特化（无 dispatch 契约）。自研的 ~4650 行 generator 是为
路径追踪 + bindless + nvrhi 量身做的，保留是正确的。**短期不升级 MaterialX**。
