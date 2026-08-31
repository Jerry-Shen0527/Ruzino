# Wetbrush 复现：3D 笔刷绘画仿真 + 体积渲染

> 复现 Wetbrush (Chen et al., SIGGRAPH Asia 2015) 的 GPU bristle-level 3D 绘画
> 仿真与渲染。本文档记录架构演进、已完成工作、关键经验教训、遗留目标。
> 论文原文（带图转录）见 `docs/paper_wetbrush_chen2015/`；
> `docs/Wetbrush_GPU_based_3D_painting_simulation_at_the.md` 为纯文本旧版
> （图片链接已失效、公式上标有 OCR 损伤，优先用前者）。

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
    └─ RuzinoGraph zone: brush_wb_sim → commit
       （brush_wb_sim = 单 execute 三 phase：PHASE 1 笔毛/窗口/蘸墨 →
         PHASE 2 ABSORB/EMIT → PHASE 3 粒子+流体解算+§5.2 移交；§30 合并）
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
         → VolumeIntersection: 只在射线穿过 paint cell (density>0.012) 时 ReportHit
         → VolumeClosestHit: first-cross + penetration blend + Lambertian + AO
         → 空 volume 段不 ReportHit，射线继续到 Paper mesh
```

### 关键设计决策

- **全局持久 grid + 局部计算窗口**（paper §4.2）：一个覆盖整张 canvas 的大 3D grid
  是 paint 的持久存储；每帧只在笔刷周围的 active window（默认 320×320×res_z，
  `WB_WIN_XY`，§33；paper 原文 128×128×32）内计算。
  窗口只是 dispatch 范围，不 commit/clear。
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
- `node_brush_wb_sim.cpp`（2026-08-30 合并后；原 `node_brush_wb_deposit.cpp`）：`alloc_win_n3d = resolution³`（global）。
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

### 12. 渲染观感调整

- **纸面**：整张 canvas（±paper_size/2），不是笔触 bbox 外一圈 margin。配合
  VolumeIntersection 的空段透明，paint 画在纸上。
- **光照**：DistantLight intensity=3 + DomeLight intensity=0.25（冷色调填光，把背景
  从死黑 lift 出来）。
- **相机**：tight framing 笔触区域（frame_size≈0.35），3/4 俯视角。
- **gridMin Z 对齐**：marker scene 的 gridMin Z 必须和 sim registry metadata 一致
  （canvas_z=0），否则 paint 渲染位置偏移，产生光晕 + 悬浮。

### 14. 架构修正：回归 paper 的粒子驱动 paint 注入（进行中）

一次彻底的架构排查，发现原实现的 paint 注入机制**整体偏离 paper**，是白色
膨胀 + 几何变形 + 颜色斑点三个症状的共同根源。

> **状态更新（2026-08-03）**：§14 记录的「粒子不逃逸 → density=0」阻塞已解决，
> 但诊断中发现这是**多层 bug 链**的表象。完整修复见下方 §15「paint 渲染链路
> 打通」。核心结论：原阻塞（粒子被锁 D0）只是第一层；其下还有 supply 无限补给
> 导致 density 暴涨、grid_to_particle color 量纲不一致导致颜色稀释、
> setPermanentBufferState 不可逆导致渲染读到空 buffer、density 量级与渲染
> [0,1] 假设不匹配共 5 个独立 bug。逐一修复后 paint 终于稳定渲染（暗红色笔触）。

#### paper 的机制（§4.1/§4.2/§5）

- bristle rasterize 出的 density/velocity **只作 pressure projection 的边界
  条件**（§4.1 line 118 "used as boundary conditions"；§4.2 line 124
  "treated as boundary conditions in pressure projection"）。
- paint 进入 grid **只有一条路径**：bristle sample 液体过载（m_j > (1+ε)M_j，
  §5.1）→ emit 粒子 → 粒子离开笔刷 D0 范围时沉积进 grid（§5.2 Eq.16）。
- bristle **从不直接接触 grid liquid**（§5 line 218 "brush bristles are not
  in direct contact with grid-based liquid"）。

#### 原实现的三重偏差

| 偏差 | paper | 原实现 | 后果 |
|---|---|---|---|
| ① bristle 直接注入 | density 只做边界条件 | `bristle_merge` 每帧 `density[gidx] += bd` | 持续不守恒注入 → 膨胀 |
| ② emit-mode-0 与容量脱节 | emit 基于 m_j > (1+ε)M_j | 基于 ink_amount，每 sample 每帧固定 emit | 过量发射 |
| ③ merge/transfer 乒乓 | §5.2 唯一 mass 路径 | bristle_merge 每帧加 mass，grid_to_particle 又转走 | paint 永不积累 |

另外发现两个无 paper 依据的「发明」（已移除）：
- `hue_var`（per-bristle ±0.075 随机色差）：paper §5.1 的 pigment c_j 只通过
  Color Mix 变化，没有 per-bristle 色差。它在源头制造 cell-level RYB 方差，
  因 Gossett&Chen RYB→RGB 对蓝色高度非线性（blue corner (0.163,0.373,0.6)
  亮度仅 0.356），方差被放大成肉眼可见的蓝色斑点（Δ brightness 0.50，其它色
  ~0.00）。
- `tip_fade`（笔尖 RYB × 0.7-1.0）：paper 的"笔尖颜料少"通过液体载量 m_j /
  容量 M_j（§5.1 Eq.12/13）建模，不乘颜色。RYB 乘小数在 Gossett&Chen 空间
  等于"加白纸"，是概念错误。

#### 本轮改动（commit `01fce9b9`，进行中未完成）

**第 1 层 — 移除 bristle 直接注入：** `deposit_at` 里移除 bristle_merge
dispatch。bristle_rasterize 仍产出 bristle_density（作 §5.1 Eq.12 capacity ψ
+ 第 3 层边界条件），但不再 merge 进主 grid。

**第 2 层 — 启用 paper §5.1 emission，停用 emit-mode-0/1：** 移除
particle_emit 的 mode-0/mode-1 dispatch；paint 粒子唯一来源 = bristle node
PASS=1（m_j > (1+ε)M_j）。移除 bristle node 的 counter reset（改为 append，
保持粒子持久性 §4.3）。ABSORB 改为饱和吸收（capillary refill）+ 允许过载吸收
（去掉 M_j×(1+ε) clamp）。sample_liquid 初始化为饱和 m_j=M'_j（蘸满颜料的笔刷
落笔，cold-start）。

**第 3 层 — bristle density 作 pressure projection 边界（§4.2）：**
fluid_divergence/jacobi/gradient 加 bristle_density SRV，brush-occupied cell
（bristle_density > brush_boundary_gate=0.01）视为 no-flux 墙。SimConstants
加 brush_boundary_gate 字段。

**bristle_merge 改为 velocity-only 耦合：** 只写 vel_x/y/z（FLIP §4.3 速度
合并），不再写 density/color/wetness/oil（paint mass 交给 §5.2 transfer）。

#### 当前阻塞：粒子不逃逸

`particle_to_grid`（§5.2 Eq.16，paint 进 grid 的唯一路径）触发条件：
`距离笔刷 ≥ D0×1.5 且 速度 < 0.5`。

但 `particle_update` 的高附着力（Eq.10 blend，β_L=0.1，D1=brush_radius×0.9）
把粒子锁在笔刷上，粒子从不离开 D0 → particle_to_grid 从不触发 → grid density
全程 ≈ 0。frame 31 笔刷跳到 stroke 1（位置突变）时旧粒子瞬间远离才触发一次
沉积（density 跳到 216）。

paper 的粒子靠真实动力学（惯性/重力/粘度）自然脱离笔刷。

#### 下一步 TODO

1. ~~**粒子逃逸**（最高优先级）~~ **已解决（§15 bug #1）**：改用 `d_{B,k}`
   （到 bristle 距离）而非笔刷中心距离后，粒子在笔刷移开时能沉积。particle_update
   的动力学本身未改——粒子仍靠笔刷移动被动脱离，非真实动力学主动脱离（遗留）。
2. ~~**color 守恒**~~ **已解决（§15 bug #3）**：grid_to_particle emit 归一化
   RYB + 同步减 color_out。残余：plain `-=` 的 race 导致长时间循环 RYB 轻微
   漂移，pack_float4 的 clamp 兜底（遗留）。
3. **wetness/oil 来源**：去掉 bristle_merge 的 wetness/oil 写入后，grid 的
   wetness/oil 无人维护。paper §5.2 说 "other liquid particle variables, such
   as oil density and dryness, can be simply merged into the grid cells"，
   需让 particle_to_grid 也写 wetness/oil（粒子携带这些属性）。
4. **爆池控制**：cold-start 初始 mass=M'_j 时所有接触画布的 sample 同时过载
   emit（76800 samples），frame 1 瞬间打满 262144 粒子池。需控制 emit 速率
   （max_emit_per_step）或 sample 过载的时序。

#### 诊断方法（本轮验证有效）

- `wb_diag` 日志（commit node）：density / color_r / color_y / color_b /
  particles / ptcl_mass / ptcl_d_sum。density=0 但 ptcl_d_sum>0 说明 rasterize
  写了但 merge 没转进 grid（本轮定位到 merge/transfer 乒乓的关键证据）。
  color_r/density 比值持续下降说明 color 被稀释（§15 bug #3 的定位证据）。
- sentinel 测试：临时把 merge 的 density 写入改成 `=999.0`，读回 0.35 →
  证明写入被后续步骤（grid_to_particle）覆盖。
- 跳过 fluid solve：`if (false && sim_dt > 1e-6f)` 隔离 merge vs advect。
- 多色对照（render_wetbrush_color.py --ryb）：RYB→RGB 非线性使蓝色斑点最
  明显，黄/红几乎不可见——用多色对照快速定位"颜色问题 vs 通用问题"。

### 15. paint 渲染链路打通（2026-08-03）

§14 的「粒子不逃逸」诊断（`wb_diag density=0 particles=262144 ptcl_d_sum=340`）
只揭露了第一层。修复 particle_to_grid 的距离判据后 density 开始涨，但暴露出一个
**5 层 bug 链**，每修一层才看见下一层。最终全部修复后 paint 稳定渲染为暗红色
笔触（reddish ~9% 画面，RGB≈116,59,59，跨帧稳定）。

#### 5 个独立 bug 及修复

| # | 症状 | 根因 | 修复 |
|---|---|---|---|
| 1 | density=0（粒子不沉积） | `particle_to_grid` 用「距笔刷 XY 中心」判据（`d >= D0*1.5`），粒子被附着力锁在 D0 内从不满足 | 改用 paper §5.2 的 `d_{B,k}`（到最近 bristle sample 的距离）；`>= D0` 即沉积。fluid.cpp 给 particle_to_grid dispatch 绑 sample_pos |
| 2 | density 暴涨到 1000+（黑团） | deposit 每帧 refill `sample_supply = ink_amount` 给全部 76800 samples = 无限墨水源，supply→ABSORB→emit→deposit 循环净注入 | supply 改为 stroke_start 一次性补给（蘸笔模型，`ink_amount * 30` per sample），总量有限。density 现在涨到 ~450 后稳定 |
| 3 | 颜色稀释消失（color_r 卡在 ~154，color_r/density 从 0.69 降到 0.20） | `grid_to_particle` emit 粒子时 color 读 grid 的**绝对 premultiplied 值**（color_r≈154），但 particle_to_grid 把它当**归一化 RYB** 用，量纲不一致；且减 density 时不同步减 color（§14 TODO #2） | grid_to_particle emit 归一化 RYB（`color/density`）+ 同步减 color_out（按邻居 color/density 比例）；fluid.cpp 加 color_r/y/b_tmp seed + swap |
| 4 | 渲染读不到 packed_paint（bindless 失效，画面空白） | commit 节点 `setPermanentBufferState(ShaderResource)` **不可逆**；第二帧 pack dispatch 需要 UAV (0x80) 但 permanent state 已是 SRV (0x60)，nvrhi 报错并**跳过 pack 写入**，packed_paint 保持 frame-1 的全 0 | 移除 setPermanentBufferState，依赖 packed_paint 的 `keepInitialState=true` + nvrhi 自动 UAV↔SRV barrier（跨 command list 靠 waitForIdle 同步） |
| 5 | density 量级与渲染 [0,1] 假设不匹配（sim 单 cell density ~0.002，渲染 kDensitySurface=0.02；AO `min(d,1)` 把大值截断驱 (1-ao)→0 渲染纯黑） | pack_float4 直接打包 raw density + premultiplied RYB，渲染当 RGB 用且无 RYB→RGB 转换 | pack_float4 归一化 density（`clamp(d/0.05, 0, 1)`）+ RYB→RGB 转换（`ryb_to_rgb`）+ clamp RYB 到 [0,1] 防 grid↔particle 循环的 race 漂移 |

#### 关键诊断手段（本轮验证有效）

- **分层隔离**：每修一层 bug，用 `wb_diag`（density/color_r/color_y/color_b/
  particles/ptcl_mass/ptcl_d_sum）确认该层修好，再看下一层症状。一次全改会
  淹没因果。
- **packed_paint readback**：临时在 commit 节点 readback packed_paint 打印
  first-nonzero voxel 的 (d, rgb, raw d, raw cr)，确认 pack shader 实际写入了
  什么。定位 bug #4（pack 被跳过，packed 全 0）和 bug #5（density=0.002 远小于
  渲染阈值 0.02）的关键证据。
- **PIL 像素分析**：`reddish = (R>30) & (R>B+12) & (lum<240)` 量化渲染输出，
  跨帧对比 RGB 均值判断颜色是否稳定（clamp 前 frame_0040 漂白，clamp 后稳定）。

#### 遗留问题（非阻塞）

- **颜色偏暗**（RGB 116,59,59 而非鲜红）：AO `min(d,1)` + Lambertian shade
  (0.4+0.6*facing) 压暗。paper §6 的 AO 是 crevice darkening，当前对平坦笔触
  也偏强。可调 AO ray 数或 shade 的 ambient floor。
- **粒子脱离依赖笔刷移动**：粒子仍被 Eq.10 附着力锁定，靠笔刷移开后 d_{B,k}>D0
  才沉积（bug #1 的修复让这成为可能，但没改 particle_update 的动力学）。paper
  的粒子靠真实动力学自然脱离；自动 stroke 下笔刷持续移动所以能工作，静止笔刷
  下 paint 会累积在 D0 边界。
- **color 守恒不严格**：grid_to_particle 的 plain `-=` 在 RWStructuredBuffer 上
  race（Slang CAS 不支持 structured-buffer 元素），长时间循环后 RYB 可能漂移。
  pack_float4 的 clamp 兜底防极端值，但根因（无 atomic）未解决。

#### 参数校准（2026-08-03，修正「paint 一大坨散开」反馈）

§15 修完后 paint 能显示但**散布成一大坨而非笔触**。根因：emit 半径 R_j 远大于
brush_radius，粒子出生即逸散到笔刷 footprint 外沉积。

R_j = cbrt(3·M_max/(4π·ρ₀))。ρ₀=1e3（paper 的 SI paint 密度）在归一化 sim 单位
（paper_size=1）下让 R_j 随 M_max 快速膨胀：

| M_max | R_j | 症状 |
|---|---|---|
| 2.0（初值） | 0.078 | 粒子出生即沉积（>D0=0.06），paint 散布全屏成 blob |
| 0.5 | 0.049 | 仍宽，paint bbox 46 cell（≈0.18 世界单位，brush 的 9 倍） |
| **0.03**（现值） | **0.02** | R_j≈brush_radius，paint bbox 17 cell（≈0.066，brush 的 3 倍），形成横向笔触 |

配套：D0 从 brush_radius×3(0.06) 降到 ×1.5(0.03)；supply dip_charge = M_max·ink·30
（ABSORB 每帧受 M_j 节流，supply 大不会爆发，只延长笔触持续时间）。结果：frame 2
不再爆发（particles 受控），paint 沿笔刷轨迹形成笔触（dense paint span 横向 > 纵向），
无 grid/checkerboard 纹理。

**教训**：paper 的参数（ρ₀=1e3 kg/m³, D0=1cm）是 SI 单位，sim 用归一化单位
（paper_size=1, brush_radius=0.02）时必须重新校准让 R_j ≈ brush_radius，否则 emit
范围失控。这是个单位匹配陷阱，不是逻辑 bug。

#### dry-start 冷启动修复（2026-08-03，修正「落笔处一大坨」反馈）

参数校准后笔触成形，但**落笔点（stroke 起点）仍有一大坨高密度堆积**。用户描述
「笔墨在空中就已经过饱和然后沉积了并且不再被流体仿真了，维持这个高高的一坨」。

根因：sample_liquid 初始化为饱和（m_j = M_max）。frame 1 的 ABSORB 路径
（bristle_liquid_transfer.slang）对饱和 sample 继续从 supply 吸收（过载 uptake，
无 M_j×(1+ε) clamp），把所有接触画布的 sample 推过 (1+ε)M_j → 它们**同一帧全部 EMIT**
→ 粒子在落笔点立即沉积（笔刷静止，速度场≈0）→ 无 scalar diffusion（见下）→
paint 维持堆积。

修复：sample_liquid 初始化改为 **dry（m_j = 0）**。frame 1 ABSORB 只填到 M_j（饱和
但不过载，EMIT no-op）；frame 2 起才过载 emit，此时笔刷已开始移动，首笔 deposit
落在笔触上而非堆在起点。pigment c_j 仍初始化为 ink color 供 color_mix 使用。
结果：frame 0-1 density=0（之前 frame 1 就一大坨），emit 从 frame 2 渐进开始，
落笔点堆积明显减轻（1024×1024×64 下可观察到连贯的横向笔触）。

#### 当前遗留的观感问题（下一轮打磨）

1. **落笔点仍有残余堆积**：dry-start 减轻了爆发，但笔刷在 stroke 起点停留期间
   （速度从 0 加速）仍累积 deposit。paper 靠真实笔刷动力学，自动 stroke 的起点
   停留是测试脚本的特性。
2. **粒子立即沉积（particles 全程=0）**：emit 的粒子在笔刷附近一出生就被
   particle_to_grid 沉积，没有以粒子形式跟随笔刷移动沿途分布。混合表示的粒子相
   寿命过短。可能需要让粒子在 D0 内存活更久。
3. **paint 沉积后不铺开**：fluid solve 对 density 有 advect（靠速度场），但**显式
   移除了 scalar diffusion**（fluid.cpp:515，之前加 diffusion 侵蚀笔触边缘）。所以
   静止区 paint 维持堆积不扩散。paper 高粘度 paint 确实少扩散，但落笔点需要某种
   铺展机制。
4. **颜色偏暗**（RGB≈140,40 而非鲜红）：渲染 AO `min(d,1)` + Lambertian shade
   (0.4+0.6·facing) 压暗。

### 16. 粒子相修复：compact 复活 bug + 笔刷携带 + 发射预算（2026-08-18）

用户在 debug 可视化（`render_wetbrush_debug.py`）里看到「粒子太大、静止不动、
规则排列在格点上」。排查发现三层叠加根因，全部修复：

#### 1. `particle_compact.slang` ping-pong 复活 bug（质量铸造）

compact 只给打包后的 `[0, live)` 槽写 `alive=1`，死线程在自己旧 idx 写 0，
但输出缓冲 `[live, max)` 区间**保留 ping-pong 上一轮的 alive=1 + 旧位置**。
后果：

- **已沉积粒子每帧原地复活**：update 全量写 alive（含已沉积者），to_grid 杀
  死它们，但 compact 输出的另一块缓冲仍是 update 的旧标志 → 下一帧复活，
  d_B≥D0 且慢 → **每帧再沉积一次同样的质量**，直到 far+stalled（d_B>2·D0
  且 speed<0.01）才被 update 杀掉（速度衰减 ~5/s，≈40 帧 → 单粒子质量放大
  ~40×）。density 爬到 ~600 的一部分就是它。
- **幸存者随机丢失**：死粒子 idx 落在 `[0, live)` 内时，它的 `alive_out[idx]=0`
  会把打包到该槽的幸存者清死。

修复：fluid 节点在 compact 前**先把 ptcl_alive_b 清零**（field_clear，
262k 线程），compact 死线程不写任何东西（只写打包幸存者）。

#### 2. 附着力恢复：笔刷携带粒子（paper §1 / Eq.10）

之前调参把 D1 压到 `0.9×brush_radius < R_j(≈0.97×)`，Eq.10 出生 blend=0，
笔刷什么都不携带 —— 与 paper §1 "Artists use the brush to carry paint to
different locations fast" 相悖，也看不到随刷流动的粒子群。现值（fluid.cpp
统一为一个定义）：

| 参数 | 旧值 | 新值 | 理由 |
|---|---|---|---|
| D0 | brush×1.5 | brush×2.0 | D0>D1 恒成立；沉积区在笔刷后缘连续铺 trail |
| D1 | brush×0.9 | brush×1.6 | 必须 > R_j(0.011-0.019)，出生 blend≈0.4-0.6，粒子群随刷移动 |

#### 3. EMIT 全局出生预算 + ABSORB 上限（防爆池）

supply drip 让全部 76800 sample 每帧过载发射（wb_diag: +70-80k/frame，262k
池直接 wrap）。paper §5.1 只有 per-sample 上限（max_emit_per_step=10），单
sample 1 粒/帧 × 76800 仍是洪流。加：

- `BristleLiquidConstants::emit_budget`（默认 12288，env `WB_EMIT_BUDGET`，
  0=不限）：EMIT pass 用 `emit_budget` buffer（bristle 节点每帧清零）原子
  预约名额，超预算的 sample **保留负载**（质量不丢，下一帧再试）。
- ABSORB 过载吸收加上限 `(1+ε)M_j + PER_PARTICLE_FLOOR`：预算阻塞时 m_j 停
  在上限，不会气球化后在预算放开时倾泻。

#### 4. 格点排列的最后一层：grid_to_particle jitter（同轮，commit 前）

§5.2 的 27 候选分层采样固定在 3×3×3 子格点中心 → 转换出的粒子呈完美子格
点（debug 渲染直接暴露）。已在每个 stratum 内加 hash jitter（真正的分层采
样定义）。

#### 5. Debug 可视化半径

三个点云半径全部 env 化并调小（cell 单位）：
`WB_DEBUG_PTCL_R`(0.25→0.2)、`WB_DEBUG_VOXEL_R`(0.55→0.3)、
`WB_DEBUG_BRISTLE_R`(0.75→0.45)。0.55-cell 的 voxel 本来就会互相叠成板，
在数万个点重叠后读作「巨大的粒子」。

#### 验证方法

`render_wetbrush_debug.py` + `wb_diag`/`wb_debug_draw` 日志：粒子数应稳定
在池内（不再每帧 +70k wrap）、density 单调增长（无铸造）、画面上应有随笔刷
移动的粒子群 + 笔刷身后连续沉积的 voxel trail。

#### 6. Pen-up 门控（同日第二轮，修复"轨迹耗尽后全画面冻结 + 池只进不出"）

首轮验证暴露两个问题：

- **测试夹具陷阱**：`render_wetbrush_debug.py` 用的是单笔画节点
  `mock_stroke`（30 点），45/60 帧的 run 中途轨迹耗尽 → emitter 抬笔
  （active=false）→ deposit/fluid 跳过 → 笔刷、鬃毛、粒子全部冻结。这不是
  仿真 bug；夹具已改为 `Num Points = NUM_FRAMES`。诊断手段：给 emitter 加
  逐帧日志（active/cursor/stroke），一眼看出 cursor 停走。
- **pen-up 时 §5.1 仍在发射**：bristle 节点不接收 BrushPoint，pen-up 期间
  仍 ABSORB+EMIT（+12288/帧），而 fluid 的 particle maintenance 虽无门控、
  但粒子都在停滞笔刷的 D0 内不沉积 → 池单方向填充直到 wrap。

修复：`WetbrushSimState::pen_down`（deposit 每帧从 bp.active 写入，早退
之前）；bristle 节点 pen-up 跳过 ABSORB/EMIT（笔刷动力学在 deposit 节点，
不受影响）；`ParticleConstants::pen_down`——pen-up 时
particle_to_grid 忽略 d_{B,k}（抬起笔刷即离开一切 bristle，携带的粒子群整
体沉降）、grid_to_particle 暂停转换。物理含义：pen-up = 笔刷不在纸上。

### 17. 正式渲染管线修复：笔刷末端断裂 + 笔画中部持续膨胀（2026-08-20）

用户对 `wetbrush_sequence`（正式 volume 渲染，非 debug 点云）报告两个症状：
笔刷末端笔画断开、笔画中部一直在膨胀。逐层定位后共修复六个问题：

#### 1. 渲染合成缺失粒子群 → 笔刷周围"护城河"（断裂主因）

`grid_to_particle` 每帧把笔刷 D0 范围内的网格密度抽成粒子，但渲染 volume
只读网格密度场（`pack_float4.slang`）→ 笔刷周围一圈既无网格颜料（被抽走）
也无渲染（粒子不可见）→ 笔画在笔刷处断开。论文 §6 渲染的活动窗口液体本来就
是粒子+网格合成。修复：`pack_float4.slang` 把 fluid 节点每帧光栅化的
`ptcl_density`/`ptcl_rast_*`（窗口 sized）按窗口映射叠加进渲染场
（`SimConstants::ptcl_render_scale`，env `WB_RENDER_PTCL`，0=关闭）；
pen-up 帧 fluid 节点清空这些光栅缓冲（避免幽灵粒子群）。

#### 2. `bristle_merge` 二次方动量注入 → 中部膨胀主因

合并公式 `vel += bvx * bd`，`bvx` 已是质量加权动量和，再乘 `bd`（质量和）→
速度随粒子密度二次增长，注入的速度场在活动窗口里持续半拉格朗日平流密度场
（res 1024 下每帧几十 cell 回溯的模糊反复叠加）→ 笔画越搅越宽。修复：改成
朝质量加权平均速度弛豫：`vel += (bvx/bd - vel) * min(bd·scale, 1)`，
`scale` = `WB_VEL_INJECT`（默认 1.0）。

#### 3. 速度无衰减 → 注入速度 600 帧不散

`fluid_damp_dry` 只在 wetness<0.01 时清零速度（drying_rate 0.1 下要 ~600
帧）。新增每帧速度衰减 `velocity_damp`（`WB_VEL_DAMP`，默认 0.8/帧，host
换算成 `pow(frame, 1/substeps)` 保持子步无关）。笔刷过后 ~5-10 帧流体静止，
与论文 §4.2 "dry 后忽略速度"语义一致。

#### 4. Eq.16 的 dV 尺度灾难 → 沉积密度比阈值低 4 个数量级

`ρ_c += m·W·dV` 在 res 1024 下 dV≈9.3e-10，单粒子沉积 ~4e-6，而渲染阈值
(raw)≈0.006 → 笔画呈一串勉强过阈值的疙瘩。之前"看起来能用"完全靠 compact
复活 bug 凭空造质量。修复：p2g/g2p 内核改**离散归一化**（Σw=1，去掉 dV），
粒子质量=密度单位，round-trip 严格守恒；g2p 的 interp 门（0.02）也因此才能
在密度单位下正确触发（之前 W·dV 形式下门永远不触发）。

#### 5. 吸附层/转换层比例失衡 → 粒子群不退休 + 笔刷"喝掉"自己喷的颜料

- ρ₀=1e3 使 R_j≈0.95×brush_radius，被迫 D1=1.6R → D1/D0=0.8（论文 0.3），
  粒子群整个随笔刷走，泄漏率仅 ~3%/帧。ρ₀=2e4 → R_j≈0.38R，D1=0.5R。
- grid_to_particle 抽干范围从 D0 收窄到 D1：D1..D0 环带保留旧沉积（否则
  环带既无网格也无粒子，重现护城河）。
- EMIT 预算原子竞争被 dispatch 顺序前 ~80 个样本每帧垄断 → 颜料从笔刷一个
  点喷出 → 沿途成串。加概率预门（brush_pos 做逐帧熵，`BristleLiquidConstants`
  新增 brush_pos_x/y/z），竞争者均匀分布。
- 预算按沉积率标定：`WB_EMIT_BUDGET` 12288→800（ABSORB 回收 ~70%，目标
  trail 密度 ~0.2/格）。

#### 6. 渲染首穿阈值偏高 → 薄沉积段不可见

`kDensitySurface` 0.02→0.012：快轨迹段的薄沉积刚好低于阈值，笔画裂成
3-4 段；降低后最大连通域 4.3k→18.1k px，残余缝隙 ≤6px（≈1.7 格，3px 膨胀
即合并为单一连通域）——沉积核之间的插值凹陷级别。

另外：`render_wetbrush.py` 夹具 Num Points 30→NUM_FRAMES（同 §16.6 的轨迹
耗尽问题），并清理陈旧 `_modifiers.usdc` sidecar（首帧 tick 抛
`json.exception.type_error.302` 的间歇性元凶——旧参数表 socket 值为 null）。

#### 验证

`render_wetbrush.py`（WETBRUSH_RES=1024，60 帧）：笔画从起笔连续到笔刷、
无孤立色块、无中部膨胀（厚度 5.7→~40px 稳定）；`test_wetbrush_zone.py`
3 passed；`wb_diag`：density 单调增长至 ~231、粒子 ~5.7k 稳定（≈800/帧 ×
7 帧寿命）、无 NaN。可调项：`WB_VEL_DAMP`(0.8)、`WB_VEL_INJECT`(1.0)、
`WB_EMIT_BUDGET`(800)、`WB_RENDER_PTCL`(1.0)。

### 18. 回归论文本意：双模式渲染 + 粒子密度 + 活动窗口内存模型（2026-08-23）

用户对 §17 的结果指出两处残留（活动窗口内颜色更深的圆盘、窗口外颗粒感
+ 偏窄），并要求对照 paper 原文审查 §17 的操作哪些是 hack。通读
`docs/Wetbrush_GPU_based_3D_painting_simulation_at_the.md` 后的结论与重构：

#### Hack 审查结论（对照 paper 原文）

1. **pack 粒子群合成（§17.1）→ 撤销**。Paper §6 明确用两套渲染：体积
   raymarch 只采样网格密度场，粒子用独立的屏幕空间方法（van der Laan
   2009），且论文直言两模式在笔刷附近会有不连续、"noticeable only in
   closeup views"——论文接受它。我们的密度合成把窗口内密度叠高 → §6 AO
   饱和 → 深色圆盘。撤销后深色圆盘消失；活粒子通过
   `wetbrush_debug_particles` 点精灵渲染进正式序列（`/LiquidParticles`
   prim，`WB_DRAW_PARTICLES=0` 关闭；笔刷本体 `/BrushBristles`，
   `WB_DRAW_BRISTLES=0` 关闭）。
2. **D1 抽干（§17.5 的一部分）→ 撤销**。Paper §5.2 就是按 D0 转换；
   护城河区域按论文本该是粒子（由点精灵渲染覆盖）。
3. **WB_VEL_DAMP=0.8 → 默认 1.0（关闭）**。论文消除 trailing velocity 的
   机制是 §4.2 强粘性扩散（把动量稀释到窗口均值）+ dryness 阈值清零 +
   有界的 §4.3 合并（二次方动量 bug 修复后）。env 保留作极端工况逃生门。
   （勘误：§17.3 曾称粘性 Jacobi 是数值 no-op——推导错了，
   `x'=(x+a·Σ₆)/(1+6a)` 在 a→∞ 时是纯邻居平均即最大平滑，a≈500 是强扩散。）
