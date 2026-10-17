# 资源系统

## AssetRef\<T\> 模板

**文件：** `deki-engine/include/deki/assets/AssetRef.h`

所有引用资源的组件都使用 `AssetRef<T>`。它有四个字段：

| 字段 | 类型 | 用途 |
|-------|------|---------|
| `guid` | `std::string` | 用于**从磁盘加载资源**的 GUID。始终指向一个实际存在的文件。 |
| `source` | `std::string` | 含义取决于上下文的元数据。参见下文的“双字段模式”。 |
| `ptr` | `void*` | 指向已加载资源的缓存指针。在第一次调用 `Get()` 时设置。 |
| `loadAttempted` | `bool` | 防止反复重试。第一次尝试加载后设为 `true`。 |

### 延迟加载

`Get()` 会触发一次性加载：

```
if (!ptr && !guid.empty() && !loadAttempted) {
    loadAttempted = true;
    ptr = static_cast<T*>(AssetManager::Get()->LoadByGuidAndType(guid, T::kAssetTypeName));
}
return ptr;
```

修改 `guid` 后如需强制重新加载，必须同时清除 `ptr` 和 `loadAttempted`：
```cpp
ref.guid = newGuid;
ref.ptr = nullptr;
ref.loadAttempted = false;
```

## 双字段模式（guid 与 source）

`guid` 和 `source` 字段的**含义因资源类型而异**。这是有意为之，反映了架构上的真实差异。

### 规则：`guid` 始终表示“要从磁盘加载的文件”

| 资源类型 | `guid` | `source` | 原因 |
|------------|--------|----------|-----|
| **精灵**（普通） | 精灵 GUID | （空） | 单个文件，没有间接引用 |
| **精灵**（帧） | **父**精灵 GUID | **帧** GUID | 父 .tex 文件包含所有帧。`guid` 用于加载文件，`source` 用于标识帧所在的区域。 |
| **字体** | **变体** GUID（已烘焙） | **TTF** 源文件 GUID | 每个字号都是单独的文件。`guid` 直接加载已烘焙的变体。 |
| **动画** | 动画 GUID | （空） | 单个文件 |

### 精灵和字体为何不同

- **字体变体是独立的文件。** 16px 和 24px 的字体会生成不同的 .dfont 和图集文件。变体 GUID 直接指向对应的文件。
- **精灵帧是同一个文件中的区域。** 第 0 帧和第 5 帧是同一个 .tex 文件的子区域。父 GUID 指向整张纹理。帧 GUID 只用于通过 `FindFrame()` 查找坐标。

### 生成代码的模式

代码生成器生成的 `ResolveObjectRefs` 如下：
```cpp
const char* _frameGuid = ref.source.empty() ? ref.guid.c_str() : ref.source.c_str();
OnAssetRefResolved(propertyName, loadedAsset, _frameGuid);
```

这样，无论**有意义的标识符**（精灵为帧 GUID，字体为 TTF 源文件）存放在哪个字段中，`OnAssetRefResolved` 都能收到它。

## 确定性 GUID 生成

虚拟资源和派生资源使用确定性 GUID，由 `GenerateDeterministicGuid()` 根据种子字符串生成（双 64 位 FNV-1a 哈希，格式化为 UUID）。

### 种子格式（绝对不要更改）

| 虚拟资源 | 种子格式 | 示例 |
|---------------|-------------|---------|
| 字体变体 | `sourceGuid + ":" + fontSize` | `"abc-123:16"` |
| 字体图集 | `sourceGuid + ":" + fontSize + ":atlas"` | `"abc-123:16:atlas"` |
| 精灵帧 | `parentGuid + ":" + frameIndex` | `"abc-123:0"` |
| 预览字体 | `"preview:" + sourceGuid + ":" + fontSize` | `"preview:abc-123:16"` |

`"preview:"` 前缀可防止与真正的字体变体 GUID 冲突。

### 生成确定性 GUID 的位置

- `deki-engine/src/Guid.cpp` - 核心实现
- `../../modules/deki-2d/editor/FontSyncHandler.cpp` - 字体烘焙
- `../../modules/deki-2d/TextComponent.cpp` - 编辑模式下的 font.guid 同步
- `src/services/AssetExporter.cpp` - 导出时烘焙
- `src/PlayMode.cpp` - 运行模式下的字体设置
- `deki-editor/src/AssetPipeline.cpp` - 缓存管理
- `deki-editor/src/TextureImporter.cpp` - 精灵帧生成

## 资源查找表（asset_table.bin）

用于运行时 O(log n) 查找的**二进制格式**：

```
[4 bytes]  entry count (uint32_t)
[N entries, 44 bytes each, sorted by hash]:
  [4 bytes]  FNV-1a hash of key (uint32_t)
  [37 bytes] GUID string (null-terminated)
  [3 bytes]  padding
```

### 哈希函数（FNV-1a 32 位）

```
offset_basis = 2166136261
prime = 16777619
```

**绝对不要更改这些常量。** 以下各处必须保持一致：
- `src/services/AssetExporter.cpp`（导出端）
- `deki-engine/include/deki/assets/AssetLookupTable.h`（运行时端）
- `deki-editor/src/AssetPipeline.cpp`（编辑器缓存）

### 表中的键类型

| 键格式 | 映射到 | 使用者 |
|------------|---------|---------|
| `"path:type"`（例如 `"textures/player:Sprite"`） | 资源 GUID | `AssetManager::Load<T>("path")` |
| GUID 字符串（例如 `"abc-123-..."`） | 同一 GUID | 已烘焙字体的直接 GUID 查找 |

### 表的两个版本

| 文件 | 范围 | 写入者 |
|------|-------|------------|
| `cache/asset_table.bin` | 项目的所有资源 | `AssetPipeline::UpdateAssetTable()` |
| `<build's assets>/asset_table.bin` | 仅用到的资源 | `AssetExporter::ExportAssetLookupTable()` |

如果目标将资源保存在内部存储中，构建的资源会导出到其引导数据旁边（`assets/`，以 `F:/assets/` 读取）；如果使用 SD 卡，则导出到 `<project>/storage/`（`S:/`）。`AssetManager::LoadAssetRoot` 从该位置加载查找表、资源包索引和所有资源；参见 [platforms.md](platforms.md#where-the-assets-go)。

## 资源加载流程

```
AssetRef::Get()
  -> AssetManager::LoadByGuidAndType(guid, T::kAssetTypeName)
    -> LookupPath(guid)
      -> Check binary asset_table.bin (binary search by FNV-1a hash)
      -> Check guidToPath_ map (runtime registrations)
      -> Return: cacheDirectory + "/" + resolved path
    -> Load<T>(resolvedPath)  // Sprite::Load, BitmapFont::Load, etc.
```

## 关键文件

| 文件 | 用途 |
|------|---------|
| `deki-engine/include/deki/assets/AssetRef.h` | AssetRef 模板 |
| `deki-engine/src/assets/AssetManager.cpp` | GUID 查找、资源加载 |
| `deki-engine/include/deki/assets/AssetLookupTable.h` | 二进制表格式 + FNV-1a |
| `deki-engine/src/Guid.h` / `.cpp` | 确定性 GUID 生成 |
| `deki-editor/src/AssetPipeline.cpp` | GUID 分配、缓存管理 |
| `src/services/AssetExporter.cpp` | 导出管线 |
