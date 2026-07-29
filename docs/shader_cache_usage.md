# Shader Cache 使用说明

## 概述

Shader缓存功能可以显著减少shader编译时间。系统会根据shader的特征（路径、源代码、宏定义等）计算hash值，并将编译后的二进制文件缓存到`shader_cache`目录中。

## 功能特性

1. **自动缓存管理**：编译成功的shader会自动缓存
2. **智能hash计算**：基于以下特征计算hash：
   - Shader类型（vertex, pixel, compute等）
   - Entry point名称
   - 文件路径
   - 源代码内容
   - 宏定义
   - 文件最后修改时间
   - 编译目标（SPIRV/DXIL）
   - 图形API后端（Vulkan/D3D12）

3. **缓存文件**：
   - `.spv` / `.dxil` - 编译后的shader二进制
   - `.meta` - Reflection信息（binding layouts等）

## 使用方法

### 启用/禁用缓存

```cpp
// 启用缓存（默认启用）
ShaderFactory::set_cache_enabled(true);

// 禁用缓存
ShaderFactory::set_cache_enabled(false);
```

### 缓存位置

缓存文件存储在：`<shader_search_path>/shader_cache/`

### 自动化流程

缓存功能已集成到`ShaderFactory::createProgram()`中：

1. 计算shader descriptor的hash
2. 检查缓存是否存在
3. 如果命中缓存，直接加载
4. 如果缓存未命中，编译shader并保存到缓存

### 缓存失效

当以下情况发生时，缓存会自动失效：
- Shader文件被修改（通过lastWriteTime检测）
- 宏定义改变
- Entry point改变
- Shader类型改变

## 性能提升

首次编译：正常编译时间
后续编译：几乎即时（仅文件I/O时间）

## 注意事项

1. **磁盘空间**：每个shader会占用一定磁盘空间
2. **手动清理**：可以手动删除`shader_cache`目录来清空缓存
3. **跨平台**：不同平台（Vulkan/D3D12）的缓存是分离的

## 技术细节

### CustomBlob实现

实现了自定义的`ISlangBlob`接口来从缓存中加载shader二进制数据：

```cpp
class CustomBlob : public ISlangBlob
{
private:
    std::vector<char> data;        // 存储shader二进制数据
    std::atomic<uint32_t> refCount; // COM风格引用计数

public:
    // 支持移动语义，避免不必要的数据拷贝
    explicit CustomBlob(std::vector<char>&& buffer);

    // 实现ISlangUnknown接口（COM标准）
    - queryInterface: 支持ISlangUnknown和ISlangBlob查询
    - addRef/release: 线程安全的引用计数管理

    // 实现ISlangBlob接口
    - getBufferPointer: 返回shader二进制数据指针
    - getBufferSize: 返回数据大小
};
```

**特性**：
- ✅ 线程安全的引用计数（使用`std::atomic`）
- ✅ 自动内存管理（引用计数为0时自动delete）
- ✅ 移动语义优化（避免大buffer拷贝）
- ✅ 完全符合Slang COM接口规范

### Hash算法

使用标准的hash combine方法：
```cpp
hash ^= std::hash<T>{}(value) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
```

### 缓存文件命名

格式：`<hash_hex>.<extension>`
- 示例：`a1b2c3d4.spv`, `e5f6a7b8.dxil`

### 元数据存储

- Binding spaces信息
- Binding locations映射
- 以二进制格式序列化