4. **Eq.15/16 离散归一化 → 确认为论文本意**。论文原文
   `ρ_c ± m_k·ΣW(p_k−x_c,h)` 本来就没有 dV——W 是纯权重核，旧代码的
   W·dV 才是误读。
5. **粒子密度 → 按论文量级**。论文 §7："This requires particles to be
   densely sampled... otherwise dirty color mixing or noisy surface
   artifacts may appear"——正是我们的颗粒感。`WB_EMIT_BUDGET`
   800→8192/帧（论文典型 210K 粒子、大笔刷 2M）。颗粒感随之消失。

#### 活动窗口内存模型（paper §4.2 "128×128×32"）

用户发现显存爆炸（进程 ~9.4GB）。根因：25 个场全部按全局网格稠密分配
（1024²×64 下每个 268MB）。论文 12GB TITAN X 跑 4096 的唯一可能是：
求解/临时场只存在于活动窗口（论文原文 "we can restrict grid-based
simulation to a small active window"，128×128×32 ≈ 每场 2MB），画布状态
在 4096 下稠密 float 也要 25.8GB——论文必然用了稀疏/量化存储（原文未写明，
算术上别无可能）。

重构（仿真侧 ~8GB → ~3.7GB）：
- **窗口尺寸化 18 个瞬态场**：vel×3 + tmp×3 + vel_old×3 + pressure×2 +
  divergence + density/color×3/wetness/oil 的 tmp。画布状态（density、
  color×3、wetness、oil）保持全局稠密；`height_field` 是死代码不再分配。
