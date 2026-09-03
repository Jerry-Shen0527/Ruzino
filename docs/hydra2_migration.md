# hd_RUZINO Hydra 2.0 迁移

> 状态：**M1、M2、SDK 26.08 转正全部完成**（2026-09-02 当日）。
> 背景：本仓 SDK 原为 OpenUSD 26.03，hd_RUZINO 是双层 Hydra 1.0（delegate 走 emulation、
> 视口走 UsdImagingGLEngine）。上游新功能只落 2.0、emulation 已转正默认通道。
> 本日完成：scene index 挂链 + 摄取面直读（points/field/volume/instancer/light/mesh
> 的全部可直读拉取 + material 网络直读 + mesh topology 含 GeomSubsets）+
> SDK/Binaries 全线切到 26.08。正式运行时上渲染回归 22/22。

## 已完成：Milestone 1 — 插件级 2.0 参与

**目标**：让 hd_RUZINO 在 scene index 链上有自己的挂点，作为后续 sim 数据注入的正门。

**实现**（全部已构建、已验证、未提交）：

| 文件 | 内容 |
|------|------|
| `source/Runtime/renderer/source/simSceneIndex.{h,cpp}` | `Hd_RUZINO_SimSceneIndex`：`HdSingleInputFilteringSceneIndexBase` 纯 passthrough 观察者；`HD_RUZINO_SIM_SCENE_INDEX_DEBUG=1` 时 spdlog 打印链上 PrimsAdded/Removed/Dirtied（含 locator） |
| `source/Runtime/renderer/source/sceneIndexPlugin.{h,cpp}` | `Hd_RUZINO_SceneIndexPlugin`：`TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)` 里 `RegisterSceneIndexForRenderer("RUZINO Renderer", ...)` 自注册（display name 必须与 plugInfo `displayName` 一字不差） |
| `rendererPlugin.{h,cpp}` | override `GetSceneIndexCreateArgs()`（26.03 时代名为 `GetSceneIndexInputArgs`）：motionBlurSupport/cameraMotionBlurSupport=false（行为中性，上游 scene index 可跳过时间采样计算） |
| `resources/plugInfo.json` | 增加 `Hd_RUZINO_SceneIndexPlugin` 类型；**必须带 `displayName`+`priority`**，否则 HfPluginRegistry 报 "type information incomplete" 且**静默不挂链** |
| `tests/test_hydra_scene_index.py` | 子进程探针（免疫测试顺序）：debug env 下断言 `[SimSceneIndex] created` 与 `+` 通知流出现；默认无日志 |

**链路事实**（26.03 实测）：
- `UsdImagingGLEngine` 默认路径（`USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX_OBSERVER_RENDERER` 默认 true）在 `SetRendererPlugin` 时：
  `mergingSceneIndex → AppendSceneIndicesForRenderer(displayName)（←我们挂在这）→ (可选 caching) → terminal → plugin->CreateRenderer(terminal, ...) → 无 `_CreateRenderer` override 时走 `_CreateRendererFromRenderDelegate` 后端 emulation`。
- 交互视口（usdview_widget 的 RuzinoEngine）与离线 HydraRenderer（hd_RUZINO_py）**都**经 UsdImagingGLEngine，因此都被覆盖。
- 通知流验证过：population 时相机/renderTask/AOV renderBuffer 等以 locator 级脏标记
  （`task/parameters`、`renderBuffer/dimensions`）流过我们的节点。

**验证**：`test_hydra_scene_index.py` 2/2 通过；渲染回归 19/19 通过
（test_render_delegate / test_rendering / test_raster_pipeline / test_hydra_renderer / test_materials）。

## SDK 26.03 → 26.08：构建、全量编译、运行时冒烟全部完成（2026-09-02）

- 配方在仓库根 `configure.py`（自研，非 build_usd.py；`openusd_version` 常量在 ~L560）。
  **直接跑 `process_usd` 危险**：它会清空 `SDK/OpenUSD/<variant>` 前缀全量重建所有依赖
  （含 Boost/OpenVDB+CUDA/OIIO，数小时）并覆盖 `Binaries/`。
