# Wetbrush 复现：3D 笔刷绘画仿真 + 体积渲染

> 复现 Wetbrush (Chen et al., SIGGRAPH Asia 2015) 的 GPU bristle-level 3D 绘画
> 仿真与渲染。本文档记录架构演进、已完成工作、关键经验教训、遗留目标。
> 论文原文见 `docs/Wetbrush_GPU_based_3D_painting_simulation_at_the.md`。

## 目标

在 Ruzino 框架内实现一个 paper-faithful 的 Wetbrush 复现：

1. **仿真**：bristle-level 笔刷物理（§4.1）+ grid-based 液体仿真（§4.2）+ hybrid
   grid-particle 表示（§5）+ bristle-particle 液体转移（§5.1）。
2. **渲染**：paper §6 的 raycast 体积渲染（first-cross 表面检测 + penetration blend
   + ambient occlusion），paint 显示为画在纸上的 3D 笔触（paint 在 Z 方向有完整
   `res_z` 层体积，非薄片；见 §4.2 grid）。
3. **验证**：双色交叉笔画（wet-in-wet mixing）渲染成动画序列，确认颜色混合、
   覆盖、干燥行为正确。

所有改动遵循 paper；任何偏离 paper 的改动（hack）必须先经用户批准。RHI / Stage
等通用模块不被 Wetbrush 特定内容污染。

## 整体架构

### 数据流（interleaved sim+render，零拷贝）

```
每帧:
  stage.tick(dt)
    └─ RuzinoGraph zone: deposit → bristle → fluid → commit
       commit 每帧:
         1. pack_float4 dispatch: density + color_r/y/b → packed_paint (Float4)
         2. SharedGPUBufferRegistry.register("wetbrush_paint_field", packed_paint, meta)
         3. (readback 全 grid → Paint Field 3D 点云 → write_usd，仅诊断用)
  hydra.reset_accumulation()   ← 强制 reset（host escape hatch）
  hydra.render(t) × SPP
    └─ Hd_RUZINO_WetbrushVolume rprim.Sync:
         → 检测 registry version bump → create_gpu_resources
         → Phase 1: 直接复用 packed_paint GPU buffer（零拷贝，RawBuffer_SRV）
         → gridRes/cellSize/gridMin 从 registry metadata 读
    └─ wetbrush_render 节点 dispatch:
         → VolumeIntersection: 只在射线穿过 paint cell (density>0.02) 时 ReportHit
         → VolumeClosestHit: first-cross + penetration blend + Lambertian + AO
         → 空 volume 段不 ReportHit，射线继续到 Paper mesh
```

### 关键设计决策

- **全局持久 grid + 局部计算窗口**（paper §4.2）：一个覆盖整张 canvas 的大 3D grid
  是 paint 的持久存储；每帧只在笔刷周围的 active window (256×256×res_z) 内计算。
  窗口只是 dispatch 范围，不 commit/clear。窗口 ≥ 笔刷 footprint 直径（4096 下
  164 格），128 会裁掉外圈 bristle。
- **零拷贝 sim→render**（详见下文）：sim 的 `packed_paint` GPU buffer 通过
  `SharedGPUBufferRegistry` 直接给 render rprim 用，无 CPU readback / USD primvar
  往返。这是支撑高分辨率（4096）的关键 —— bake 回路在 4096 下 RAM 爆炸。
- **paint 是纯 emissive**（paper §6）：VolumeClosestHit 算出 paint 颜色后直接作为
  辐射度，RayGen 立即 break，不参与 path tracing bounce。

## 已完成的工作

### 1. Sim 存储：全局 grid（Part A，committed `f8321079`）

3D fluid fields 从 window-sized 改为 **global grid**。删除 2D canvas 层 +
canvas_commit（paper footnote 1 拒绝 2D height-field）。