- **窗口滚动**（`window_scroll.slang`）：窗口随笔刷移动时把 vel×3 +
  pressure_a（warm-start）的重叠区重定基，窗口外丢弃（论文：窗口外无液体
  运动）。scratch 复用 vel_tmp / pressure_b / divergence_buf。
- **两套索引空间的桥**（`field_copy_window.slang` 三模式）：vel 快照
  window→window；g2p 的 Eq.15 扣减种子 global→window、写回 window→global
  （替代旧的整场 copyBuffer 种子 + swap）；标量平流 window→global 写回。
- **求解器窗口局部索引**：jacobi/divergence/gradient 的瞬态缓冲走
  `wnb()`（窗口边缘取该 shader 的边界规则：散度=墙、梯度=镜像、扩散=镜像），
  BC 场（wetness/density）保持全局索引；advect 的速度回溯采样走
  `win_field_idx`（窗口外=0）；particle_update / particle_flip_pic /
  bristle_simulate 的三线性速度采样同样窗口化；bristle_merge 的速度写入
  改窗口索引（顺带发现：它之前用全局 gidx 写——若漏改会越界）。
- **damp_dry 拆分**：全局 pass 只做 wetness 干燥（论文"every grid cell"），
  窗口 pass 做速度衰减/dry 清零（速度只在窗口存在）。
- 顺带：Slang 不支持 C++ lambda（`auto f=[&](...)` 语法错误）——全部改
  普通函数；改 shader 后先用 `slangc -target dxil` 预检全部 21 个再跑，
  省渲染迭代。

#### 验证（run11，60 帧完整）

- **显存**：进程 ~3.7GB（原 ~9.4GB）；渲染 4s/帧（原 60-90s，VRAM 压力
  消失）。
- **连续性（像素级）**：frames 35/50/55/59 全部**单一连通域**（60k-106k
  px，>99% 红像素），历史首次完全连续；厚度 44-48px 稳定，轨迹结点区
  82-98px 有界（帧间不再增长，runaway 膨胀消除）。
- **深色圆盘**：消失（视觉确认颜色均匀）。
- **颗粒感**：消失（论文密度粒子）。
- `test_wetbrush_zone.py` 3 passed。
- 已知：视觉模型对最终帧仍报"断块+中部膨胀"，与像素连通域分析矛盾，
  以像素为准（该模型在同类图上多次过度解读）。

### 19. 统一颜料渲染 + 流体速度场"死场"三连修（2026-08-23）

用户反馈两个问题：① 渲染时 active window 内外用了两种"颜料"画法
（内=粒子 sprite，外=格子 raymarch），要求统一为格子系统、粒子仅
按需可视化；② window 内的粒子帧与帧之间完全不动，流体行为缺失。

#### 诊断手段：WB_DEBUG_DUMP_PTCL=1

commit 节点新增逐帧 readback 统计（粒子数/质心/bbox/vmean/vmax +
窗口速度场 gridvmax），一行日志看清粒子是否真的在动。**第一轮输出
直接定案：gridvmax ≡ 0.00000（每帧、精确为零）**——不是"渲染冻结"，
是整个欧拉速度场从第一帧起就是死的；粒子 vmean ~0.003-0.014 只是
发射点随笔刷移动的假象。

#### 根因 1：fluid_damp_dry 的 dry-zero 缺 density 门

`wetness` 只在粒子 **deposit** 时写 1（particle_to_grid）。刷子底下
质量都在粒子里、没有 deposit → wetness=0 → damp 的窗口 pass 把
`wetness < 0.01` 的格子速度**清零**——swarm merge 注入的速度每个
substep 都被抹掉。paper §4.2 说的是"**干掉的颜料**格子忽略其速度"，
不是"空格子"。修复：dry-zero 加 `density > 1e-6` 门（与 divergence/
gradient 的 is_solid 谓词一致——空格子里的速度场是流动液体本身，
必须存活）。

#### 根因 2：刷毛边界是"移动墙"，不是"静止墙"（§4.2 本义）

paper §4.1: "we rasterize them into a density field **and a velocity
field**. They will be used as boundary conditions"；§4.2: "the
discretized bristle density **and velocity** fields are treated as
boundary conditions in pressure projection"。我们的实现只 rasterize 了
bristle_vel_* 却从不消费——gradient 把刷毛格速度硬置 0（静止墩子），
divergence 直接跳过刷毛邻居（无通量）。修复：

- **fluid_gradient**：刷毛格速度 = raster 平均刷毛速度
  （bristle_vel/bd，质量加权），不再是 0；
- **fluid_divergence**：朝向刷毛格的面速度 = 该格壁面速度（移动墙
  源项），干颜料格维持跳过（真静止墙）。

这是 paper 让笔刷"推动"液体的原生机制。

#### 根因 3：particle_update 局部标架的平移抵消（Eq.9/10 两步法）

Step A 原实现用**当前**采样点位置做正逆变换的原点——进去出来抵消，
笔刷平移永远不会携带粒子（adhesion 名存实亡）。paper 的两步法：t 时刻
用采样点**旧**位置（origin − v_frame·dt）转进去，t+dt 用**新**位置转
出来。修复：v_frame = brush velocity（ParticleConstants 新增
brush_vel_*，取 field->prev_brush_vel，deposit 帧末维护）；v_L 改为
**相对**速度（vel − v_frame），转出时加回。

#### 统一渲染（有意偏离 paper §6）

paper §6 双模式渲染（grid raymarch + 屏幕空间粒子）自带笔刷周围的
模式接缝，paper 自己承认"noticeable in closeup views"。按用户指令改为
**单一格子渲染**：pack_float4 把每帧的粒子 raster（ptcl_density/
ptcl_rast_*，particle_rasterize 为 §4.3 联合速度场生产的 transient，
质量加权 Σw=1）复合进渲染场。质量自洽：grid_to_particle (Eq.15) 减掉
的正是 raster 加回来的，总量守恒（之前的"深色圆盘"bug 是 grid 未
排水时叠加了 splat——§5.2 排水后复合恰是逆运算）。配套：

- 粒子 sprite 改为 **opt-in**（WB_DRAW_PARTICLES=1，默认 0），
  仅供粒子状态可视化；
- 抬笔帧（!bp.active）fluid 清零 4 个 raster 格，防止幽灵复合；
- deposit 分配 raster 格时一次性清零（GPU 堆内存不清零，否则第 1 帧
  pack 复合的是垃圾噪声）。

#### 修复后实测（zone test, WETBRUSH_RES=1024）

| 指标 | 修复前 | 修复后 |
|------|--------|--------|
| gridvmax（窗口速度场峰值） | **0.00000**（恒零） | 2.4-9.3 |
| 粒子 vmean | 0.003-0.014 | **0.45-1.09**（≈笔刷速度，70×） |
| 粒子 z 范围 | 全钉在纸面 z=0 | 0-0.04（被流场卷起） |
| 笔抬后（f13-14） | 全零 | 残余 gridvmax 2.4（动量保持，§4.3 原文行为） |

#### 渲染验证（run13，60 帧，粒子 sprite 关闭 + 统一渲染）

- **统一渲染生效**：全程无粒子圆点、无窗口圆盘接缝；笔画在 window
  内外以同一体积 raymarch 呈现（frame 50 剖面：主体 50-60px 连续带）。
- **流体行为可见**：笔刷后方出现拖尾/涡痕；粒子群 3.7 万、vmean
  0.39-0.53、z 被卷到 0.03-0.04；笔移动时质心跟随。
- **粘性迭代**：初版（1 次 Jacobi 扫掠）局部速度尖峰 ~10× 笔速，把
  轨迹撕成带状碎块；按 paper Algorithm 1 的迭代精神加到 3 次扫掠后，
  f20/f35 主连通域 93-94%。
- `test_wetbrush_zone.py` 3 passed（修复后复跑）。

### 20. 头部囤积 vs 笔画饥饿：质量平衡三连调（2026-08-23）

run13 的用户反馈：window 外"薄薄一层"、window 内"超级大的一坨"。
新增 `[wb-mass]` 逐帧诊断（grid_tot / grid_max / cells_vis / swarm /
rastmax）量化后定位，三次实验排除两个错误假设：

1. **绝对速度门槛（`speed > 0.5` 不沉积）**：流场活了以后粒子随流
   速度恰好 0.3-0.5 卡在门槛上——f12 时 grid 0.4 vs swarm 413，
   前半段笔画完全没沉积。改成 §5.2 "moves slowly" 的**相对流场**
   读法（|v − u_grid| < 0.1，随流被动输运 = 格子液体）+ **落地即沉积**
   （距纸面 <2 格 = 已落地的液体属于密度场，只有真正的空中喷雾留
   粒子态）。注意纯相对速度门槛有个反直觉坑：粒子甩出 D0 后带着
   笔速冲进更慢的尾流，读作"独立运动"反而不沉积——必须配落地规则。
2. **bristle_merge 正反馈假设（WB_VEL_INJECT=0.05 对照）**：排除，
   数字不动。尾流能量来自 moving-wall 注入本身，不是 merge 回灌。
3. **WB_VEL_DAMP（0.85 / 0.6 扫描）**：0.85 略优（cells_vis +50%），
   0.6 反而更差（沉积也变慢），与 D0 收窄叠加后不增益——最终保持
   默认 1.0（关）。

**根因是 D0 壳的骑行时间**：粒子在 D0 区内随笔刷同速移动（adhesion
携带 + 尾流拖拽），只有比笔慢的少数粒子穿出 d_B = D0 沉积；骑行
时间 ≈ D0/(v_brush − v_swarm) ≈ 40+ 帧。头部囤积 = 发射通量 × 骑行
时间（f60：swarm 821 vs grid 443，头部质量 2 倍于笔画且密度 10×）。
paper 的头部:笔画质量比在长笔画下自然趋小（trail 累计时间 ≫ 骑行
时间），60 帧短测试里必须压骑行。