- 隔离方案：`scripts/build_usd_isolated.py` —— 复制 `SDK/OpenUSD/Release` →
  `SDK/OpenUSD/26.08-Release`，剥掉 USD 自身安装物
  （`pxrConfig.cmake`、`include/pxr`、`lib/{usd_ms*,cmake/pxr,python,usd}`、`plugin/usd`、
  `bin/usd*.exe`；注意 `share/` 是 Ptex 的 cmake、`resources/` 是 OpenSubdiv 的，**不能删**），
  只调 `configure.build_usd()`（它以 `include/pxr/pxr.h` 判已装）。
- **结果：26.08 Release 构建成功**（5346 步零错误；usd_ms.dll 46.6MB；pxrConfig=2608；
  hdStorm 等 plugin 齐全）。依赖未升级（MaterialX 1.39.3 等——configure 通过，
  26.08 对现有版本集兼容）。两个无害偏差：
  - Python 绑定装到 `lib/site-packages/pxr`（26.03 布局是 `lib/python/pxr`）——
    正式采用时加 `-DPXR_PYTHON_INSTALL_DIR=lib/python` 保持布局，免改下游脚本；
  - SDK/python 缺 jinja2 → usdGenSchema/usdInitSchema 跳过（26.03 本来也没装它们，非损失）。
- **全仓对 26.08 全量编译通过**（`build-2608` 目录，1335 步零错误，含 Editor/stage/usdview
  的 pxr 用点）；rendererPlugin 的双版本守卫两个分支都验证过
  （26.03 分支=主 build，26.08 分支=GetSceneIndexCreateArgs+HdRendererCreateArgsSchema IsSupported）。
- **26.08 运行时冒烟通过**（隔离输出 `Binaries/Release-2608`，
  `RZ_BUILD_TYPE=Release-2608` 跑测试）：链路活性 2/2 + points A/B 1/1。
  冒烟踩掉两个 26.08 特有陷阱（见下）。

### 26.08 运行时陷阱（冒烟实测）

1. **`usd/` 插件资源目录必须与 usd_ms.dll 同版本**。把 26.03 运行时的 `usd/` 目录
   配 26.08 的 usd_ms 会产生：26.03 execIr schema 资源与 26.08 内建类型声明冲突
   （Coding Error: _DeclareType ... different set of bases）、26.08 新增的
   HdSt_BackPlateSceneIndexPlugin 等因 26.03 hdStorm plugInfo 缺条目而
   "type information incomplete"。修法：`usd/` = 26.08 prefix 的
   `lib/usd/*` + `plugin/usd/*` 合并（两边顶层 plugInfo.json 内容相同，覆盖无妨），
   再放回我们的 `usd/hd_RUZINO/` 子树。
2. **Hybrid 排序策略丢弃纯 C++ 注册的 scene index 插件**。26.08 新增
   `HD_SCENE_INDEX_PLUGIN_ORDERING_POLICY_DEFAULT`（默认 **Hybrid**）：
   `AppendSceneIndicesForRenderer` 的合成列表**以 JSON（plugInfo）声明为基底**，
   C++ `RegisterSceneIndexForRenderer` 的非 callback 条目靠 pluginId 与 JSON 条目配对
   合并排序约束——plugInfo 没有对应声明时该条目**静默消失**（26.03 无此要求）。
   修法：plugInfo 类型加 `"loadWithRenderer": "RUZINO Renderer"`（对 26.03 无害，
   只是额外的预加载声明）。
3. 未决（不阻塞）：26.08 的 pxr Python 绑定（site-packages 布局）在现有测试环境
   `import pxr` 报 DLL load failed——hd_RUZINO_py（nanobind 直链 usd_ms）不受影响；
   转正时按官方布局整理（PXR_PYTHON_INSTALL_DIR +路径）后复验。

### 共享 Binaries 陷阱（已发生并已修复，参数化已完成）