- `brush_sim_common.hpp`：删除 `canvas_*` 字段 + `canvas_commit_program`。
- `node_brush_wb_deposit.cpp`：`alloc_win_n3d = resolution³`（global）。
- `common.slangh`：`window_map` 返回 global index。
- 4 个 fluid stencil shader（divergence/jacobi/gradient/advect）+ 8 个 world→grid
  shader：删 window-local 转换，直接索引 global grid。

### 2. 守恒链修复（Eq.15/16）

- **bristle_merge 守恒 bug**（`f8321079`）：`density += bd * ink_amount` 是净注入
  bug（bd 已经是 mass-weighted）；改为 `density += bd`。移除 `min(...,3.0)` 硬 cap。
- **density 爆炸**（`361a7bac`）：`W_smooth_3d` 在 r≈0 返回 ~317000；particle_to_grid
  (Eq.16) 缺 cell-volume 因子 → 单粒子注入 158K density。修复：Eq.15/16 都乘
  `dV = cell_size² × cell_z`。

### 3. §4.1 bristle-level 物理 + §4.2 真 3D volume grid

两项都是 paper-faithful 实现（之前误记为"遗留目标"，已澄清）：

- **bristle 物理**（`bristle_simulate.slang`，329 行）：完整实现 paper §4.1。
  - Eq.2 非惯性 brush frame 积分，4 个惯性项全在：直线加速 `a_B`、离心 `ω×(ω×x_B)`、
    Euler 角加速 `ω̇×x_B`、Coriolis `-2ω×v_B`，按 `dv/dt = a_i − β_B·(四项之和)`。
  - PBD 约束（Müller et al. 2007）：不可拉伸（双向距离）+ 弯曲 + rest-shape restore +
    canvas collision + pressure splay（Fig 3a 扇形展开）。
  - 每 bristle 是 M 顶点链（`BRV_STRIDE`），根节点用 Vogel/golden-angle spiral 均匀铺
    满 brush footprint；bristle 感受 grid-liquid drag（§4.1 要求）。
- **真 3D volume**（`node_brush_wb_deposit.cpp:140-154`）：grid 是 `res×res×res_z`
  （如 1024×1024×64），**64 层 Z**，`height = paper_size × res_z / res`，
  `canvas_z` 是 volume 的**底部**（`brush_sim_common.hpp:241-243` "paint-volume floor"）。
  paint 在 Z 方向有完整体积，不是薄片。`bristle_simulate` 的 `if (p.z < canvas_z)` 只是
  挡 bristle 顶点不穿透纸面，不钳制 paint 的 Z 分布。

### 4. §6 体积渲染（committed `06422279`）

`Hd_RUZINO_WetbrushVolume` rprim + `wetbrush_render` 节点（path_tracing + 2 个
procedural volume hit group）。

- `VolumeClosestHit`：first-cross（density > 0.02）→ normal from gradient →
  penetration blend（oil-density-proportional depth，加权平均 pigment）→
  Lambertian shade (0.4+0.6·facing) → 64-ray AO。
- `VolumeIntersection`：只在射线段确实穿过 paint cell 时 ReportHit；空 volume 段
  不报，射线继续到 Paper mesh（否则 paint 看起来画在背景上而非纸上）。

### 5. 圆形跳变 artifact 消除（committed `790c0092`）

笔刷移走时圆形边缘颜色跳变。根因：grid_to_particle (Eq.15) 只从 emit cell 减
density，违反 paper §5.2 "c can be any cell near new particles"。修复：density
减法扩散到 3×3×3 邻域，按 W kernel。

### 6. paper §5.2 守恒清理

移除多处偏离 paper 的 hack：0.1 mass 缩放、0.5 retention、mass cap、velocity 阻尼、
bristle_psi 进 is_solid 判定（paper §4.2 只用 dryness）。mode1 window mapping bug
修复。

### 7. 零拷贝 sim→render（SharedGPUBufferRegistry）

**问题**：bake 回路（commit readback 全 grid → Python 光栅化成 dense render grid
→ USD primvar → rprim 读 primvar 建 buffer）在 4096 grid 下 RAM 爆炸（render grid
3.8B cells，60GB/帧）。