**修复：D0 = 2R → 1R**（`node_brush_wb_fluid.cpp`）。d_{B,k} 是到
sample 的距离，sample 本身铺到 R，所以 2R 壳 = 中心 3R 半径的区域
（paper D0/R≈1.8 也是从刷毛量起，但我们的 40 帧骑行实测过长）。
收窄后区域仍完整包住笔刷（中心 2R），骑行减半、沉积通量翻倍：

| 指标 (f60) | D0=2R | D0=1R |
|---|---|---|
| grid_tot / swarm | 443 / 821（头 2× 笔画） | **899 / 314（笔画 3× 头）** |
| f12 早期沉积 | 0.5 | **47**（立即开始） |
| grid_max / cells_vis | 0.028 / 10.5k | **0.058 / 17.5k** |
| 主连通域 (f59) | 65% | **89.6%**（113k px） |

f20/35 主连通域 ~89%，f50 在轨迹急转弯处仍有 78%（转弯撕裂是活流体
的物理表现）。头部现在是适度的"湿头"（密度仍高于笔画——真实笔刷
携带的湿颜料本就如此），笔画主体厚实连续。

### 21. Paper-fidelity 对齐轮：真实 D₀/D₁ + 原子守恒 + 蘸墨即初始化 + 论文渲染（2026-08-24）

对 §15-§20 遗留问题的逐条 paper 审查后，本轮把四处偏离论文的实现拉回论文本意。
范围决策（用户批准）：#1-#4 实施；#5 稀疏化改为量化分析（见下）；#6 压感只查论文定义。

#### 1. 粒子沉积回归 §5.2 字面判据 + 论文 D₀/D₁

- **D₀ = 1.8R、D₁ = 0.55R**（`node_brush_wb_fluid.cpp`）。论文 Table 1 是 SI 绝对值
  （D₀=1cm、D₁=0.3cm，笔刷半径 ≈0.55cm），换算成相对值即 1.8R/0.55R，
  D₁/D₀ = 0.3 与论文一致。§17/§20 的 1.0R/0.5R 是针对 60 帧短测试调的骑行时间，
  本轮按论文恢复。R_j ≈ 0.36R（ρ₀=2e4）仍 < D₁，新生儿照样骑行。
- **`particle_to_grid` 沉积条件回归字面读法**：`d_{B,k} ≥ D₀ 且 |v| <
  slow_deposit_speed(0.1)`（绝对速度，≈ 真实 2cm/s 换算）。删掉自创的
  「相对流场速度」和「落地即沉积」两条引申判据。能这么做的依据：§19 修复后
  速度场是活的，且 §4.2 强粘性扩散（α=dt·visc·N²，3 扫掠）会把窗口动量
  稀释到窗口均值——笔刷身后的尾流自然减速，粒子经 FLIP/PIC（γ=0.8 偏 PIC）
  跟随减速、过阈沉积。
- **删除 far+stalled 击杀**（`particle_update.slang`）：旧代码把 d_B>2D₀ 且
  慢的粒子直接 `alive=0`——**质量凭空销毁**，是隐藏的守恒漏洞（§16.1 时代
  为回收池子加的）。现在这类粒子正是 §5.2 的沉积对象，字面判据天然覆盖。
- `grid_to_particle` 转换半径从「笔刷中心 D₀」修正为 **R + D₀**（"距任一
  sample < D₀"的正确中心距上界——samples 铺到半径 R）。

#### 2. Eq.15/16 原子守恒（CAS 循环）

"Slang 不支持 CAS"只对 structured-buffer 元素成立；`common.slangh` 里
`atomicFloatAdd`（RWByteAddressBuffer 上的 InterlockedCompareExchange 循环）
一直是通的。本轮把三处裸 `+=`/`-=` 全部改走原子路径：

- `particle_to_grid`：density/color×3 沉积 `atomicFloatAdd`，wetness 用
  `atomicFloatMax`（新增 helper）。
- `grid_to_particle`：Eq.15 扣减 + 非负 clamp 全原子化。
- `particle_rasterize`：7 个通道的光栅累加原子化（笔刷下 swarm 密集，
  碰撞不是"罕见"而是常态）。

配套：`brush_create_field_buffer` 加 `CanHaveRawViews`（raw UAV 绑定的前提）；
两处粒子槽位 `% max_particles` wrap 改为「池满即放弃出生、质量留在源头」
（wrap 会覆盖活粒子 = 静默质量销毁）。pack_float4 的 RYB clamp 从"兜底"
降级为"防御性"（race 根因已除）。

#### 3. 蘸墨 = 初始化（§5.1 本意）

论文把蘸满的笔当作**初始状态**：m_j 起笔饱和、中途不补。现在
`stroke_start` 时一次性写 `m_j = M_max`（Eq.12 在 ψ≈0 未受压状态下的容量，
即"蘸满"的定义）+ 当前墨色。颜料流出的唯一通道是 EMIT——由**容量下降**
驱动：笔压到纸上 → r_j 变小（Eq.13 球冠削顶）+ 刷毛挤压（Eq.12 ψ 项）
→ m_j > (1+ε)M_j → 发射。**压力控制出墨量是容量模型的涌现行为**，
不是脚本。删除：逐帧 supply 滴灌 buffer（论文外发明，历史上是质量铸造
源头）、全局 emit_budget 默认值（论文只有 per-sample 上限；env
`WB_EMIT_BUDGET` 保留为逃生门，默认 0=关）。落笔点的墨团是论文本身的
行为（蘸满的笔按下就该出一坨）。ABSORB 保留颜色渗透（contact-weighted
grid-bleed，不再依赖 supply），论文的 Eq.14 粒子吸收（质量回到笔刷）
**仍未实现**——这是 §5.1 剩余的最大缺口，也是 wet-in-wet 蘸色的完整形态。

#### 4. 渲染对齐论文 §6：颜色 = penetration blend × (1−AO)

删掉自创的 Lambertian `0.4+0.6·facing` 和 wet-gloss Blinn-Phong 高光
（论文 §6 只有 first-cross → 梯度法线（用于 AO 半球）→ penetration
blend → AO 压暗，没有 BRDF）。预期效果：**变亮**（去掉 ×0.4~1.0 的
shade 因子）、更平/哑光（论文本来的样子）。

#### 5. 4096 内存：量化分析（替代稀疏化假设）

用户假设：论文未必用了稀疏存储，可能是**量化**——不是所有场都需要
全精度，也不是所有场都需要全局。分析支持这个方向：

当前全局场（`node_brush_wb_deposit.cpp`）只有 6 个画布状态 + packed_paint；
瞬态场已窗口化（§18）。4096²×64 = 1.07e9 cell：

| 场 | 现格式 | 值域（实测，§17.4/pack 注释） | 可量化到 |
|---|---|---|---|
| density | f32 | 0.001–0.285（中位 0.041） | **fp16 (2B)** |
| color×3 | f32×3 | 预乘颜料，量级同 density | **fp16×3 (6B)**，或存归一化 c/ρ 后 UNORM8×3 (3B)* |
| wetness | f32 | [0,1] 单调衰减 | **UNORM8 (1B)** |
| oil_density | f32 | [0,1] 松弛到 base | **UNORM8 (1B)** |
| packed_paint | f32×4 | 渲染就绪值全在 [0,1] | **UNORM8×4 (4B)** |

\* 存归一化颜料（而非预乘）还有一个结构红利：Eq.15 转移时**颜色根本不用
减**（质量走了、归一化色不变），颜色守恒的 race 从表示层面消失；代价是
混色时要做质量加权平均（写侧仍需原子）。

内存账：40B/cell → **10–14B/cell**；4096 全局画布 43GB → **11–15GB**
（TITAN X 12GB 的边缘——这正是论文能跑 4096 的一个自洽重构：**量化
dense**，不需要假设未公开的稀疏结构）。中间收益立即可用：2048 从
10.7GB → ~3GB，1024 从 2.7GB → ~0.8GB。实现代价：读写侧全部要过
dequant（21 个 shader），fp16 对的原子 CAS 需要按 32bit 打包双通道——
非平凡，单独立项。

#### 6. 压感：论文的定义是几何深度，不是力反馈

论文全文检索：`pressure` 只出现在流体的 pressure projection；没有任何
力反馈/压感硬件/传感器描述。笔刷输入模型是**位姿**（位置 + 朝向随时间）；
§7 艺术家反馈的 "pressing strongly or softly" 通过**笔刷压入画布的深度**
起作用——深度由 Z 坐标给出，bristle sim 的 canvas collision + splay +
Eq.12/13 容量下降已经完整建模。**结论：压感 = 笔尖高度（Z）**，即
`node_brush_capture` 从轨迹 Z 推 BrushPressure（tip 距纸面越近压力越大），
而非接 Wintab 力反馈。这是纯输入侧工程，与仿真无关。

#### 验证

- slangc（sm_6_6）全部 29 个 BrushSimulation shader 编译通过（含 CAS 路径
  ——同时实证了"Slang CAS 不可用"的旧注释只适用于 structured-buffer 元素）。
- `test_wetbrush_zone.py` 3 passed（新蘸墨模型出墨、字面判据沉积、无 NaN）。
- 60 帧渲染验证见本轮 commit / 下次 run 记录。

### 22. 重力修复 + blob 沉降测试 + f12 爆炸根因调查（2026-08-24 后半，进行中）

> **本文档是上下文压缩前的完整交接记录。**§21 之后的所有改动、调查结论、
> 待办都在这里。所有改动**未 commit**（叠加在另一 agent §18-20 的未提交工作之上）。

#### A. §21 之后的迭代修正（蘸墨/发射参数经历了 4 轮）

§21.3 首版"蘸墨 = m_j = M_max"暴露问题后继续迭代，**当前生效值**：

| 参数 | 当前值 | 演变原因 |
|---|---|---|
| M_max（WB_M_MAX，brush_sim_common.hpp） | **0.15** | 0.03 时 15 帧墨干（笔画后段断碎）；5× 蘸墨撑满 60 帧 |
| ρ₀（bristle 节点） | **1e5** | 与 M_max 耦合同升，保持 R_j≈0.36R < D₁ |
| 蘸墨方式 | **shader 内 m_j = M'_j(ψ)**（`BristleLiquidConstants::dip_frame`，deposit 置 flag，transfer 的 ABSORB pass 消费） | host 写 M_max 无视 ψ 拥挤 → 内部样品容量只有 ~0.07 → 整支笔像超载海绵全程滴墨（76800 出生/帧，池 90 万） |
| max_emit_per_step | **1**（原 10） | 论文 §5.1 "smoothen the liquid transfer"；10/步时落笔点 5 帧吞掉半个墨仓 |
| per_particle 下限 | **0.002**（原 0.05，公式 max(M_j·0.05, floor)） | 同上；emit_m 额外 clamp 到 per_particle 防单粒吞全部过剩质量 |
| emit_budget 默认 | **0=关**（env WB_EMIT_BUDGET 保留逃生门） | 论文只有 per-sample 上限；蘸墨总量本身有限，无需全局预算 |

主笔画 sequence（run5，60 帧 1024）结果：连通域 **91%**（历史最好，§20 旧调参
89.6%，run1 41%）、红色像素 135k、质量守恒 grid 4020 + swarm 669 ≈ 4689/蘸墨 5400 ✓、
颜色零漂移（原子化生效，color_r/density 恒 1.0）。

#### B. 已实施的修复（本轮）

1. **粒子重力标定**（bug 级）：`particle_update.slang` 原 -0.2 → **cb 传入**，
   默认 `(0,0,-35.7)` units/s²（1 unit ≈ 27.5cm，真实 g=981cm/s²）。env
   `WB_GRAVITY_X/Y/Z` 可调方向（用户要的实验旋钮）。字段加在
   `ParticleConstants`（host+slang 同步）。
2. **grid 侧重力**（新）：`fluid_damp_dry.slang` 窗口 pass 对**流体格**
   （density>1e-6 且 wetness≥0.01）加速度体力，每 substep；投影随后把下压
   转成不可压缩流——贴纸面即横向铺展（"standard Eulerian"读法）。
   字段加在 `SimConstants`。副作用见 §22.D：它会拉下一切悬空沉积。
3. **pen-up 陈旧刷毛墙清理**（bug 级）：fluid.cpp pen-up 分支多清
   `bristle_density/vel_x/y/z`（原来只清 4 个粒子光栅格）——刷毛 no-flux 墙
   停留在笔刷最后位置变成幽灵墙。
4. **rznode 序列化 null-list 崩溃修复**（框架 bug）：`node.cpp` 序列化对零
   输入/输出节点写 null（`brush_wb_init_state` 无输入、`write_usd` 无输出），
   全新 stage 首帧 tick 抛 `json.exception.type_error.302`。修复：序列化端
   空 socket 列表初始化为 `json::object()`（注意是 object 不是 array——
   列表按索引字符串键，array 会抛 305），反序列化端 null-guard。
5. **测试设施**：
   - 新节点 `node_mock_press_lift.cpp`（自动被 GLOB 拾取；**需在
     `Binaries/Release/geometry_nodes.json` 手工注册**
     `"node_mock_press_lift": ["mock_press_lift"]`，若 json 被重新生成需重加）。
   - `source/tests/render_wetbrush_blob.py`：blob 沉降测试（压 0.3s→抬 0.15s
     至 z=0.08→悬停观察，90 帧，**侧视机位**看 Z 剖面，bristle 保留作参照）。
   - `source/tests/blob_sim_probe.py`：**只跑仿真不渲染**的轻量探针（配合
     512 分辨率 ≈ 几百 MB 显存、~40s），复现压墨阶段。

#### C. blob 测试的观察与数据（用户肉眼 + wb_diag/wb-ptcl dump）

用户观察（以肉眼为准，见 workflow）：压墨期颜料正确压扁在纸面（重力修复 ✓）；
**f11-12 颜料突然上抛**，悬停在带黑条纹的高度。数据（`WB_DEBUG_DUMP_PTCL=1`
的 `[wb-ptcl]` 行，commit 节点每帧 readback）：