`OUT_BINARY_DIR` 原全局写死 `Binaries/<type>`：scratch 构建会把不同 SDK ABI 的 DLL
写进共享 Binaries 形成混态（当日发生过一次，已通过重链接恢复）。
**现已参数化**：根 CMakeLists `-DOUT_BINARY_DIR=` 可覆盖 +
`build_devshell.ps1 -OutBinaryDir`；26.08 构建全程落在 `Binaries/Release-2608`，
共享 `Binaries/Release` 未再被触碰（事后哈希校验 usd_ms.dll 仍为 26.03 原版）。
26.03 SDK 本体在 `SDK/OpenUSD/Release`（未动），26.08 在 `SDK/OpenUSD/26.08-Release`。

### 转正（当日完成）

- `SDK/OpenUSD/26.08-Release` → `SDK/OpenUSD/Release`（26.03 完整备份保留在
  `SDK/OpenUSD/26.03-Release`）；前缀内 `lib/python/pxr` ← `lib/site-packages/pxr`
  补齐旧布局。
- `Binaries/Release` 运行时切换：usd_ms.dll / usd/ / pxr/ / python/pxr 全部 26.08。
- `configure.py`：`openusd_version = "26.08"` + `build_usd()` 增加
  `-DPXR_PYTHON_INSTALL_DIR=lib/python`（今后重建布局一致）。
- `rendererPlugin.{h,cpp}` 的 `PXR_VERSION` 双版本守卫**已删除**，只保留 26.08 API
  （`IsSupported(HdRendererCreateArgsSchema)`、`GetSceneIndexCreateArgs`）。
- 主 build：`cmake` reconfigure + `ninja -t clean` + 全量重编 1335 步零错误，
  Ruzino.exe 链接完成；正式运行时回归 22/22 零 flake。
- Debug 变体：`scripts/build_usd_isolated.py --base-variant Debug` 后台构建中。

**⚠️ 换 SDK 必须强制全量重编**：`mv`/复制切换前缀**不改变文件 mtime**，ninja 不会重编
pxr TU——旧 26.03 obj（符号命名空间 `pxrInternal_v0_26_3__`）与新 26.08 obj 混链产生
数百个 unresolved external。切换后必须 `ninja -t clean` + 全量。

**残余步骤**：pxr Python 绑定在 26.08 布局下 `import pxr` 的 DLL 解析问题（不阻塞
nanobind 直链路径）——`lib/python/pxr` 布局已就位，待复验；交互视口（Ruzino.exe +
usdview_widget）人工冒烟。

## 工具链改动（本次已落地）

- root `CMakeLists.txt`：`SDK_FOLDER` 可用 `-DSDK_FOLDER=` 覆盖（默认逻辑不变）。
- `scripts/build_devshell.ps1`：新增 `-SdkFolder` 参数（透传给 cmake，已按脚本自身的
  引号规则处理，避免 `-DSDK_FOLDER=26.08-Release` 被 PowerShell 拆 token）。
- `scripts/build_usd_isolated.py`：隔离前缀 USD 构建驱动。
- `rendererPlugin.{h,cpp}`：~~`PXR_VERSION >= 2608` 双版本守卫~~（转正时已删除，只保留 26.08 API：`IsSupported(HdRendererCreateArgsSchema)`、`GetSceneIndexCreateArgs`）。

## Milestone 2 — 摄取面直读 data source（第二批完成，覆盖全部轻中量 prim）

**共享基础设施**：`source/Runtime/renderer/source/hydra2Ingest.{h,cpp}`——声明头 +
单 TU 实现（schema 头的全部 include 顺序/COM 宏陷阱被封在 cpp 里；实现改动不再
连带重编 10 个消费者 TU）。实现镜像 `HdSceneIndexAdapterSceneDelegate` 各翻译的
直读助手：`ReadPrimvar`（primvars 容器）、`ReadLightParam`（light 容器）、
`ReadVolumeFieldParam`（volumeField 容器）、`ReadTransform`（xform schema，缺省
identity）、`ReadMaterialId`（materialBindings schema 按 binding purpose）、
`ReadVisible`（visibility schema，缺省 true；含 legacy instancer 特判）、
`ReadPrimvarDescriptors`（primvar 名单→name/interpolation/role/indexed，逐项镜像
`HdPrimvarDescriptorFromSchema`；无效插值丢弃；26.03 无 element 插值）、
`ReadInstanceIndices`（instancerTopology schema `ComputeInstanceIndicesForProto`）。
每个助手失败时返回空值/false，调用方回退 legacy Get*。迁移期 A/B 门
`RZ_HYDRA2_PREFER_LEGACY=1` 在像素级对照全部通过后已于 2026-09-02 移除。