**方案**：sim 的 `packed_paint` GPU buffer 直接给 render rprim 用。

- `SharedGPUBufferRegistry`（`source/Core/RHI/`）：generic key→buffer+meta 注册表，
  语义无关（不知道 density/Wetbrush）。
- commit 每帧 `pack_float4` dispatch 后注册 `"wetbrush_paint_field"`，metadata 带
  `{resX,resY,resZ,cellSize,gridMinX,Y,Z}`。
- rprim `create_gpu_resources` Phase 1：查 registry，hit 则直接用外部 buffer
  （`RawBuffer_SRV`），从 metadata 读 grid 几何；miss 则走 primvar fallback。
- **修复了之前的两个 blocker**：(1) `packed_paint` 加 `CanHaveRawViews` flag
  （rprim 用 RawBuffer_SRV 绑定，原 buffer 只有 TypedViews → 读零）；(2) commit 在
  pack dispatch 后做 UAV→ShaderResource state transition（`setPermanentBufferState`
  + `commitBarriers` + `waitForIdle`）。

### 8. Interleaved sim+render（真 per-frame 动画）

`render_wetbrush.py` 改为 interleaved：每帧 `tick(dt)` → `render(t)` × SPP → save PNG。
sim_graph 引用保留到循环结束（否则 GPU buffer 被释放，registry 悬空）。删除了
bake_render_scene（不再需要）。

### 9. accumulate 跨帧叠加修复

**问题**：interleaved 下已画位置随时间变深（拖影）。根因：`accumulate` 渲染节点
的 reset 触发只看 material/light/size dirty，漏了 geometry content change —— sim 每
帧更新 paint，但 `reset_accumulation` 没触发，path tracing 样本跨帧叠加。

**双机制修复**（两个机制都保留）：
1. **自动触发**（正确架构路径）：`renderer.cpp` 轮询 registry version bump → mark
   `DirtyGeometry` → `wetbrush_render` 检测 `geom_dirty` → `reset_accumulation`。
   `wetbrush_render.cpp` 在 geom_dirty 时设 reset；`renderer.cpp` fold
   `pending_force_reset_accumulation` 进 global_payload。
2. **强制打穿**（host escape hatch）：`HydraRenderer::reset_accumulation()` Python
   接口，host 显式要求。通过 renderParam 的 `pending_force_reset_accumulation` sticky
   flag，renderer.cpp 在每次 render 开头 fold 进 reset，node 执行后消费清除。

Python loop 里两个机制都触发（belt-and-suspenders）。

### 10. Group B/C buffer 改窗口大小

**问题**：4096 grid 下 Group A（26 个全 grid buffer）占 ~112GB 显存，Group B/C（14 个
bristle/particle accumulation buffer）原本也是全 grid，额外浪费。

**方案**：paper §5/§5.1 的 bristle sample 和 particle 只在笔刷局部 window 存在，14 个
Group B/C buffer 改成窗口大小分配（128²×res_z = 1M cells，vs 全 grid 1B cells）。
4 个 shader（bristle_rasterize / particle_rasterize / bristle_merge /
bristle_liquid_transfer）的索引从 global 改为 window-local。

### 11. 颗粒感改善

bristle 单点 XY splat + 1024 grid 下 footprint 稀疏 → paint 呈颗粒状。paper §6 说
brush 含 40-600 bristles，平滑来自密集采样。NUM_BRISTLES 80 → 600（paper 上限），
纯参数、paper-faithful，颗粒感显著减弱。

后续（详见 §13）：bristle→grid 注入改用 paper 自己的 `W_smooth_3d` 3×3×3 核 +
细步进，彻底消除颗粒/空洞。600 bristles 仍是平滑密度的基础（每根 bristle 一个 XY
列，采样沿 Z 铺开）。

### 12. 渲染观感调整

- **纸面**：整张 canvas（±paper_size/2），不是笔触 bbox 外一圈 margin。配合
  VolumeIntersection 的空段透明，paint 画在纸上。
