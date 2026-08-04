# Rasterization-Based G-Buffer Pipeline

状态进度文档(2026-08-04，大幅更新)。与 path-tracing pipeline 并列的、基于连接节点
的 rasterization pipeline。目标:GPU-driven + instance-based 渲染,产出 G-Buffer,
复用现有材质生成管线的数据,后续接 deferred 直接光。

## 架构

```
[rasterize] ──G-Buffer(MRT)──► [deferred_direct_lighting] ──► [present_color]
                                  (compute, Lambertian 直接光)
```

- **rasterize 节点**:GPU-driven + instance-based,一条 `draw_indirect` over
  `draw_indirect_pool`,VS 从 bindless buffer 取顶点。输出 6 MRT + depth:
  Position / Texcoords / DiffuseColor / MetallicRoughness / Normal / MaterialID。
- **deferred_direct_lighting 节点**:compute,消费 G-Buffer,decode 材质,
  遍历 lightBuffer 算 Lambertian 直接光(albedo/π × intensity × NdotL)。
- 节点自动注册:`add_nodes()` glob `nodes/*.cpp` → DLL + `render_nodes.json`
  自动重生成,**不需要手改 json**。用 Python API 建图
  (`tree.add_node` / `tree.add_link`)。

## 已完成 ✅

1. **材质函数抽提**(`Scene/MaterialEvaluation.slang` + `Scene/MaterialBSDF.slang`)
   - `decode_material_surface()`:材质属性解码(G-Buffer 填充用),按 material_type
     分支到 `_decode_standard_surface` / `_decode_preview_surface` / fallback
   - `evaluate_material_bsdf()`:按 material id dispatch 不同 BSDF
     (standard_surface / UsdPreviewSurface / Lambertian),返回纯 BSDF 值
     `f(L,V)`(无 NdotL,和 callable 约定一致)
   - `light_sampling.slang`:从 `pt_sample_lights.slang` 抽出的几何采样
     (去 shadow ray),raster 和 PT 共用
   - 这些是纯 Slang 函数(无 ray intrinsic),raster 和 path tracer 都能调

2. **rasterize G-Buffer 节点** —— 产出真实材质 albedo:
   - GPU-driven:VS 通过 `instanceDescBuffer[SV_InstanceID]` → `meshDescBuffer`
     → `load_vertex(mesh, id, t_BindlessBuffers[...])` 取顶点
   - Instance-based:`SV_InstanceID` → instance transform(和 PT 共用 instance_pool)
   - 真实材质解码:`materialHeaderBuffer[materialID]` → LUT → `decode_material_surface`
     → base_color / metalness / roughness
   - **已验证**:RedMaterial (UsdPreviewSurface, diffuseColor=(0.8,0.2,0.2)) 渲染出
     albedo=(0.8000,0.2000,0.2000),完全正确

3. **deferred_direct_lighting 节点** —— Lambertian 直接光:
   - 读 G-Buffer SRV(Position/Normal/MaterialID/Albedo)
   - decode 材质 → base_color
   - 遍历 lightBuffer,Point/Distant 光型,算 `albedo/π × intensity × NdotL`
   - **已验证**:512 像素被照亮,RGB 比例 4:1:1 匹配 (0.8,0.2,0.2)
   - 无 shadow(deferred 无 TLAS,合理取舍)

## 根因修复记录(本次 session 的核心突破)

### Bug 1: slang 无 `register()` 注解 → VS+PS 合并编译 slot 分配不确定

**症状:** materialBlobBuffer 绑定正确(validation 显示 bound),buffer 有正确数据
(staging readback 确认 0x3F4CCCCD=0.8),但 PS 读出全 0。materialHeaderBuffer(16B)
能读,materialBlobBuffer(1024B)读不到。

**根因:** `MaterialEvaluation.slang` 声明 `StructuredBuffer<MaterialDataBlob>
materialBlobBuffer;` 时**没有 `register()` 注解**。slang 在 VS+PS 合并编译时,
对无 `register()` 的资源做自动 slot 分配 —— 这个分配是**非确定性的**:合并反射
报告一个 slot,但 PS 的实际 shader bytecode 里 binding 可能在不同 slot。结果:
ProgramVars 按合并反射的 slot 绑定,但 PS 读的是另一个 slot(空)→ 读到 0。

**修复:** 在 `MaterialEvaluation.slang` 加显式 `register()`:
```slang
StructuredBuffer<MaterialDataBlob> materialBlobBuffer : register(t3);
StructuredBuffer<MaterialHeader> materialHeaderBuffer : register(t4);
StructuredBuffer<uint> materialTypeLUT : register(t5);
```
这同时修了 materialBlobBuffer 和 materialTypeLUT 的读取(LUT 之前 CPU 写 1 GPU 读 0
也是同一个 slot 分配问题)。

### Bug 2: rasterize C++ 重复 `finish_setting_vars()` 

**症状:** binding 可能不稳定 / 偶发 device removed。

**根因:** `node_render_rasterize.cpp` 调了两次 `finish_setting_vars()`,中间重复
bind 了 `t_BindlessBuffers` + 错误 bind 了 `t_BindlessTextures`(shader 没声明,
被静默丢弃)。第二次 finish 会销毁重建所有 binding set。

**修复:** 对齐 `path_tracing.cpp` 的写法 —— bind 全部资源后只调一次 finish。

### Bug 3: deferred CS register 冲突 + RGBA16_FLOAT readback segfault

**症状:** deferred CS dispatch 时 device removed / segfault。

**根因 A:** MaterialEvaluation 的 `register(t3/t4/t5)` 和 deferred CS 的
G-Buffer texture `register(t3/t4/t5)` 冲突 → root signature overlap。
**修复:** deferred CS 的 G-Buffer textures 移到 `register(t10-t15)`。