**已转换（全部：直读优先 + legacy fallback）**：

| 文件 | 转换的拉取 |
|------|-----------|
| `geometries/points.cpp` | points/widths/displayColor/debugKey（首批，已收编进共享助手） |
| `geometries/field.cpp` | filePath/fieldName（volumeField 容器） |
| `geometries/volume.cpp` | transform（xform）、materialId（materialBindings） |
| `volume_impl/volume_impl.cpp` | volumeType（impl 选择） |
| `volume_impl/cloud_volume_impl.cpp` | 全部 cloud 参数 primvar |
| `volume_impl/wetbrush_volume_impl.cpp` | 全部 grid 元数据 primvar（gridRes*/cellSize/gridMin/paintField） |
| `instancer.cpp` | instance primvar 名单+值、instancerTransform（适配器映射到同一 xform schema）、instanceIndices |
| `light.cpp` | **49 处** `GetLightParamValue` → light 容器直读；transform；simpleLight 的 `Get(id, params)`（适配器把它路由到 light 容器，已按此直读） |
| `geometries/mesh.cpp` | visible、materialId、points、primvar 名单+值循环（_UpdatePrimvarSources）、transform、instanceIndices |

**刻意推迟 → 已全部完成（当日第二轮）**：
- mesh `GetMeshTopology`：✅ 已直读（`Ruzino_Hydra2::ReadMeshTopology`），含
  `_GatherGeomSubsets` 复刻（geomSubset 子 prim 遍历、invisible faces/points 并集、
  面集材质绑定）——适配器的 `_geomSubsetParents` 提示集只是性能优化，直读侧无条件扫描等价。
- material `GetMaterialResource`：✅ 已直读（`Ruzino_Hydra2::ReadMaterialResource`），
  完整复刻适配器的 `_ToMaterialNetworkMap` + `_Walk` + `_GetHdParamsFromDataSource`
  （含 colorSpace/typeName 元数据键 `colorSpace:<param>`、`typeName:<param>`，仅非空时写）
  + `_ToDictionary` + `includeDisconnectedNodes` 分支。
- camera：拉取在 USD 基类 `HdCamera::Sync` 内部，转换=重写基类行为，保留 legacy（唯一保留项）。
- renderBuffer：无 delegate 拉取。extComputation：stock USD 类。

**迁移中踩掉的坑（26.03 环境下，转正前）**：
1. `materialNetworkSchema.h` 直接 include 报语法错——生成头依赖调用方先 include
   `pxr/base/tf/staticTokens.h`（TF_DECLARE_PUBLIC_TOKENS）和 `materialConnectionSchema.h`。
2. **Windows COM 宏污染**：RHI/nvrhi 传递引入的 Windows 头 `#define interface struct`，
   把生成 token 列表里的 `(interface)` 替换成 `struct` 导致 hd_RUZINO 整批 TU 编译炸。
   修法：hydra2Ingest.h include 块外 `#pragma push_macro("interface")` +
   `#undef interface` + 完成后 pop。用真实命令行 `/P` 预处理失败 TU 定位（预处理展开里
   直接看到 `TfToken struct{"struct"...}`）。

**验证（正式 26.08 运行时，转正后）**：22/22 全过零 flake
（活性 2/2、points A/B、mesh/MaterialX/光栅/instancer/渲染基础全套）。

### 2026-09-02 复核与收尾（对上游 26.08 源码逐函数核对后）