- f2：dmax 0.51、gridvmax 3.8（地面中心，Z 分量）——**初始化瞬态**；
- f4-11：gridvmax 在 lz16-19（z≈0.03）以 ~0.1/帧爬升 = **grid 重力拉半空沉积下落**；
- f12：dmax 0.58 → gridvmax **11**，argmax lz2→lz26 上移——**射流**；
- 之后：swarm 质心 z=**0.0363 ≈ D₀=1.8R 精确**，悬停在 D₀ 壳，自激励。

#### D. f12 爆炸完整因果链（二分实验全部完成）

**环 1** f2 瞬态（把首批粒子抛到 z≤0.036 → 少量半空沉积，density tot 0.012）。
写入者未最终定位，已排除：merge（WB_DISABLE_MERGE=1 仍爆）、移动墙
（WB_NO_BRUSH_WALL=1 仍爆）、重力。剩余嫌疑：bristle 第一帧"瞬移初始化"
（顶点从零位到垂直链，0.03/帧 ≈ 1.8 units/s 假速度）经某路径进入散度/速度。
**环 2**（我的）grid 重力正确地拉半空沉积下落（0.1/帧²，从 0.03 高度
恰好 f11-12 触底——**f11/12 没有任何调度操作，是自由落体到岸时刻**，
发射耗尽只是同期巧合）。**环 3** 触底撞进刷毛墙+纸面围死的口袋，
**压力投影**把不可压缩约束变成轴心向上射流（WB_NO_PROJECT=1 则全程
安静、gridvmax 恒 0——投影是放大器）。**环 4** 抛起的粒子卡在 D₀ 壳
（笔按下 → 不沉积）+ 速度 > 0.1 慢速阈值 → 自激励速度场维持悬停。

注意：GPU 原子操作使 sim 跑跑之间有非确定性（单次二分可能误导，结论以
多跑交叉验证为准）。

#### E. 已加的诊断开关（全部保留，非物理旋钮）

| env | 作用 |
|---|---|
| `WB_DEBUG_DUMP_PTCL=1` | commit 每帧打印 swarm 运动学 + gridvmax + argmax 位置(gv@) + over1 + pmax/dmax（P warm-start 与散度峰值） |
| `WB_DISABLE_MERGE=1` | 跳过 swarm→grid 动量 merge |
| `WB_NO_PROJECT=1` | 跳过两次压力投影 |
| `WB_NO_BRUSH_WALL=1` | 刷毛 no-flux/移动墙 BC 完全关闭（gate=1e9） |
| `WB_GRAVITY_X/Y/Z` | 重力向量（默认 0/0/-35.7） |
| `WB_EMIT_BUDGET` | 默认 0=关 |

#### F. 待办（按优先级）

1. **（已提议待批准）掐掉 f2 初始化瞬态**：bristle 首帧初始化不应产生速度
   ——初始化帧 sample_vel 置零（或跳过首帧流体 solve）。瞬态一除，半空墨
   不再产生，§D 整条链断掉。然后复跑 blob 探针 + 渲染，用户肉眼验收。
2. f2 修后复测残余：口袋射流 / D₀ 壳自激励是否还有别的燃料；pen-up 的
   grid 质量流失（此前观测 199→15.8，−92%，疑平流出窗口边界）。
3. **主 sequence 回归未做**：重力/蘸墨改动后 run5 之后的
   `render_wetbrush.py` 没有重跑（blob 调查占用了时间）。
4. AO 对密集复合场的黑化（悬空层的"黑条纹"）：物理层修后重新评估。
5. 遗留大项：Eq.14 粒子吸收未实现（§5.1 最大缺口，也是 paper 的"泄压阀"）；
   4096 量化方案（§21.5）；压感=几何深度（Z）结论已定（§21.6）未接线。
6. `test_wetbrush_zone.py` 3 passed（蘸墨+重力后需复跑确认）。

#### G. 工作流约定（本轮确立）

- **渲染验收由用户肉眼**（视觉工具太差已弃用）；像素统计仅作量化补充。
- **GPU 任务严格串行**——并发两个渲染叠加 ~7GB+ 溢出 shared memory（本轮
  事故）；杀后台任务用 `nvidia-smi --query-compute-apps` 找 pid + taskkill。
- 测试用 python313（scoop）；shader 改动 slangc 预检
  （`slangc -target dxil -stage compute -profile sm_6_6`）；新 .cpp 需
  `cd build && cmake .` 重 configure 让 GLOB 拾取。
- **不 commit**（用户需明确授权）。

### 23. f2 修复验证 + f12 真因定案：wall BC 抬笔吸抽 + 单位统一 + bisect 失效 bug（2026-08-24 深夜）

#### A. f2 修复已验证
`bristle_simulate.slang` 初始化帧跳过 PBD 速度折叠（存 `brush_vel` 刚体随动）。
probe 复测：f2 dmax 0.51→0.000126，gridvmax 3.8→0.0019，f2–f11 全部粒子趴在
canvas（bbox z=[0,0]），**无半空墨**。§22.D 旧因果链的中间环节被推翻——
f12 爆发在无半空墨时依然发生。

#### B. 三个已修 bug
1. **advect 单位错配**（`fluid_advect.slang`）：回溯用 `dt·N`/`dt·D`（假定
   cells/s），但所有写入方（bristle rasterize、swarm merge、gravity 体force、
   wall BC、FLIP/PIC、粒子 drag）都是世界单位 u/s。改为逐轴
   `vel·dt/cell_size`（XY 用 cell_xy，Z 用 cell_z；顺带修了 y 轴误用 Z 步长的
   次级 bug）。全场统一为世界单位。
2. **bisect CB 失效**（`node_brush_wb_fluid.cpp`）：`WB_NO_BRUSH_WALL` 的
   gate 在 cb 上传**之后**才写入 struct——project() 内 divergence/gradient
   复用旧 `cb_buf`（gate=0.01），wall 从未被真正关掉，§22.E 的
   "wall 关闭仍炸"结论无效。gate 定义挪到上传前。**教训：改 CB 字段必须
   在 upload 之前，project() 复用外层 cb_buf。**
3. 新增 `WB_STAGE_DUMP=1` 分阶段插桩（pre/post diffuse/project1/advect/
   project2 的 |vel|max + div/p max，fluid 节点，只 dump substep 0）。

#### C. f12 真因（stage dump 定案）
f11 post_diffuse vz=0.0014（安静）→ **post_project1 vz=8.4，divmax=1.6**。
凸 stage（diffuse/advect）不可能放大极值 → div 的唯一非速度输入 =
**moving-wall BC 的 brush_wall_vel 替换**。修复后的 bisect 证实：
`WB_NO_BRUSH_WALL=1` 下 f12 **完全安静**，f14 质量正常转化，之后 blob 趴地
温和沉降（正是用户要的流体行为）。

机制：抬笔时笔毛尖在墨池上方形成 no-flux "wall"（bristle_density>gate 的
栅格化柱），墙速度 = 抬笔 0.6 + 笔毛链 canvas 钳位释放的 PBD 反冲 ~1.0
≈ 1.6 u/s 向上。移动壁 + 不可压 = **注射器活塞上提**，整个 79.6 万粒子池被
吸上 z=0.12 后悬停 D₀ 壳自激励。press/stroke 阶段 wall 是对的（排开液体）；
**separation（墙远离液体）时的吸抽不是 paper 意图**——§5.1/5.2 抬笔时墨应
留在纸面（笔只带走附着量 Eq.9/10）。

#### D. wall BC 分离语义 — 用户选了 (B) 分离面豁免；实施后的完整结论
实施 (B) 后发现豁免必要但不充分，又落了两层修复（均为 paper 保真语义）：
1. **分离面豁免**（`fluid_divergence.slang`）：wall 邻居面的速度替换仅在
   墙面逼近或静止（排开语义）时施加；沿面法线远离 = 分离界面（自由面，
   无项）。行笔排开保留、尾迹不再被倒吸。
2. **液体占据判定（air cells）**（divergence/jacobi/gradient + fluid cpp
   绑定 `ptcl_density`）：液体 = grid paint ∨ 粒子群栅格 ∨ 笔毛（§4.1/4.3
   joint field）。无液体 = 空气：div=0、pressure=0（Dirichlet 自由面）、
   velocity 清零。此前投影把整个窗口当不可压液体解——笔刷在空无一物的
   空间里"排开虚构液体"，FLIP 粒子（唯一真实质量）骑着不存在的压力场飞。
   air 模型后 pmax 2.4→0.6。
   **坑**：jacobi 声明了 `ptcl_density` 槽位后，diffuse dispatch（mode 0
   不读它）必须也绑定该槽，否则 binding set 创建失败、**整个 dispatch 被
   静默跳过**（RZ_RHI_VALIDATION 抓到）。

**残余（未除根）**：抬笔 trail——发射粒子继承 sample 位置躺在上升笔毛柱
内部/边缘（sample 质量已耗尽，cell 无笔毛 splat，gate 0.01 与 0.001 数字
完全相同），其下邻笔毛 cell 被判"逼近"→ 替换注入 div≈1.6 → fixed-point
投影（3fp×2jacobi×2project，paper Algorithm 1 规格）在薄液体丝+混合 BC 下
不收敛（after_project2 div 仍 ~1），重复 −∇p 过量扣减放大到 vz 9-16。
**wall 替换是唯一残余驱动**：`WB_NO_BRUSH_WALL=1`（现在真的生效）+ air 模型
下 blob 测试 30 帧全程安静，f14/f15 质量干净转化，墨池趴地温和摊开
（z 0.001-0.005）——即用户想要的流开行为。

**下一步二选一（待用户决策）**：
- **Eq.14 粒子吸收**（推荐，本就是 §5.1 最大缺口）：paper 里 D0 内的粒子
  会被笔毛重新吸收（ε 滞回）——抬笔 trail 应立即被吸回笔刷，不存在悬空
  液体，注入源自然消失。这是补论文机制而非继续 BC 手术。
- **去掉固体 wall**（原 D 选项）：已验证安静；笔毛=多孔介质只经 merge 动量
  耦合；需主序列 stroke 回归验证排开行为。

#### E. 其他观察
- **grid density 变负**（Eq.15 原子减法透支，grid_tot=-6.2，NO_PROJECT 运行
  中出现）：转化减掉的超过该 cell 曾存的。待查 particle_to_grid/grid_to_particle
  同帧次序与竞态。未修。
- mock 轨迹 `pen_down = bp.active` 全程 true → 抬笔期间 deposit 分支照跑、
  笔毛 raster/wall 一直活着（这是 C 选项需改 active 语义的原因）。
- stage dump 中 commit 的 `[wb-ptcl] f=N` 是该帧 fluid solve **之前**的快照
  （同帧 stage f=N 的爆发体现在 commit f=N+1）。

### 24. 输入契约重构：BrushPoint → StrokeSample（笔的动态采样点）（2026-08-25）

#### A. 动机（用户定方向）

用户指出结构限制了物理：旧 `BrushPoint` 只有 pos/time/active/stroke_start/color，
一笔的输入被降格成"一条曲线 to 笔画"，笔的**动态**（朝向、角速度）根本进不来。
排查证实比预想更糟：

- `brush_rotation`（由速度航向 atan2 派生的"朝向"）**是死字段**——CB 里带着，
  没有任何 shader 消费它；笔毛 root 螺旋固定在世界 XY，笔杆永远写死垂直
  （`down_dir=(0,0,-1)`）。朝向支持是彻底空白，不是"猜错"而是"没有"。
- 所有导数靠 host 帧间差分：accel 是位置的**二阶**差分，逐帧量化噪声放大两次
  （轨迹拐弯处的毛鞭速度尖峰一部分来自这里）；stroke_start 假定笔从静止开始
  （第二帧出现 vel/dt 假加速度尖峰，实测 72 u/s²）。

#### B. 新契约（brush_sim_common.hpp::StrokeSample）

| 字段 | 语义 |
|---|---|
| `pos` | 笔参考点（root 盘中心），世界坐标（含 Z，压深自由度不变） |
| `orientation` | `glm::quat` local→world。局部约定：-Z=root→tip，XY=root 盘。单位=旧硬编码直立笔（默认零回归）。**glm/glm.hpp 在本仓库 vendored GLM 里不含 qua，必须补 `glm/gtc/quaternion.hpp`** |
| `vel` / `angular_vel` | 世界系速度 u/s、角速度 rad/s（解析源直接供） |
| `has_dynamics` | true=deposit 直接采信；false=回落旧 FD 路径（真实采集输入） |

#### C. 落地（本轮已实施）

1. **emitter 供解析动力学**：vel=光标所在线段速度（线性段上精确），
   orientation=单位直立（CurveComponent 暂无朝向通道——契约支持、轨迹格式未跟上，
   侧锋作者化留作输入侧后续），omega=0，has_dynamics=1。
2. **deposit 位姿三路**：stroke_start+解析 → 当帧即采纳 vel/omega（笔落下时本就在动；
   假尖峰 72→消除，见 [wb-pose]）；常规帧+解析 → vel/omega 直采，accel/omega_dot 只做
   一阶差分（精确值相减，无二次放大）；无解析 → 旧 FD 全套。`brush_rotation` 降级为
   legacy 注释（勿再依赖）。
3. **朝向进物理**：BristleConstants 尾部追加 `brush_R0/1/2`（mat3_cast 列存行，
   shader `pen_rot(v)=v.x·R0+v.y·R1+v.z·R2` 无 mul() 行列歧义；追加在结构体尾部
   不动现有偏移）。`bristle_simulate.slang` 的 root 螺旋盘与 `down_dir` 改经
   `pen_rot`——倾斜笔将盖椭圆足印（侧锋）、毛沿倾斜轴生长；单位朝向下与旧代码
   逐位等价。
4. **[wb-pose] gate-A 诊断**（`WB_DEBUG_DUMP_PTCL=1`，deposit 每帧）：pos/|v|/|a|/
   |w|/tilt/dyn。zone 测试实测：dyn=1 全程、|w|=0、tilt=0、f1 假尖峰消除
   （剩余 |a| 3~33 是轨迹折线在 30fps 采样的真实一阶加速度）。
5. 连锁改名：socket `Brush Point`→`Stroke Sample`、emitter 输出 `Current Point`→
   `Stroke Sample`，9 个 py 文件 addEdge 同步；C++ 注释 BrushPoint→StrokeSample。

#### D. 验证与遗留

- slangc 预检：bristle_simulate/density_constraint/resample/rasterize 全过（CB 镜像
  hpp↔slangh 字段一致）。构建过。`test_wetbrush_zone.py` **3 passed**（10.9s；
  teardown 的 "Resource leak (2 in use)" 是注册表持有 buffer 的退出噪声，非本轮引入）。
