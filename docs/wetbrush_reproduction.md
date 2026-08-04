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
  是 paint 的持久存储；每帧只在笔刷周围的 active window (128×128×res_z) 内计算。
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