- **保真性核对**：拿 `SDK/OpenUSD/source/OpenUSD-26.08` 的
  `sceneIndexAdapterSceneDelegate.cpp` 逐行比对全部翻译——材质网络/geomSubsets/
  拓扑默认值等一致；修正三处偏差：`ReadVisible` 补上游的 legacy-instancer 特判
  （`isLegacyInstancer` 一律可见）、visibility 值缺失时镜像上游返回可见（原来走
  回退）、全量 `ReadPrimvarDescriptors` 丢弃无效插值条目（上游丢弃+TF_WARN；上游
  自己此处还有 `-1` 越界索引的 UB，我们映射成 Count 规避）。`_WalkMaterialNetwork`
  对畸形 connection 的空指针解引用加了防护（上游同样裸解引用，我们不跟着崩）。
- **mesh 描述符读取**：每 Sync 全量读一次（原 6 个插值桶各走一遍完整 scene index
  链）。
- **A/B 验证扩展后移除**：新增 mesh+GeomSubsets+三材质+球灯场景（像素级 diff
  <0.02 通过）与 PointInstancer+灯场景（均值一致通过）后，按定调移除
  `RZ_HYDRA2_PREFER_LEGACY` 门；三场景测试转为直读冒烟（非黑断言）钉子。
- **发现既有 bug（与迁移无关，待查）**：instancer 渲染跨进程不确定——同一场景
  同一路径连跑三次图像互差最大 ~0.11（prototype geometryID 随 rprim Sync 顺序在
  3/4/1 间变化）。像素级断言在该路径修复前不可用。
- `sceneIndexPlugin.cpp` 注册注释更正：26.08 默认 Hybrid 排序策略下 plugInfo 的
  `loadWithRenderer` JSON 条目与 C++ `RegisterSceneIndexForRenderer` **缺一不可**
  （纯 C++ 注册会被静默丢弃），两处必须同步维护。

**修正一个旧判断**：8-30 评估认为"1.0/2.0 数据流无公共形状、无法渐进"。实际存在渐进路径——
`HdRenderIndex::GetTerminalSceneIndex()` 在 26.03 即可用，emulation 适配器
（`HdSceneIndexAdapterSceneDelegate`）正是把我们 Sync 里消费的 Get* 面
（GetMeshTopology/GetPrimvar/GetMaterialResource/GetLightParamValue…）按需翻译成 data source 读取。
因此：

- **Track A（渐进，低风险）**：在现有 `Sync()` 内，按 prim 类型把 `sceneDelegate->Get*(id)`
  逐步替换为 terminal scene index 的 schema 直读（points 已按此完成）：
  ```cpp
  HdSceneIndexPrim prim =
      sceneDelegate->GetRenderIndex().GetTerminalSceneIndex()->GetPrim(GetId());
  HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
  ```
  脏位仍由 emulation 从 PrimsDirtied 翻译驱动，Sync 照常被调；行为等价但绕开适配器的
  值翻译/缓存层，并解锁 Get* 面不暴露的数据（完整 primvar 树、自定义容器、scene globals）。
- **Track B（自主 population，sim 零拷贝正门）**：SimSceneIndex 从 passthrough 升级为
  合成节点——内部持 `HdRetainedSceneIndex`，节点图仿真结果（GPU buffer registry 的
  points/网格）以 prim + data source 形式 AddPrims/Dirtied 注入 `/RuzinoSim` 子树；
  `Hd_RUZINO_Points` 现成的 `debugKey` 零拷贝模式即消费端。这是
  gpu-buffer-zerocopy-generalization 的渲染侧前置。

**迁移顺序**（每步过 render_anchor 像素对比再进下一步）：
~~points（已完，双 SDK A/B 验证）~~ → camera/renderBuffer（trivial）→ field/volume（薄）→
instancer → light 家族（GetLightParamValue 面最大）→ mesh（最重，拓扑+面变化 primvar 三角化）→
material（MaterialX 网络 FetchMaterialNetwork→GetMaterialResource 直读）。

## 2026-09-03 审阅收尾（复审落地）