- 60 帧 sequence A/B 归因（`WB_NO_DYNAMICS=1` = 旧 FD 行为；两 run 均存
  `Binaries/Release/wetbrush_sequence{,_dyn}/`）：
  | | 解析(新契约) | FD(旧行为) |
  |---|---|---|
  | f10/f20 主连通域 | 99.5% / 99.4% | 94.1% / 97.0% |
  | f59 主连通域 | 54.5% | 45.7% |
  | f60 grid_tot | 1541 | 1888 |
  新契约**不劣于**旧路径（前段还更好）。但两 run 同现"中段墨尽→笔画裂成两大块
  （14.9k+11.2k px）+ 质量泄漏（f15 时 grid+swarm≈3134 → f30 只剩≈1816-2207，
  ~40% 凭空消失）"——这是 §22–23 的重力/单位/wall/air 改动**从未做过 sequence
  回归**（§22.F.3 欠账）的既有回归，与本轮契约无关。泄漏形态吻合 §23.E 已记录
  的 Eq.15 原子透支/窗口边缘流出两个未修 bug；下一步按 gate 阶梯补 [wb-liquid]/
  [wb-xfer] 账本定位。
- 朝向作者化（轨迹带倾角）、子步内 orientation/vel 插值（当前子步只插位置）留作
  后续。

### 25. 输入侧收尾：mock_pen_motion 取代曲线中间层（2026-08-26）

§24 留下的尾巴：mock 侧仍走 `mock_stroke → 曲线 → emitter 折线插值 →
fill_pen_dynamics 差分`。三个实际伤害：①"解析速度"其实是折线段差分
（C0 不 C1，accel 是顶点尖峰阶梯——f1 |a|=13.97 的残余正是折线拐点加速度）；
② orientation 恒 identity、ω 恒 0，§24 铺的 brush_R0/R1/R2/pen_rot 整条路零测试；
③ pen_down 全程 true，mock_press_lift 只能用 Z 剖面曲线绕（已知问题）。

**新节点 `mock_pen_motion`**（`node_mock_pen_motion.cpp`，zone 内、ALWAYS_DIRTY、
由 payload 时钟驱动、无输入）：解析时间轴 DESCEND(hover→press) → STROKE(落笔
行走，stroke_start 首帧) → LIFT(press→hover，即刻 pen-up) → DONE。每帧解析求
pos/vel/orientation/ω：sinuous 路径 y=A·sin(2πCs) 与 mock_stroke 形状一致
（Cycles=2 ⟺ 旧 sin(4πt)）；tilt=绕水平轴 angleAxis，`mat3_cast(q)[2].z=cosθ`
与 [wb-pose] 诊断严格一致；Tilt Sweep 沿笔画线性爬升 → ω=常量解析值；
Length=0 为 blob 按压模式（吸收 mock_press_lift）；Enable=false 恒 pen-up
（"空输入"降级测试的新等价物）。`WB_NO_DYNAMICS` A/B 开关保留。

**图接线简化**：zone 边界从双槽（Curves+State）减为单槽（State）——
`mock_pen_motion → deposit` 是普通内部边，不再过 sim_in。**曲线路径保留**：
`mock_strokes`（双笔画混色 fixture）+ `mock_point_emitter` 原样不动，
`render_wetbrush_cross.py`/`export_particles_cross.py` 继续走回放路径；
为此 commit 的 "Stroke Curves" 槽改 `.optional(true)`（须配 `has_input` 守卫——
对未连接槽直接 `get_input<Geometry>` 会对空 meta_any 解引用，null+0x28 访问
违例，cdb 定位）。删除 `node_mock_stroke.cpp`、`node_mock_press_lift.cpp`。

**验证**（Tilt=25°+Sweep=10°，Speed 0.15，[wb-pose] 45 帧）：

| 量 | 预测（解析） | 实测 |
|---|---|---|
| 落笔帧 | f3（descend 0.05s@60fps） | f3，pos=(−0.150,0,0) |
| tilt | 25+10s，s=(t−0.05)/2 | 25.0°→28.3° 逐帧吻合 |
| \|ω\| | radians(10)/2=0.0873 rad/s | 0.0873 恒定 |
| \|v\| | √(0.15²+(0.314·cos4πs)²)∈[0.15,0.35] | [0.150,0.348] |
| \|a\| | 0.05(4π/2)²·sin4πs ≤ 1.97，平滑 | 0→1.970 正弦，无尖峰 |
| stroke_start \|a\| | 0（无假尖峰） | 0.000 |

test_wetbrush_zone 3 passed / test_brush_sim 2 passed（含禁用笔空画布）。
blob/pen-up 语义修正：抬笔瞬间 active=false（旧轨迹全程 true），blob 测试从此
真正只按压不漏刷。定位脚本 `Binaries/Release/wb_pose_check.py`（不入库）。
跨脚本注意：`add_nodes` 用裸 GLOB——**新增节点 .cpp 必须 `-Reconfigure`** 才能
被 cmake 发现。


### 26. "蓝色小棍"定案：BristleConstants 尾部 vec4 的 CB 布局错位（2026-08-27）

用户连续质疑序列里的"一根蓝色小棍在平移，一点也不像毛刷"。视觉+像素+GPU dump
三路会诊，**不是遮挡、不是相机比例、不是渲染能力**——是真实几何 bug：

**现象链**：debug 视图里蓝色物体 = 一根 ~90×8px 的细棍平躺在颜料表面，沿运动
方向；`WB_DUMP_BRISTLES`（本节新增的 commit 读回开关，env 给路径前缀逐帧写
bristle_data 顶点二进制）显示：600 根鬃毛的**根盘塌缩成 XZ 竖直面里 r=0.0088
的螺旋盘**（应为倾斜 XY 面 r=0.02），所有链**水平指向运动反方向**（-X），
第一帧即定型且永不恢复。

**根因**：`BristleConstants` 在结构体**末尾**追加 3 个 `glm::vec4`
（brush_R0/1/2，§24 朝向重构引入）。host 侧 vendored glm vec4 按 4 字节对齐
紧凑排列（R0@184/200/216，sizeof=232）；slang 常量缓冲按 std140 把 float4
对齐到 16 字节（R0@192/208/224，视图 240）。shader 读到的旋转矩阵整体 +8B
错位、尾部越界到资源池旧数据——CB 侦察兵（shader 把 cb 字段原样写进顶点再
读回）实测收到的"矩阵"是 (sinθ,0,0)/(0,0,−sinθ)/(cosθ,0,0)，等价于把
local X→+X、local Y→−Z、local Z→+Y：根盘立进 XZ 面、半径缩成 sin(tilt)×R。
子步行标量（vel/window 等）部分正确，说明错位只打中了 float4 对齐敏感的尾部。
画颜色/沉积一直正常，因为 R 行只被 bristle_simulate 消费。

**修复**（两处）：
1. `brush_R0/1/2` 前置到两侧结构体的**最前**（offsets 0/16/32，对任何打包
   规则免疫）；教训：共享 CB 结构体**永远不要在末尾追加 vec4/float4**，两侧
   字段 diff 一致 ≠ 布局一致（对齐规则不同）。
2. `brush_upload_cb` 改为**复用已存在缓冲**（句柄够大就不销毁重建）——
   每 substep destroy+create 会把同一资源池槽位在 in-flight dispatch 期间
   递出去，读数跨帧串味（实测一个 CB 的不同标量读到 4 个不同帧的上传）。
   注：当前调用方全部传新鲜局部句柄，此修复对它们仍是 no-op，但规则先立住。

