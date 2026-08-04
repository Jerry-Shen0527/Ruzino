# Rasterization-Based G-Buffer Pipeline

状态进度文档(2026-08-04)。与 path-tracing pipeline 并列的、基于连接节点的
rasterization pipeline。目标:GPU-driven + instance-based 渲染,产出 G-Buffer,
复用现有材质生成管线的数据,后续接 deferred 直接光。

## 架构

```
[rasterize] ──G-Buffer(MRT)──► [deferred_direct_lighting] ──► [present_color]
                                  (compute, WIP)
```

- **rasterize 节点**:GPU-driven + instance-based,一条 `draw_indirect` over
  `draw_indirect_pool`,VS 从 bindless buffer 取顶点。输出 6 MRT + depth:
  Position / Texcoords / DiffuseColor / MetallicRoughness / Normal / MaterialID。
- **deferred_direct_lighting 节点**(WIP):compute,消费 G-Buffer,按 material id
  dispatch 不同 BSDF,累加直接光。
- 节点自动注册:`add_nodes()` glob `nodes/*.cpp` → DLL + `render_nodes.json`
  自动重生成,**不需要手改 json**。用 Python API 建图
  (`tree.add_node` / `tree.add_link`)。

## 已完成 ✅

1. **材质函数抽提**(`Scene/MaterialEvaluation.slang` + `Scene/MaterialBSDF.slang`)
   - `decode_material_surface()`:L 无关的材质属性解码(G-Buffer 填充用)
   - `evaluate_material_bsdf()`:按 material id dispatch 不同 BSDF
     (standard_surface / UsdPreviewSurface / Lambertian),返回纯 BSDF 值
     `f(L,V)`(无 NdotL,和 callable 约定一致)
   - `light_sampling.slang`:从 `pt_sample_lights.slang` 抽出的几何采样
     (去 shadow ray),raster 和 PT 共用
   - 设计意图:这些是纯 Slang 函数(无 ray intrinsic),raster 和 path tracer
     都能调,不依赖 DXR CallShader。

2. **rasterize G-Buffer 节点** —— 修复了原 stub(原 PS 硬编码洋红、VS 的
   `load_vertex` 2 参数调用从没编译过):
   - GPU-driven:VS 通过 `instanceDescBuffer[SV_InstanceID]` → `meshDescBuffer`
     → `load_vertex(mesh, id, t_BindlessBuffers[...])` 取顶点
   - Instance-based:`SV_InstanceID` → instance transform(和 PT 共用 instance_pool)
   - 真实材质解码路径已搭好(读 materialHeaderBuffer + LUT + decode)
   - **已验证渲染通过**:三角形正确光栅化(Position target 8192 像素,世界坐标
     合理),Y 轴方向正确(去掉 VS 里多余的 `position.y *= -1`)

3. **关键 bug 修复记录**(详见下方"踩过的坑")

## WIP / 未通 ⚠️

1. **`materialBlobBuffer` 读取导致 device-removed**:`materialHeaderBuffer[0]`
   (16B)能读,但 `materialBlobBuffer[0]`(1024B `MaterialDataBlob`)读就崩。
   path tracer 读同一个 buffer 没问题(RT 单 stage),但 rasterize(VS+PS)读就崩。
   怀疑 DeviceMemoryPool relocate 或 VS+PS binding 的 stride 问题。
   → 当前 PS 里 `decode_material_surface` 调用被注释掉,base_color 用常量
   fallback(所以渲染图是固定红色,不是真实材质 albedo)。

2. **materialTypeLUT 上传到 GPU 值不对**:CPU 写 1(UsdPreviewSurface),GPU 读 0。
   `mapBuffer(Write)` 和 `writeBuffer` 两条路都试过,都不生效。

3. **deferred_direct_lighting 跨 graphics→compute SRV 读取崩**:结构完整
   (TAA 式手动 binding + 材质 LUT + 光照循环 + per-material BSDF),但读 G-Buffer
   的 SRV 在 compute pass 里 device-removed。依赖问题 1 修复后才能继续。