对上述全量 diff 的第二轮代码审查（对照上游 26.08 源码逐函数复核）后落地的改进：

- **reader API 统一 bool+out**：`ReadPrimvar`/`ReadLightParam`/`ReadVolumeFieldParam`
  与其余 reader 一致化。语义：true=数据源路径可服务该 prim（值可能合法为空=未 author，
  与适配器返回相同，不再触发回退）；false=回退 legacy（无 terminal SI / 无 prim /
  legacy-emulation prim（适配器尾部 sceneDelegate 兜底会另行服务）/ ReadVolumeFieldParam
  的 primType 门控不过）。效果：缺席的可选参数（cloud 参数、灯光可选项等常态场景）
  不再每次 Sync 双走直读+legacy 两条链。陷阱：`HdLegacyPrimTypeIsVolumeField`
  虽是 HD_API 但**单体 usd_ms 不导出**，链接不到——两行比较已内联镜像。
- **geomSubset 扫描提示集**：SimSceneIndex 观察链上 PrimsAdded 的 geomSubset 子 prim，
  记录父路径（hydra2Ingest 进程级注册表）；ReadMeshTopology 只对有提示的 prim 扫描
  子 prim（镜像上游 `_geomSubsetParents`，且补了上游没有的 PrimsRemoved 子树剪枝）。
  无活观察者（插件未挂链的上下文）时保守全扫。提示集只影响性能不影响正确性
  （过期条目=多一次空扫描）。
- **类型防御日志**：mesh/points 的 points、widths primvar 持意外类型时
  spdlog::warn（此前静默保留旧数据）。
- **测试**：两份 ~50 行子进程渲染脚本样板合并进 hydra2_ab_common
  （run_render_subprocess，帧数/输出可控）；新增 invisible_occluder 场景——
  红色背板被更近的 `visibility="invisible"` 绿色遮挡板完全覆盖，断言红通道主导
  （ReadVisible false 路径端到端：不可见 mesh 被 `IsVisible()` 挡在 TLAS 外；
  判别力已验证：遮挡板改 inherited 时绿主导 diff=-0.168）。
  **invisible GeomSubset 分支（SetInvisibleFaces 并集）从 usda 不可达**：
  `UsdGeomSubset` 非 `UsdGeomImageable`，usdImaging 不为 subset prim 发布
  visibility schema（dataSourcePrim.cpp 只对 Imageable+authored 发布）——该分支
  只服务 legacy 转换链（HdLegacyGeomSubsetSceneIndex）；且 hd_RUZINO 尚未消费
  `GetInvisibleFaces()`。留档不测。
- **工具链**：`build_devshell.ps1` 强制 `-SdkFolder` 搭配 `-OutBinaryDir`
  （缺配对直接报错——防 09-02 的 Binaries ABI 混态事故复发），cmake 参数改数组
  splatting；`build_usd_isolated.py` strip 列表补 `lib/site-packages`
  （26.08+ 绑定新布局，未来以 26.08 为 base 构建更新版本时防旧绑定残留）；
  points.cpp 清掉 A/B 门时代遗留的死 include。
- 验证：format 后全量重编零错误；renderer 测试目录 25/25 通过
  （原 24 + 新增 invisible_occluder）。

**回归红线**（历史血泪全在摄取半边，改哪类 prim 就重验哪类）：
MaterialX static document 污染、相机垂直翻转（up 行取反）、shader-path callable、
slang register() 注解、`RefCountPtr::operator&` 陷阱、config:* 属性的 `__ruzino_dirty`
通知 workaround（ruzino_engine.cpp）、三个非标准 GetRenderSetting 逃生口
（`RenderNodeSystem`/`HdRuzinoRenderParam`/`VulkanColorAov[:name]`——保持不动或明确迁移）。

## 里程碑之后（不在本次范围）

- 视口层 UsdImagingGLEngine → 2.0 任务层（上游 HdxTaskController 26.08 才 deprecated，不急）。
- Storm/nvrhi fork 落地后两渲染器统一 native scene index 消费。