- **光照**：DistantLight intensity=3 + DomeLight intensity=0.25（冷色调填光，把背景
  从死黑 lift 出来）。
- **相机**：tight framing 笔触区域（frame_size≈0.35），3/4 俯视角。
- **gridMin Z 对齐**：marker scene 的 gridMin Z 必须和 sim registry metadata 一致
  （canvas_z=0），否则 paint 渲染位置偏移，产生光晕 + 悬浮。

### 13. 平滑度修复 + paper-faithful 渲染补全

- **bristle→grid 注入用 paper 的 W kernel**（`bristle_rasterize.slang`）：单格 XY
  splat 改为 `W_smooth_3d`（h=1.5 格，与 §5.2 Eq.15/16 粒子注入同一个核）3×3×3
  归一化分摊。之前每帧只有 600 个 XY 格被画（每 bristle 一列，采样全在 Z 上），
  密度场是散点 → 表面凸点。现在每个采样平滑铺开，密度场连续。
- **细步进**（`node_brush_wb_deposit.cpp`）：`n_sub` 阈值从"1 个笔刷直径"改为
  "~2 格"（cap 128）——paper §Limitations 明说平滑依赖 "densely sampled stroke
  path"（论文实时负担不起，我们是离线渲染）。1024 下每帧 ~11 子步，消除了相邻帧
  间 20-41 格的斑马纹空档。
- **active window 256²**：`WIN_ALLOC_XY` 128→256。4096 下笔刷 footprint 直径 164
  格 > 128 窗口 → 外侧 ~39% bristle 采样被裁掉（footprint 被砍成方形 + 窗口量化
  跳动）。256²×res_z×14 buffer ≈ 235MB，消费级卡可接受。
- **渲染 RYB→RGB**（`wetbrush_render.slang` + `volume_intersection.slang`）：混色
  blend 出的颜料是 RYB 空间，直接当 RGB 辐射度输出是错的（纯黄 (0,1,0) 会显示成
  绿、混合色全偏）。`ryb_to_rgb`（Gossett & Chen 2004）转换后再输出。
- **§6 oil_density 穿透距离**：paper "we use the oil density to calculate a
  penetration distance"——pack_float4 新增 `packed_oil` 通道（第二个 registry key
  `wetbrush_oil_field`，rprim 绑定第二个 RawBuffer_SRV，`VolumeDesc.oilIndex`），
  渲染端 `penDepth = max(cell, oil×cell×8)`（油多→透明→穿透深→混合更多下层颜料）。
  无 oil buffer（primvar fallback）时回退 density 驱动。
- **mock 交叉笔触用真 RYB 黄**：`(1,1,0)` 在 RYB 里是橙色不是黄色，与蓝混出脏灰。
  改 `(0,1,0)`（paper Fig 11c 黄+蓝→绿验证真正成立）。

## 关键经验教训

1. **先量像素再下结论**。"纸是黑的"其实是背景 dome 蓝；"变深"先以为是 sim 累积，
   实际是渲染 accumulate 叠加。用 PIL 采样像素 + ASCII map 比肉眼判断可靠得多。

2. **bake 是过渡方案，零拷贝是终点**。bake 回路（CPU readback + Python 光栅化 +
   USD primvar）在高分辨率下 RAM 爆炸。零拷贝路径（registry）早就写好但被禁用，
   只差 buffer flag + state transition 两个 blocker。

3. **paper §6 first-cross 要在 intersection 阶段判断**。无条件 ReportHit 空体积段会
   吞掉射线（一次 TraceRay 一次结果），paint 看似画在背景上。把 first-cross 判断
   前移到 VolumeIntersection，空段不报，射线继续到纸面。

4. **accumulate 节点跨帧叠加是隐藏陷阱**。interleaved sim+render 下每帧是新场景，
   但 accumulate 只看 material/light/size dirty，geometry content change（sim 更新
   paint）不触发 reset。必须显式处理。