**验证**：dump 回归——根盘 min/mean/max = 0.0005/0.0126/**0.0200**（Vogel 盘
理论均值 0.0133 ✓）、z=±0.0094=R·sin(tilt) ✓、束包围盒 0.048×0.041（原
0.042×**0.0044**）✓。debug 序列 60 帧（res512）与 beauty 序列 60 帧（res1024）
全部渲完，无 NaN/Device Removed；蓝色像素 220–550 → **6500–9400**。beauty 视
角下蓝色鬃毛尖端以扇贝状从红色颜料前缘下方露出（被湿颜料遮挡属物理正确）；
debug 视角可见完整蓝色压垫 + 黄色颜料脊被推挤 + 红色粒子飞溅。

**伴随教训**：`render_wetbrush.py` 默认 `SIM_RES=4096`——跑 beauty 必须
`WETBRUSH_RES=1024`，否则首帧分配爆显存，nvrhi createBuffer 返回 NULL →
写 NULL+8 段错误（cdb 栈：`deposit!node_execution → nvrhi` create 路径）。
调试工具链存档：dump→SVD 平面拟合/golden-spiral 回归反推矩阵 → CB 侦察兵
→ offsetof 打印（slang 无 offsetof，用侦察兵实测）。

### 27. 笔刷形态定案：共面饼→星暴/扫帚束 + 斜笔跟随（2026-08-28）

图层隔离（`WB_SHOW_PAINT=0` / `WB_DRAW_BRISTLES=0` + `WB_OUT_DIR`）+
`WB_DUMP_BRISTLES` 数值测量钉死：**整把笔 100% 顶点 z=0、链压至 40% 段长、
Extent=根盘**——竖直 rest-restore(0.5×3) 与地板钳制的拉锯把刷子压成共面
薄饼。论文转录稿无图（jpeg 被剥离），marker 转换
`pdf-library/wetbrush_single/` 亲验 **Fig 3a = 戳刺星暴**（放射长条细丝），
用户"平摊应呈线状"的直觉正确；Fig 3a 是结果图，机制论文未给。

修复（bristle_simulate.slang）：静止形态改为**扫帚形**——链沿笔轴垂到各自
触纸点（arc_plane0 精确求根），其后沿 flat_dir 贴纸平铺；flat_dir =
radial 扇形（竖直笔=戳，Fig 3a）与笔轴地面投影（斜笔=拖）按 sin(tilt)
smoothstep 混合。**第一版"从 vi=1 起整条平躺"被 600 毛方位均匀打散**：
斜笔根盘上缘比纸面高 R·sin(tilt)，该目标与定距约束几何不可达，求解器拉锯
散射；弧长守恒的扫帚形约束处处可满足，散射消失。

斜笔跟随（node_mock_pen_motion.cpp `Tilt Follow Stroke`）：方位角=瞬时解析
航向+180°（杆前倾毛后掠），含方位旋转的 coning 项
ω = θ'·n + φ'·(ẑ−qẑ)；下压即按初航向斜置落纸。验证：悬停悬挂 −116°=解析值；
按压 600/600 毛同向后掠（合成向量 |mean|=1.00），脚印 0.092→0.050。
渲染脚本新旋钮 `WB_PEN_TILT` / `WB_PEN_FOLLOW`。

**伴生修复**：`deposit` 的 pen-up 提前返回曾让笔毛缓冲全程保持零初始化
（抬笔=世界原点一个像素点）；现在抬笔跑单步松弛（不栅格化 ψ/BC，Step-4
清空笔毛场=流体眼里空中无刷），链条跟随悬停笔。

### 28. 质量泄漏定案与修复：−36% → −0.03%（2026-08-28）

**账本**：commit 的 `[wb-mass]` 加 `sample`（Σm_j）与 `neg_cells/neg_sum`；
fluid 节点加 `[wb-xfer]` 四站探针（WB_LEDGER_PROBE=1，A 进口/B solve 后/
C P2G 后/D G2P 后，仅尾帧全局读回）；`wb_ledger_run.py` 纯 tick 驱动免渲染。

**定案**（倾斜拖动 135 帧）：蘸墨后总账 ~10980；行笔段守恒至 −0.7%；
**抬笔大沉积帧 Eq.16 落账 +3412 完全成功，下一步 B_solve（标量平流）一步
−66%**。因果链：①divergence 在 gz==0 跳过底面（地板=墙）⇒ 投影构造上看
不见地板法向速度；②damp_dry 重力 −0.2 在地板层积累 vz<0；③cell_z 仅
cell_xy 的 1/4，dtz=dt/cell_z≈68，一步回溯 13.7 格：贴地薄板被抹空。
修复一（fluid_gradient.slang）：地板 no-flux 钳制 vz≥0 —— 灾难性损失消失，
但暴露镜像问题：**半拉格朗日插值"读值不扣源"**，薄板值被逐帧克隆上一层
（+15% 创造，几何级数收敛于把板复制一遍）；z 位移钳 ±0.9 格只是把撕裂换
成克隆。

修复二（根治）：标量场改**迎风通量式**（新 shader
`fluid_advect_upwind.slang`，程序句柄 `advect_scalar_program`）——面通量
按外法向投影、两侧同值反号 ⇒ 严格成对守恒；全部边界（画布地板/窗口边/
全域 XY 边/顶盖）取墙。**一维 CFL 钳（每面 ≤1）多维不稳**（三轴和 ≤3，
f60 起 1109 负格，f67 密度爆到 1.5e15），改 `h/(3dt)`（和 ≤1，单调性恢复）。
速度场保持论文半拉格朗日（非守恒量，每子步重投影）。
（2026-08-31 更新：修复二已按论文忠实性复位为可选——默认恢复 semi-Lagrangian，
upwind 转入 `WB_SCALAR_ADVECT=upwind` 逃生口，代价账见 §35；本节修复一保留。）

验收：f5 蘸墨 10980 → f135 **10977（−0.03%）**，全程 neg=0；大沉积帧
swarm 3639→0 与 grid +3652 严格对账。遗留（非守恒 bug）：①粒子池仍打满
1M（Eq.15 每 D0 格 27 候选/帧，铸造在池满时安全跳过不丢账）；②抬笔前
venturi 射流 ~4 u/s（26×笔速）仍在，CFL 钳下只表现为欠平流不丢账。

### 29. 窗口内外明暗接缝定案与修复：pack 复合 kernel 与 Eq.16 不一致（2026-08-30）

**现象**：active window 内部比外部深很多；转弯时湿区以窗口边界为硬边，
看起来像"边角冒墨"。纯画布区新旧渲染 diff=0.001 ⇒ 仿真与光照链路完全
未动，纯渲染侧问题。

**因果**：pack_float4 把 swarm 栅格按窗口格加进全局场，但这份栅格来自
§4.3 合并用的 `particle_rasterize.slang`——**2×2×2 trilinear（1 格宽）**；
而 Eq.16 沉积（`particle_to_grid.slang`）是 **W_smooth_3d、h=2.5 格、5³、
Σw=1**，Eq.15 排水同为 2.5 格。同一份粒子质量：窗内以 1 格浓度叠加（XY
约 6× 浓缩），`d/0.3` 归一化饱和 → AO 饱和 → 发黑；窗外是沉积摊开后的
2.5 格（亮）。窗口边界即两种 kernel 的交界。账本中期 swarm≈3600 vs
grid≈11000，浓缩全部进 1/64 画布面积，黑斑量级吻合。

**修复**（纯渲染侧，账本零风险）：新 shader
`particle_rasterize_render.slang`——用 **Eq.16 同款 kernel** 把全体存活
粒子重新栅格化进窗口栅格（无慢速/远距 gate：活着的就是真实墨），commit
节点 pack 前 clear+重栅格，pack 复合变成"**沉积预览**"：窗内外观 ≈ 这些
质量落定后的笔迹外观，接缝按构造消失。§4.3 速度合并栅格保持 1 格不动，
仿真零改动。

验收（f60/f80/f100 新旧对比）：湿区核心 RGB (170,15,18)→(204,11,12)
（剩余略深是物理——笔下湿墨库本来就比已沉积笔迹浓）；黑心、麻点、硬边
全消；笔迹与湿区平滑衔接。附注：预览同宽后细段会以真实薄度显示（旧 1 格
浓缩曾把欠阈值细段暂时"糊"住），与论文自述的 mode seam 时间不连续同源。

### 30. 节点合并定案：deposit/bristle/fluid 三节点合并为单一 brush_wb_sim（2026-08-30）

**动机**：三节点拆分是图编排选择而非论文结构——论文算法是单循环，三阶段共享
同一个 `WetbrushZoneState`（节点边界只是转发同一 shared_ptr）。拆分的真实代价：
① 命名错位（"deposit" 节点一行沉积代码都没有，真正落墨是 fluid 里的 Eq.16）；
② §5.1 吸收/发射被锁死在帧粒度（论文是每子步；慢笔 n_sub=1 时无差别，快甩笔
放大）；③ 跨节点隐式契约（`dip_frame` 标志、`prev_brush_vel` 滞后一帧、抬笔
清理责任分散）。

**合并结构**（`node_brush_wb_sim.cpp`，2531 行）：merged execute 读一次可选
State（init 帧缺失），三个 phase 以引用共享同一 state——保住 init 帧 PHASE 1
分配、PHASE 2/3 必须看到分配的语义；各阶段 `set_output`/早退改成语义等价的
"结束本阶段"（后续阶段自带门控照跑）。Sockets = 三节点并集（默认值不变）；
删掉 "Stroke Sample" 直通输出与无人消费的 "Bristle Samples" 读回输出
（`BristleSampleOutputs` 结构体同步删除）。commit 保持独立节点（渲染打包器，
非仿真）。注册机制零手工：`add_nodes` 每个 .cpp 自动一个 DLL，
`geometry_nodes.json` 由 `nodes_json.py` 扫宏自动再生。

**等价验证**：15 帧账本 A/B（老图 vs 新图，512 与 1024）——偏差 sample 3.0% /
swarm 2.3%；而**老图自身两次运行的噪声带就有 sample 3.74% / swarm 2.83%**
（GPU 原子顺序 + EMIT 概率门控的固有非确定性）——合并行为与旧图在管线噪声
范围内不可区分，neg_cells 全程为 0。pytest 5/5（拓扑标签断言更新为
Sim/Commit）；135 帧渲染全通（lit=81.4%/帧，与合并前 seam-fix 序列一致；
f60 均值 RGB 红通道差 0.3%）。

**排障插曲**（记录两个误导性症状）：① 首轮 1024+dump=1 账本报
`json.type_error.302 (number is null)`——cdb 下复现消失，重跑 3/3 通过，
判定为 DLL 落盘后即刻启动的瞬态抖动（与本框架已知的 DLL 加载 flakiness 同
类），与合并无因果；② 无调试器渲染冒烟两次 exit 139——实为**进程退出时**
nvrhi 清理路径的 AV（135 帧已全部渲染完成），属退出清理的既有问题，干净
重跑 EXIT=0。

**斜笔默认（2026-08-31 追记）**：应用户要求直立笔退役——`mock_pen_motion` 节点
默认改 `Tilt=30° + Tilt Follow Stroke=true`（§28 账本验证的同款拖动姿态），
`render_wetbrush.py` 的 env 回退同步 `30/1`（`WB_PEN_TILT=0` 可显式切回直立）。
blob 顿笔模式有 `tilt_follow && !blob` 守卫，不受影响。合并节点斜笔验收：
`wetbrush_merge_tilt/` 135 帧拖帚后掠形态正确（按压段 z 跨度 [-0.065,0.01]，
比直立版 [-0.15,0.07] 更贴纸）、`degenerate_segs` 全 0、斜笔账本 15 帧
`neg_cells` 全 0、pytest 5/5。

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

### 31. 行笔不落布 + 抬笔堆积定案：§4.2 移动墙 BC 被毛速度伪影泵成地板射流（2026-08-31）

**现象**（`wetbrush_merge_tilt` 序列）：行笔期间颜料只有零碎弧形溅射落到画布，
大部分质量以"骑行群"跟随笔刷；抬笔后群在一帧内于笔刷最后位置整体倾泻成堆积。
像素统计：行笔 120 帧只落 24%，抬笔后一帧 +76%（抬笔区红色 15k→29k px 翻倍）。

**诊断链**（账本工具全流程）：

1. `[wb-mass]`：群质量 f=20 起 ~3790 恒定（骑行群稳态），画布缓爬；抬笔帧
   网格 +3512 一帧跳变。粒子池 1M 自 f=21 起打满——发射端被池拒绝，蘸墨的
   2/3 永远留在毛样本上。
2. `[wb-ptcl]`：粒子 vmean 0.15-0.39（笔速 1-2.6 倍），swarm bbox 压平在
   地板 z≈0、沿 x 拉长 4.4 倍笔径——骑行群实为被流场拖行的"地毯"。
3. `[wb-stage]`：窗口 |v|max 全程 1.3-4.4 u/s（笔速的 10-30 倍），峰值在
   笔刷正下方贴地板层；粘性扩散能把峰值压掉 2/3，压力投影又顶回来——投影
   在被持续供能。
4. **三 bisect**（现成 env 开关，各跑一次 sim-only）：`WB_GRAVITY_Z=0` 射流
   仍在；`WB_DISABLE_MERGE=1` 仍在；**`WB_NO_BRUSH_WALL=1` 射流消失**
   （gridvmax 0.27-0.45），且池子连续周转（1M→230k→30k），颜料沿途落布——
   实锤 §4.2 毛刷移动墙 BC 是射流泵。
5. `WB_DUMP_BRISTLES` 量化墙速来源：毛顶点速度 = 笔速的 **2.3-4.1 倍**
   （PBD 修正/弹簧鞭梢伪影，即 §27 记录的"尖速 4×笔速"）。栅格化把这个
   伪影速度喂给移动墙 BC → 投影按 3-4× 笔速卷吸 → 地板射流拖行粒子毯 →
   永远过不了 §5.2 慢速门（0.1 u/s）→ 沉积停摆。

**修复**（`bristle_rasterize.slang`）：墙速度场改用笔刷**刚性速度**
`v_B + ω_B × (p − x_B)`，不再用 sample_vel。β_B≈0 时毛紧贴笔刷刚体运动，
刚性场才是无伪影的墙速度；sample_vel 继续喂粒子出生/局部标架（未动）。

**验证**：账本行笔期落布 98%（85→2246→3609→4922→5331 连续爬升，群同步
排空），抬笔后仅 +2% 尾巴；窗口 |v|max 降至 0.5-1.8；渲染序列成连续正弦
笔画带，无终笔堆积。诊断工具链：`wb_diag_parse.py`（Binaries/Release，
解析 [wb-mass]/[wb-xfer]/[wb-stage] 出曲线）。

**教训**：论文原文机制（移动墙 BC）+ 被污染的输入（PBD 伪影速度）= 比不用
机制更糟。BC 类机制的输入合法性要单独量化。另：诊断途中一次"确定性段错误"
实为漏带 `WETBRUSH_RES=1024`（默认 4096 的 zeros3d 正好 4GiB，超 D3D12 单
CPU 可见资源上限 4294901760 → 创建失败 → nvrhi null 解引用），cdb 抓栈
`CreateCommittedResource Width invalid ... 4294967296` 一锤定音。

### 32. 窗口边缘"不知所谓液体"定案：永不干的颜料在无流墙上积堤（2026-08-31）

**现象**（用户实时观察）：活动窗口随笔移动，窗口边缘（远离笔刷处）不断冒出
不明液体。用户怀疑是守恒搬运在边缘"洒"液体。

**静态审计排除索引类 bug**：`particle_rasterize`（越界 splat 有界检查，跳过
窗外格子）、`fluid_advect_upwind`（窗口边界显式无流墙，通量成对反对称守恒）、
`field_copy_window`（mode 1/2 映射直接无偏移）——三者均无越界写/错位写。

**真实机制**（三因素叠加）：

1. **湿颜料永不干**：沉积写 wetness=1.0，旧默认 drying_rate=0.1/s、固化阈
   0.01 → ~600 帧才干透。整个会话里画布颜料始终可流动。
2. **尾流+重力持续搬运**（CFL 钳制后仍 ~3 格/帧）：湿尾迹被逐渐向后拖。
3. **窗口边缘 = 平流格式的无流墙**（论文 §4.2 窗口语义内建）：向后拖的颜料
   在墙上**积堤**，窗口一移走堤就冻结进画布——表现为沿窗口扫过路径出现的
   直线边/异常液体，远离笔刷。辅证：修复前渲染里笔画起笔端呈笔直斜切（顶到
   初始窗口左墙被铲平）。次级机制：粒子越窗瞬间被 FLIP 冻结（窗外速度=0）
   → 立即过慢速门 → 在边缘排成线状沉积。

**修复**：`Drying Rate` 默认 0.1 → **2.0/s**（socket 上限内；沉淀湿度 1.0
约 0.5s 到固化阈）——论文 §4.2 的尾迹冻结机制本就是干透格子"无视速度、按
固体处理"，我们只是把干燥速度调到了笔画时间尺度。WB_DRYING 环境变量可覆写
（render_wetbrush.py）。渲染验证（2.0/s 出货默认）：尾迹边缘干脆带鬃毛纹理、
不再糊开、无终笔堆积；外观湿感不变（wetness 不进渲染方程，只门速度/固体）。

**注意**：干燥快 ⇒ 笔画 ~0.5s 后变固体障碍，后续笔画会被它偏折（物理正确，
"在干颜料上叠笔"）；更快的 3.0/s 也验证过无副作用。遗留观感：子步印章的
梳齿纹理、收笔端细拖尾（lift 漂移）。

### 33. 世界单位重定标：1u = 1cm，纸 10×10cm，刷头宽 1cm（2026-08-31）

**动机**：旧世界（1u≈27.5cm、纸 1.0u、笔 R=0.02u≈0.55mm）在测试分辨率下笔刷
直径只占 41 格，且笔画只占画布面积 6.8%——分辨率利用率过低。用户定标：
画布 10×10cm，刷头宽 1cm。

**新尺度**（1u = 1cm，一切长度以世界单位为准）：

| 参数 | 旧 | 新 | 依据 |
|---|---|---|---|
| Paper Size | 1.0u | **10.0**（10cm） | 用户定标 |
| Brush Radius | 0.02u | **0.5**（刷头宽 1cm） | 用户定标 |
| D0 / D1 | 1.8R / 0.55R | **1.0 / 0.3（cm）** | 论文 Table 1 SI 绝对值逐字可用（R=0.5≈论文笔刷 0.55cm），推断偏差消除 |
| 重力 | 35.7 | **981** | g=981cm/s² 换算 |
| slow_deposit | 0.1 | **2.0** | ~2cm/s 尾迹沉降速度 |
| ρ₀ | 1e5 | **12.3** | 与 M_max=0.3 配对保 R_j≈0.18cm=0.36R<D1 |
| M_max | 0.15 | **0.3** | 0.15 在 5cm 笔画 60% 处耗尽（首验证发现） |
| 沉积/取料核长 h | 2.5×cell | **0.0244cm（世界绝对）** | ≈真实鬃毛粗细 0.25mm；=设计格点 2.5 格，观感约定保持 |
| 活动窗口 | 128 格 | **320 格**（WB_WIN_XY） | 窗口世界覆盖 3.1cm≈6.25R，与论文相对空间一致 |
| Canvas Height | auto | **1.0cm**（WB_HEIGHT） | 自动式 0.625 会夹毛链（rest 0.75cm）；cell_z≈1.6 cell_xy |
| 粘度 | 0.5 | **2.0** | 新格子尺寸下重力 CFL 顶到输运钳制，需更强扩散冻尾流 |
| fixture 笔画 | 0.3u 长/0.15 速 | **5cm / 2.5cm/s**（T_s=2s 不变） | 真实手速 |
| 网格 | 1024²×64=67M 格 | **1024²×64**（刀耕 33.5M@rz32 亦可） | 密度 102 格/刷头直径 = 旧 2.5× |

**关键做法**：核长/归一化/阈值这一批"格点相对"约定，通过让核长在世界尺度
= 设计格点的 2.5 格（=鬃毛粗细）保持不变——每格密度约定、pack 的 d/0.3、
Eq.15 的 0.05 阈、干燥固化的 1e-6 全部无需改动。**分辨率无关的正确做法**
是这批约定全部世界化（后续任务）。

**验证**：账本连续落布（535→4983→9326→10605，群同步排空，无终笔倾泻），
渲染 135 帧全通。**遗留**：笔画中段仍有斑点状欠覆盖（sag 湍流 gridvmax
~20-27cm/s + 蘸墨尾段稀疏化），子步梳齿纹理加重（格子密了）——观感打磨项。

**插曲**：json.exception 抖动确认与配置无关（同配置 cdb 下完整 135 帧），
疑似渲染器初始化与 tick 的竞态，预存问题挂账。

### 34. 窗口边界闪烁 + 鳞片状 artifact 定案：粘度单位错标 ×100 + Eq.15 不尊重干燥固体（2026-08-31）

**症状**（重定标后 135 帧序列，帧间 diff 量化）：① f15-18 尾迹整块"消失又
出现"（已沉积颜料一帧内被吃掉再补回）；② f47-55 实心笔画中部被横向豁开
（宽凿痕），尾段满屏锯齿"鳞片"；③ churn 指标 3.7%/帧（丢失+新增像素/总
颜料像素），内部凿孔 295/帧、边缘蚕食 317/帧——旧标定好序列为 1.3%。

**逐帧 diff 工具**（新增，纯后处理）：对相邻帧取"红色系颜料像素"掩码做
差集，红=丢失、绿=新增；`nb_count≥7` 区分内部凿孔（真消失）与边缘蚕食
（阈值抖动）。本节全部数字出自该工具（内联 python，未入库）。

**根因 1（顺手修复，非主犯）：粘度 Jacobi 系数的单位域假设**。
`a = dt·ν·N²` 隐含"画布=1 单位域（h=1/N）"（Stam stable fluids 的单位盒
约定），paper_size=1 时恰好成立；重定标后 paper=10，正确形式
`a = dt·ν/h² = dt·ν·(N/paper)²`——重定标把有效粘度**静默放大 100×**（与
§33 修的核半径同族）。ν=2.0 时 a≈5460，每 sweep 邻域权重 99.4%，整窗
速度被拉平均匀。修复后 Viscosity 语义 = 运动学粘度 cm²/s（水 0.01 / 甘油
1 / 厚丙烯 2-20）。**梯队证据**：a∈{54.6,218,546}（ν=2/8/20）churn 完全
不动（0.037/0.036/0.036）——粘度不是闪烁主因，但单位 bug 是真的，修复
保留（粘度梯队里 f48 宽凿痕在 8/20 档依旧，也证明凿痕另有来源）。

**根因 2（主犯，论文偏差）：Eq.15 排水不尊重 §4.2 干燥固体**。论文：干
燥达标格"视作固体"。我们的 grid_to_particle 只看 density≥0.05——排水盘
（brush_radius+D0=1.5cm 半径）内**已干透的尾迹**照样每帧整格抽成粒子、
随流漂移、再抖动落回：每次抽/还循环 kernel+抖动把质量随机游走 ~1 格，
尾迹边缘持续退化到渲染阈值下（消失），再落回（出现）；窗口每帧整数滚动
（≈10 格/帧）把侵蚀线烙成周期锯齿=鳞片。**修复**：g2p 三处加
`wetness < 0.01` 门（cell 门、插值归一化、邻域扣减，0.01=is_solid 同款
阈值），干漆不可液化——论文忠实（固体不回粒子）。

**根因 3（放大器）：干燥率 2.0/s 太慢**。线性律 wetness-=rate·dt，1.0→
0.01 需 0.5s=笔刷走 1.25cm；叠加 1.5cm 排水盘和沉积重湿戳（p2g
atomicFloatMax wetness=1.0），笔迹后 ~2.75cm 永远湿+可拖拽——豁口（湿膜
被压力投影流撕开）和鳞片全在此区内。**梯队**（+根因2 修复）：
interior-loss/frame 295→292(dry2)→266(dry6)→258(dry12)，boundary
317→291→244→213；dry12 的 f48 宽凿痕消失、鳞片区冻结。**默认提为
12/s**（触干 0.083s≈2.5cm/s 下一刷宽；论文只说 "increase the dryness by
a small amount"，无数值，属言之成理）。socket 上限 2→50。

**遗留（量化挂账）**：f13-18 早段内部凿孔 905→848 改善有限——该段笔迹
全长 <1.9cm，整体在排水盘内且全湿（干燥门保护不到新漆）；尾缘每帧被抽走
随湿头前移 ~7px（=笔速），减速/转头时还回=用户所见"消失又出现"。这是
§5.2 排水盘内"湿头骑行"的固有动力学，下一步方向=拖尾流速度衰减（Eq.8
摩擦/投影回流）或排水盘边界 d_B 精确化（现用中心距离近似
d_center≤R+D0）。转头区羽状暗带+蕾丝斑点=§33 遗留的 sag 湍流斑驳，未变。

**回归风险**：blob 顿笔 sag 验收依赖湿态。render_wetbrush_blob.py 本是
旧标定（paper 1.0 / R 0.02 / ν 0.5 / dry 0.1），§33 漏改——本节一并补齐
1u=1cm（paper 10、R 0.5、HOVER_Z 2.0 清 D0=1cm 粘附球、ν=2.0、
dry=12/WB_DRYING、相机 frame 1.6）。验证见 §34.1。

**脚本/插槽变更**：`render_wetbrush.py` 新增 `WB_VISCOSITY`（默认 2.0）、
`WB_FRAMES`（默认 135）；WB_DRYING 默认 2.0→12；节点 Viscosity 语义改
cm²/s（默认 2.0、max 50），Drying Rate 默认 12.0、max 50。

### 35. 标量平流回归论文默认：semi-Lagrangian 复位，upwind 降为逃生口（2026-08-31）

**动机**：§28 修复二把标量场改成迎风通量式，是当时唯一能止血质量泄漏的手段，但论文 §4.2
原文是 "we advect **all** of the fields using the semi-Lagrangian method"——迎风是复现
自创偏离，且其一阶格式带来的数值耗散（笔触边缘钝化）与论文不同。fidelity 审核对齐：默认
回归论文方法，偏离降级为可选。

**改动**（`node_brush_wb_sim.cpp`）：`advect_scalar_program` 默认载入
`fluid_advect.slang`（与速度场同一 shader，其全局读入路径 flag=0 本来就在）；新增
`WB_SCALAR_ADVECT=upwind` 环境变量恢复迎风式（`fluid_advect_upwind.slang` 留盘）。
注意点：标量段必须显式复位 `advect_field_window_local=0`——速度段把它留在 1（窗口
局部），semi-Lagrangian 据此决定 field_in 全局/局部索引，漏复位会读错数据（upwind
不受影响，它恒按全局读）。§28 修复一（fluid_gradient 地板 no-flux 钳制）**保留不动**。

**A/B 账本**（同 fixture 同参数 135 帧，`wb_ledger_run.py`，蘸墨基线 ≈22330）：

| 方案 | 总漂移 | 最差单帧 | 负格 |
|---|---|---|---|
| semi-Lagrangian（默认） | **+4.56%** | 0.31% | 84/135 帧，峰值 285 格（Σ−10.3） |
| upwind（逃生口） | −0.02% | 0.05% | 9/135 帧，峰值 58 格（Σ−0.4） |

**如实记录的代价**：论文默认是"造质量"侧漂移（+4.6%/笔，无灾难步——§30/§31 修复后
地板射流放大器已不存在，旧 −66%/步事故不会复现；最差 +0.31%/帧）。迎风的严格守恒
（−0.02%）与半拉格朗日的论文一致性不可兼得，默认取论文；需要严格守恒的账本排查用
`WB_SCALAR_ADVECT=upwind`。负格是微量级别（Σ−10 vs 总量 22330，−0.05%），semi-
Lagrangian 的凸插值本身不产负值，来源是 §5.2 转移 clamp 的残差，不追。

## 遗留目标

### 高优先级

- **Group A 全 grid buffer 稀疏化**：26 个核心 sim field 在 4096 下要 112GB。需要
  sparse / block-allocated grid（只为有 paint 的区域分配）。这是真正上 4096（paper
  分辨率）的前提。12 GB 卡当前用 `WETBRUSH_RES=1024` 跑（~7 GB），2048 需 28 GB。
- **压感输入接入**：`node_brush_capture` 已能捕获鼠标轨迹，但笔刷压力目前只是
  `brush_wb_sim` 的 BrushPressure socket 常量（默认 1.0；原 `node_brush_input`
  节点已删除），无 Wintab / Windows Ink / pen pressure
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
- **XY splat kernel（如 600 bristles 仍不够）**：bristle_rasterize 单点 XY splat 改
  Gaussian/3×3 kernel。偏离 paper（paper 没写 XY kernel），需批准。

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
| Sim state | `source/Editor/geometry_nodes/BrushSimulation/brush_sim_common.hpp` | WetbrushSimState, NUM_BRISTLES, packed_paint |
| Sim nodes | `source/Editor/geometry_nodes/BrushSimulation/node_brush_wb_{init_state,sim,commit}.cpp` | global grid（2026-08-30 deposit/bristle/fluid 三合一进 sim；2026-08-31 移入 BrushSimulation/） |
| Pen/粒子输入 mock | `source/Editor/geometry_nodes/BrushSimulation/node_mock_pen_motion.cpp`、`node_mock_point_emitter.cpp` | StrokeSample 生产者（2026-08-31 随迁） |
| Shader indexing | `BrushSimulation/shaders/common.slangh` | window_map, bristle_gi/grid_gi |
| Mock strokes | `source/Editor/geometry_nodes/node_mock_strokes.cpp` | 双色交叉笔画测试 fixture（留在根目录） |
| Render rprim | `source/Runtime/renderer/source/geometries/volume_impl/wetbrush_volume_impl.{h,cpp}` | 零拷贝 lookup（VolumeImpl 子类） |
| Render node | `source/Runtime/renderer/nodes/wetbrush_render.cpp` | geom_dirty reset |
| Renderer | `source/Runtime/renderer/source/renderer.cpp` | registry version poll, reset fold |
| Render param | `source/Runtime/renderer/source/renderParam.h` | pending_force_reset, registry version |
| Render delegate | `source/Runtime/renderer/source/renderDelegate.cpp` | HdRuzinoRenderParam setting |
| Python binding | `source/Runtime/renderer/python/renderer.cpp` | reset_accumulation() API |
| Volume shader | `source/Runtime/renderer/nodes/shaders/wetbrush_render.slang` | VolumeClosestHit/Intersection |
| Volume helpers | `source/Runtime/renderer/nodes/shaders/volume_intersection.slang` | samplePaintField, intersectSlab |
| Test driver | `source/tests/render_wetbrush.py` | interleaved sim+render |
| Cross test | `source/tests/render_wetbrush_cross.py` | 双色交叉混合 |

> 2026-08-31 整理：wb 三节点 + `brush_sim_common.hpp` + 两个 mock 输入节点从
> `geometry_nodes/` 根目录移入 `BrushSimulation/`（与 shaders/ 同处；CMake 在
> `add_nodes` 里给该目录追加了一个 `SRC_DIRS`，节点注册 JSON 不变）。
> `node_brush_input.cpp` 已删除（下游 `brush_paint_sim` 早已不存在）；
> `node_brush_capture.cpp` 保留在根目录（视口捕获路径）。历史小节中出现的
> `node_brush_wb_{deposit,bristle,fluid}.cpp` 均为合并前的旧文件名。

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
# 默认即 1024²×64（~7 GB，§33）。要升分辨率时（4096 全 grid 分配需 ~112 GB，
# 须先稀疏化）用环境变量：
# WETBRUSH_RES=2048 python ../../source/tests/render_wetbrush_cross.py   # ~28 GB
# WETBRUSH_RES=4096 python ../../source/tests/render_wetbrush_cross.py   # ~112 GB
# （Python 须 3.13；PATH 里的默认 python 若是 3.12 会报
#  "Module use of python313.dll conflicts"——用 scoop 的 python313 或
#  Binaries/Release/python.exe）
```

Shaders 运行时编译（非 build 时）。编辑 `.slang` 后无需 rebuild，但 renderer 加载的
是 deployed copy（如 `Binaries/Release/usd/hd_RUZINO/resources/shaders/`）。

分辨率/参数在 `render_wetbrush.py` 顶部（SIM_RES 默认 1024、SIM_RES_Z 默认 64，
可用 `WETBRUSH_RES` / `WETBRUSH_RES_Z` 环境变量覆盖；SPP 同理）和
`BrushSimulation/brush_sim_common.hpp`（NUM_BRISTLES、WB_M_MAX、WB_WIN_XY）。

## 参数对照（paper Table 1 vs 当前）

| 参数 | Paper | 当前 | 备注 |
|---|---|---|---|
| Grid 分辨率 | 4096×4096×64 | 脚本默认 1024²×64（`WETBRUSH_RES[_Z]`；节点 socket 默认 512/32） | §33 后 102 格/刷头直径；4096 全 grid 分配需稀疏化 |
| D₀ (grid→particle range) | 1 cm 固定 | **1.0 cm 绝对**（`node_brush_wb_sim.cpp` PHASE 3） | §33 起论文 SI 值逐字可用；旧 1.8R 推断已弃 |
| D₁ (bristle adhesion) | 0.3 cm | **0.3 cm 绝对** | 同上；R_j≈0.36R < D₁ |
| ρ₀ (paint density) | 1.0e3 kg/m³ (SI) | 12.3（与 M_max 配对调定） | §33；R_j=cbrt(3·M_max/(4πρ₀))≈0.18cm |
| M_max (蘸墨容量) | — | 0.30（`WB_M_MAX`） | §33；0.15 会在 5cm 笔画 60% 处耗尽 |
| 粘度 ν | 0.5（论文单位） | 2.0 cm²/s（max 50） | §34：a=dt·ν/h²（旧 dt·ν·N² 在 paper=10 下错标 ×100） |
| Drying Rate | 无数值（"small amount"） | 12/s（max 50） | §32 曾 2.0；§34 提至 12 + g2p 加 wetness<0.01 固体门 |
| slow_deposit | 无数值（"moves slowly"） | 2.0 cm/s | §33 |
| 沉积/排水核长 h | — | 0.0244 cm 世界绝对（p2g/g2p 同款） | §33；≈2.5 格@1024=鬃毛粗细 |
| 活动窗口 | 128×128×32 | 默认 320×320×res_z（`WB_WIN_XY`） | §33；世界覆盖 3.1cm≈6.25R，与论文相对空间一致 |
| γ (FLIP/PIC blend) | 0.8 | 0.8 | ✓ |
| δ (particle friction) | 1/0.2 cm | 5.0/D₀ | 单位换算后一致 |
| α (pressure solver) | 1 | 1 | ✓ |
| Bristle 数 | 40-600 | 600 | paper 上限 |
| 每 bristle sample 数 | 128 | 128 | ✓ |
| oil_density vs density | 分开的两个 field | 分开 | paper §3/§4.2/§6 明确区分，不冗余 |