**根因 B:** deferred output 用 RGBA16_FLOAT,但 `get_output_texture()`
(`renderer.cpp:283`) 硬编码假设每像素 16 bytes(4×float32),只有 RGBA32_FLOAT
匹配。RGBA16_FLOAT(8B/像素)的 row-copy 越界 → segfault。
**修复:** deferred output 改用 RGBA32_FLOAT。

## 踩过的坑(耗时很长,记录避免重复)

1. **shader cache staleness**:`Binaries/Release/shader_cache` 缓存编译产物 +
   reflection。改 `.slang` 后必须 `rm -rf`,否则 binding 不一致 → 各种红鲱鱼崩溃。
2. **shader runtime 路径**:`nodes/shaders/shaders/*` ≠
   `Binaries/Release/usd/hd_RUZINO/resources/shaders/*`。新 .slang 要么重建
   hd_RUZINO(后构建步骤拷贝),要么手动 `cp`。
3. **slang 无 register() 注解 → VS+PS slot 不确定**(见根因修复 Bug 1)。
   教训:跨 stage 共享的 buffer 声明**必须**加显式 `register()` 注解。
4. **VS+PS 合并反射丢弃 PS-only imported 资源**:只在 PS import 的模块里声明的
   资源不出现在合并反射里。修复:VS 和 PS 都 import 同一个模块。
5. **R32_UINT render target 不能 `clearTextureFloat`**。MaterialID target 用
   R32_FLOAT + `float(uint)` 打包。不能用 `asfloat`/`asuint` round-trip
   (D3D12 下崩 CS)。
6. **blob 字段布局不是 struct 布局**:MaterialX 生成器把每个 uniform 按
   声明顺序逐个 `asfloat(data.data[i])` 写,不是按 C struct 布局。
   `reinterpret<Struct>(blob)` 读到垃圾。必须按生成顺序的 index 逐字段读。
7. **material_type_id 是 pool index 不是枚举**:要建 C 侧 LUT 扫材质 shader
   源码(`shader_type_id = 0/1` 标记)做 pool-index → enum 映射。
8. **`get_output_texture()` 硬编码 16B/像素**:只支持 RGBA32_FLOAT。
   RGBA16_FLOAT 会 segfault。所有需要 readback 的 render target 用 RGBA32_FLOAT。
9. **`hd_RUZINO.dll` / 节点 DLL 锁**:重建前要 kill 占用 DLL 的 python 进程。
10. **DistantLight `dirW` = 变换矩阵的 Z 轴**(`light.cpp:275` 取 `transform.GetRow(2)`)。
    默认恒等变换 → `dirW=(0,0,1)` → `-dirW=(0,0,-1)` 指向远离相机 → 朝相机的
    表面永远 `NdotL<0` 全黑。要让光从相机侧照来,必须旋转使 `dirW.Z < 0`。
    调试方法:在 deferred CS 里把 `NdotL`/`L.z`/`N.z` 编码进输出通道读回。
11. **USD 灯光的 `intensity` 必须写 `inputs:intensity`**:少了 `inputs:` 前缀
    属性被 schema 忽略,fallback 到 `UsdLuxDistantLight` 默认值 **50000**
    (近似太阳)。`float intensity = 1.0` 会静默变成 50000 → 渲染全白。
    正确写法:`float inputs:intensity = 1.0`。

## 文件清单

| 文件 | 类型 | 说明 |
|------|------|------|
| `nodes/node_render_rasterize.cpp` | 改 | G-Buffer rasterize 节点(真实材质 albedo) |
| `nodes/node_deferred_direct_lighting.cpp` | 改 | deferred 直接光节点(Lambertian) |
| `nodes/shaders/shaders/rasterize.vs.slang` | 改 | GPU-driven 顶点着色器 |
| `nodes/shaders/shaders/rasterize.ps.slang` | 改 | G-Buffer 像素着色器(真实 decode) |
| `nodes/shaders/shaders/Scene/MaterialEvaluation.slang` | 改 | 材质解码(+register 注解修复) |
| `nodes/shaders/shaders/Scene/MaterialBSDF.slang` | 新 | BSDF 评估(纯函数,WIP:reinterpret 需改) |
| `nodes/shaders/shaders/light_sampling.slang` | 新 | 共享光照几何采样 |
| `nodes/shaders/shaders/deferred_direct_lighting.cs.slang` | 改 | deferred compute shader(直接光) |
| `tests/test_raster_pipeline.py` | 改 | raster pipeline 测试(3 passed) |

## 已知遗留 / TODO

1. **MaterialBSDF.slang 用 `reinterpret` 读 blob**:应改为 sequential `asfloat`
   读法(和 MaterialEvaluation.slang 的 `_decode_*` 一致)。当前 deferred 用
   Lambertian 近似(albedo/π),不走 BSDF,所以暂不影响。改了之后可以接入
   完整 per-material BSDF(specular 等)。
2. **buffer leak**:`materialTypeLUT` 不在 `MARK_DESTROY_NVRHI_RESOURCE` 追踪里
   (`has_storage = false`)。低优先级 —— 每次 cook 重建但不释放旧 buffer。
3. **无 shadow**:deferred 直接光穿透遮挡物。后续可加 RT shadow pass 或
   screen-space 近似。
4. **texture sampling 缺失**:材质纹理暂不支持(纯色参数)。需 raster link
   per-material texture fetch,同 path_tracing.cpp 的纹理收集逻辑。
5. **光强单位**:deferred Lambertian 输出值偏大(DistantLight intensity 直接乘
   albedo/π,没有 radiance→irradiance 转换)。需校准光强单位。