5. **显存是硬约束**。4096 grid 的 Group A 全 grid buffer（26 个 × 4096²×64）要 112GB，
   消费级卡跑不起。paper 能跑 4096 一定是 sparse 分配（paper 没明说）。Group B/C 改
   窗口大小省了 1000×，但 Group A 必须稀疏化才能真正上 4096。

6. **build 慢是 DevShell 初始化，不是 ninja 增量**。`build_devshell.ps1` 每次加载 VS
   DevShell（扫描 VS 安装）要 10-30 秒；ninja 增量本身只编改动文件。单 target 直接
   `ninja <target>.dll` 在 build/ 目录（需 vcvars 环境）。

7. **不要假设问题在哪一层**。"变深"我假设是 sim，实际是渲染；"颗粒"先假设是渲染
   raymarch，实际是 sim 的 bristle splat 离散性。跨层诊断比单层猜测快。

## 遗留目标

### 高优先级

- **Group A 全 grid buffer 稀疏化**：26 个核心 sim field 在 4096 下要 112GB。需要
  sparse / block-allocated grid（只为有 paint 的区域分配）。这是真正上 4096（paper
  分辨率）的前提。12 GB 卡当前用 `WETBRUSH_RES=1024` 跑（~7 GB），2048 需 28 GB。
- **压感输入接入**：`node_brush_capture` 已能捕获鼠标轨迹，但 `node_brush_input` 的
  BrushPressure 是 socket 常量（默认 1.0），无 Wintab / Windows Ink / pen pressure
  输入。压感→bristle 压扁→容量 Eq.12→注入量这条链目前断开。接入真压感会让 footprint
  随力度动态变化。
  （注：bristle 物理本身已 faithful 实现，见 `bristle_simulate.slang` 的 Eq.2 非惯性
  brush frame 积分 + PBD 约束 + canvas collision + pressure splay。）

### 中优先级

- **commit readback 优化**：interleaved 下 commit 每帧 readback 全 grid（诊断用），
  4096 下慢。零拷贝确认正确后可删除（renderer 不需要它）。
- **生产级 threaded sim/render 同步**：当前 Python sequential + waitForIdle 提供隐式
  同步，无并发。真多线程 sim/render 需要 double-buffering 或 GPU fence（registry 单
  buffer 帧间竞争）。
- **bristle 弯曲**：paper §4.1 的 bristles 随拖拽弯曲（Fig 4），采样沿弯曲链在 XY
  铺开——这正是论文"bristle-level"细节的来源。当前 rest-shape restore 刚度 0.5 把链
  拉直，128 采样叠在同一 XY 列（XY 覆盖 = 600 格/帧）。W kernel 注入 + 细步进已补偿
  平滑度，但真弯曲会带来更自然的 footprint 与笔触纹路。

### 低优先级

- **per-frame 真动画的 accumulate 正确性**：当前用 reset_accumulation 强制每帧从 0
  开始，SPP=32 够用但略噪。长期看 accumulate 节点应正确支持 animated scene。
- **§6 van der Laan metaball 粒子层**（可选增强）：screen-space splatter/filaments，
  paper 说是 optional enhancement，不是主笔触体。

## 关键文件索引