4. **readback 偶发崩**:RGBA16_FLOAT 输出 → readback 必崩;RGBA32_FLOAT(和
   path tracer 一致)能读但偶发崩。一次 crash 后 device 状态损坏,同进程后续
   渲染也失败 —— 调试时要每个迭代开新进程。

## 踩过的坑(耗时很长,记录避免重复)

1. **shader cache staleness**:`Binaries/Release/shader_cache` 缓存编译产物 +
   reflection。改 `.slang` 后必须 `rm -rf`,否则 binding 不一致 → 各种红鲱鱼崩溃。
2. **shader runtime 路径**:`nodes/shaders/shaders/*` ≠
   `Binaries/Release/usd/hd_RUZINO/resources/shaders/*`。新 .slang 要么重建
   hd_RUZINO(后构建步骤拷贝),要么手动 `cp`。
3. **VS+PS 合并反射丢弃 PS-only imported 资源**:只在 PS import 的模块里声明的
   资源(materialBlobBuffer 等)**不出现**在合并反射里 → ProgramVars 名字绑定静默
   no-op → GPU 读 0。**修复:VS 和 PS 都 import 同一个模块**
   (`rasterize.vs.slang` 也 `import Scene.MaterialEvaluation`,即使 VS 不读这些
   buffer)。这是 rasterize 材质 binding 一直失败的根本原因。
4. **R32_UINT render target 不能 `clearTextureFloat`**(GraphicsContext::begin
   清所有 color target 为 float)。MaterialID target 用 R32_FLOAT + `float(uint)`
   打包。**不能用 `asfloat`/`asuint`** round-trip —— D3D12 下崩 CS;plain
   `float(id)`/`uint(x+0.5)` 工作(id 是小整数,精确表示)。
5. **blob 字段布局不是 struct 布局**:MaterialX 生成器把每个 uniform 按
   声明顺序逐个 `asfloat(data.data[i])` 写,不是按 C struct
   (`PreviewSurfaceMaterialParams` / `PackedStandardSurfaceMaterialParams`)。
   `reinterpret<Struct>(blob)` 读到垃圾(padding/对齐)。必须按生成顺序的
   index 逐字段读(见 `_decode_preview_surface` 的 UsdPreviewSurface 字段顺序:
   diffuseColor 0-2, emissiveColor 3-5, ...)。
6. **material_type_id 是 pool index 不是枚举**:`MaterialHeader.material_type_id`
   = `material_header_handle->index()`(pool 分配的 index),不是 0/1/2。要建
   C 侧 LUT 扫材质 shader 源码(`shader_type_id = 0/1` 标记)做映射。
7. **`hd_RUZINO.dll` / 节点 DLL 锁**:重建前要 kill 占用 DLL 的 python 进程。

## 文件清单

| 文件 | 类型 | 说明 |
|------|------|------|
| `nodes/node_render_rasterize.cpp` | 改 | G-Buffer rasterize 节点 |
| `nodes/node_deferred_direct_lighting.cpp` | 新 | deferred 直接光节点(WIP) |
| `nodes/shaders/shaders/rasterize.vs.slang` | 改 | GPU-driven 顶点着色器 |
| `nodes/shaders/shaders/rasterize.ps.slang` | 改 | G-Buffer 像素着色器 |
| `nodes/shaders/shaders/Scene/MaterialEvaluation.slang` | 新 | 材质属性解码(纯函数) |
| `nodes/shaders/shaders/Scene/MaterialBSDF.slang` | 新 | BSDF 评估(纯函数) |
| `nodes/shaders/shaders/light_sampling.slang` | 新 | 共享光照几何采样 |
| `nodes/shaders/shaders/deferred_direct_lighting.cs.slang` | 新 | deferred compute shader |
| `tests/test_raster_pipeline.py` | 新 | raster pipeline 测试 |

## 下一步

1. 修 `materialBlobBuffer` 读取崩(最关键 —— 修了它真实 albedo 就通,deferred
   也能接着通)。方向:检查 DeviceMemoryPool 的 buffer 在 VS+PS binding 时的
   stride/state,或对照 path_tracing.cpp 的 binding 差异。
2. 修 materialTypeLUT 上传。
3. deferred lighting 跨阶段 SRV(依赖 1)。
4. readback 稳定性。
