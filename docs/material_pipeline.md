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

### TODO-1：UsdPreviewSurface opacity 语义重叠（中优先级）

`UsdPreviewSurface.slang:138-141` 仍用 opacity 做 diffuse↔dielectric mix 权重：

```slang
float transmission_mix_amount_out =
    (opacityThreshold > 0.0) ? 1.0 : opacity;
```

这与主循环新加的 opacity presence 穿透（P0 改动 [3]）**语义重叠**——
opacity 被算了两次（一次主循环直通，一次 BSDF 内 mix）。对 preview surface
目前"歪打正着"（preview 没有 transmission 参数，透明只能靠 opacity，BSDF 内
mix 反而补偿了效果），但不干净。

**正确做法**：opacity 应该只在主循环做 presence（直通），BSDF 内部不该再用
opacity 做 mix 权重。需修改 `UsdPreviewSurface.slang` 去掉 :138-141 的
opacity→transmission_mix 映射，让 opacity<1 的 BSDF 求值完全等同于 opacity=1
（因为穿过的那部分光线在主循环已经处理了）。

**风险**：需对比修改前后 preview_surface 渲染图，确认视觉一致或更好。

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