| 关注点 | 文件 | 说明 |
|--------|------|------|
| Registry（generic） | `source/Core/RHI/include/RHI/shared_buffer_registry.hpp` | key→buffer+meta |
| Registry impl | `source/Core/RHI/source/shared_buffer_registry.cpp` | Meyers singleton |
| Pack shader | `source/Editor/geometry_nodes/BrushSimulation/shaders/pack_float4.slang` | 4 float→1 Float4 |
| Sim state | `source/Editor/geometry_nodes/brush_sim_common.hpp` | WetbrushSimState, NUM_BRISTLES, packed_paint |
| Sim nodes | `node_brush_wb_{deposit,bristle,fluid,commit}.cpp` | global grid |
| Shader indexing | `BrushSimulation/shaders/common.slangh` | window_map, bristle_gi/grid_gi |
| Mock strokes | `node_mock_strokes.cpp` | 双色交叉笔画测试 fixture |
| Render rprim | `source/Runtime/renderer/source/geometries/wetbrush_volume.{h,cpp}` | Phase 1 零拷贝 lookup |
| Render node | `source/Runtime/renderer/nodes/wetbrush_render.cpp` | geom_dirty reset |
| Renderer | `source/Runtime/renderer/source/renderer.cpp` | registry version poll, reset fold |
| Render param | `source/Runtime/renderer/source/renderParam.h` | pending_force_reset, registry version |
| Render delegate | `source/Runtime/renderer/source/renderDelegate.cpp` | HdRuzinoRenderParam setting |
| Python binding | `source/Runtime/renderer/python/renderer.cpp` | reset_accumulation() API |
| Volume shader | `source/Runtime/renderer/nodes/shaders/shaders/wetbrush_render.slang` | VolumeClosestHit/Intersection |
| Volume helpers | `source/Runtime/renderer/nodes/shaders/shaders/volume_intersection.slang` | samplePaintField, intersectSlab |
| Test driver | `source/tests/render_wetbrush.py` | interleaved sim+render |
| Cross test | `source/tests/render_wetbrush_cross.py` | 双色交叉混合 |

## 构建 / 运行

```bash
# 增量构建（DevShell 脚本，加载 MSVC 环境 + ninja）
pwsh -File scripts/build_devshell.ps1

# 单 target（已在 build/ 有 build.ninja，需 vcvars）
cd build && ninja node_brush_wb_commit.dll

# 跑双色交叉渲染（interleaved，零拷贝）
cd Binaries/Release
python ../../source/tests/render_wetbrush_cross.py
# 输出: Binaries/Release/wetbrush_cross_sequence/frame_XXXX.png
#
# 显存不够跑 4096（默认，需 ~112 GB）时用环境变量降分辨率：
# WETBRUSH_RES=1024 python ../../source/tests/render_wetbrush_cross.py   # ~7 GB
# WETBRUSH_RES=2048 python ../../source/tests/render_wetbrush_cross.py   # ~28 GB
# （Python 须 3.13；PATH 里的默认 python 若是 3.12 会报
#  "Module use of python313.dll conflicts"——用 scoop 的 python313 或
#  Binaries/Release/python.exe）
```

Shaders 运行时编译（非 build 时）。编辑 `.slang` 后无需 rebuild，但 renderer 加载的
是 deployed copy（如 `Binaries/Release/usd/hd_RUZINO/resources/shaders/`）。

分辨率/参数在 `render_wetbrush.py` 顶部（SIM_RES 默认 4096，可用 `WETBRUSH_RES` 环境变量
覆盖；SIM_RES_Z / SPP 同理）和 `brush_sim_common.hpp`（NUM_BRISTLES）。

## 参数对照（paper Table 1 vs 当前）

| 参数 | Paper | 当前 | 备注 |
|---|---|---|---|
| Grid 分辨率 | 4096×4096×64 | 默认 4096（env 可降：1024 ~7GB / 2048 ~28GB） | 全 grid 分配，需稀疏化才能在消费级卡跑满 4096 |
| D₀ (grid→particle range) | 1 cm 固定 | brush_radius×3.0 | 单位换算，经批准 |
| D₁ (bristle adhesion) | 0.3 cm | brush_radius×0.9 | 同上 |
| γ (FLIP/PIC blend) | 0.8 | 0.8 | ✓ |
| δ (particle friction) | 1/0.2 cm | 5.0/D₀ | 单位换算后一致 |
| α (pressure solver) | 1 | 1 | ✓ |
| Bristle 数 | 40-600 | 600 | paper 上限 |
| 每 bristle sample 数 | 128 | 128 | ✓ |
| oil_density vs density | 分开的两个 field | 分开 | paper §3/§4.2/§6 明确区分，不冗余 |
