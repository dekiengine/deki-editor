# 精灵系统

## 概述

精灵表要经过一条导入管线：PNG 源文件 -> 内嵌帧元数据的已编译 .tex 文件。所有帧都作为区域存在于**父 .tex 文件内部**，不会为每一帧生成单独的文件。

```
assets/sprites/player.png (source)
  -> cache/{parentGuid}  (compiled .tex with FrameList chunk)
```

与字体（每个字号生成一个单独的文件）不同，精灵帧是单张纹理的子区域。父 .tex 文件同时包含像素数据和一个列出所有帧坐标的 FrameList 元数据块。

## GUID 生成

帧 GUID 由父 GUID + 帧索引**确定性地**生成：

```
frameGuid = GenerateDeterministicGuid(parentGuid + ":" + frameIndex)
```

这意味着：
- 相同的父资源 + 索引始终生成相同的帧 GUID
- 缓存可以从头重建，而不会破坏引用
- 不同的帧绝不会冲突

## .tex 文件格式（2DTX）

```
[Header - 32 bytes]
  magic: "2DTX" (4 bytes)
  version: uint32_t
  width, height: uint32_t
  format: uint32_t (RGB565, RGB565A8, RGBA8888, ALPHA8)
  dataSize: uint32_t
  metadataSize: uint32_t
  flags: uint32_t (HasTransparency, HasAlpha, IsSprite)

[Pixel Data]
  Raw pixel data in the specified format

[Metadata Chunks]
  numChunks: uint32_t

  For each chunk:
    chunkType: uint32_t (1=Sprite, 2=FrameList, 3=ChromaKey, 4=SourceSize)
    chunkSize: uint32_t
    ...chunk data...
```

### Sprite 块（类型 1）

基于网格的精灵表存储默认的帧尺寸：

```
frameWidth:  int32_t
frameHeight: int32_t
```

### FrameList 块（类型 2）

所有帧及其 GUID 和坐标：

```
frameCount: uint16_t

For each frame (52 bytes):
  guid:   36 bytes (no null terminator)
  x:      int32_t
  y:      int32_t
  width:  int32_t
  height: int32_t
```

### SourceSize 块（类型 4）

仅当纹理的存储尺寸小于其图像时存在（最大大小）：

```
width:  uint32_t   the image's size
height: uint32_t
```

Sprite 块和 FrameList 块、九宫格边框以及瓦片集都仍以图像的像素表示。`Sprite::ApplySourceSize` 在加载时将帧、边框和 `pixelsPerMeter` 换算为存储后的像素，因此精灵在世界中保持其尺寸；瓦片集用 `Sprite::SourceToStoredX/Y` 映射其瓦片矩形。两者都对矩形边缘使用 round(v x stored / image)，即编码器重新采样每一帧时使用的规则，因此原本相接的帧仍然相接，也不会有帧渗入其他帧。

## .data 附属文件

每个精灵表都可以有一个 `.data` JSON 附属文件（例如 `player.png.data`）：

```json
{
  "guid": "random-uuid-for-png-file",
  "slicing": {
    "mode": "grid",
    "frameWidth": 32,
    "frameHeight": 32
  }
}
```

图集模式（自定义帧定义）：

```json
{
  "guid": "random-uuid-for-png-file",
  "slicing": {
    "mode": "atlas",
    "frames": [
      { "x": 0, "y": 0, "width": 32, "height": 48 },
      { "x": 32, "y": 0, "width": 24, "height": 48 }
    ]
  }
}
```

## 导入管线：TextureImporter

**文件：** `deki-editor/src/TextureImporter.cpp`

TextureImporter 负责带帧元数据的 PNG -> .tex 转换：

### 两遍导入

**第 1 遍：发现**
- 扫描 assets/ 中的所有图像文件
- 为每一帧生成确定性的 SubAsset GUID
- 在 `s_FrameDataRegistry` 中注册帧坐标

**第 2 遍：编译**
- 通过 stb_image 加载源图像
- 转换为目标格式（RGB565 等）
- 写入包含头部 + 像素数据 + 元数据块的 .tex 文件
- FrameList 块包含所有帧 GUID + 坐标

### SubAsset 生成

```cpp
// Grid mode
for (row = 0; row < rows; row++) {
    for (col = 0; col < cols; col++) {
        int index = row * cols + col;
        subAsset.guid = GenerateDeterministicGuid(parentGuid + ":" + std::to_string(index));
        subAsset.parentGuid = parentGuid;
        subAsset.subAssetIndex = index;
    }
}
```

### 帧数据注册表

用于在编辑器中快速查找帧坐标的内存缓存：

```cpp
static std::unordered_map<std::string, SpriteFrameData> s_FrameDataRegistry;

// API:
TextureImporter::RegisterFrameData(frameGuid, data);
TextureImporter::GetFrameData(frameGuid);
TextureImporter::ClearFrameData();
```

## 精灵的 AssetRef 字段

在 SpriteComponent 上：

```
sprite.guid   = parent sprite GUID (the .tex file to load)
sprite.source = frame GUID (which sub-region to render)
```

### 为什么 guid = 父资源，source = 帧

精灵帧是**同一个文件中的区域**，而不是单独的文件。父 .tex 在其 FrameList 块中内嵌了所有帧。因此：

- `sprite.guid` 必须指向父 .tex（要从磁盘加载的实际文件）
- `sprite.source` 标识该文件中的哪个帧区域

这与字体不同，字体的每个变体都是单独的文件（`font.guid` = 变体文件，`font.source` = TTF 源文件）。

### 普通精灵（没有帧）

```
sprite.guid   = sprite GUID (the .tex file)
sprite.source = (empty)
```

## 帧解析流程

```
1. Scene loaded from MessagePack
   -> SpriteComponent.Deserialize()
   -> sprite.guid = "parent-sprite-guid"
   -> sprite.source = "frame-guid" (or empty)

2. SpriteComponent::ResolveObjectRefs()
   -> sprite.Get() loads parent .tex via AssetManager
   -> Sprite::Load() parses FrameList chunk -> populates frames vector

3. Choose the meaningful GUID:
   -> If sprite.source is empty: use sprite.guid (full texture)
   -> If sprite.source is set:  use sprite.source (specific frame)
   -> Call OnAssetRefResolved(propertyName, loadedSprite, frameGuid)

4. SpriteComponent::OnAssetRefResolved()
   -> Sprite::FindFrame(frameGuid) -> linear search in frames vector
   -> SetFrameRect(frame->x, frame->y, frame->width, frame->height)

5. SpriteComponent::RenderContent()
   -> Uses frame_x, frame_y, frame_width, frame_height for QuadBlit source rect
```

### 生成的 ResolveObjectRefs

代码生成器生成如下模式：

```cpp
void SpriteComponent::ResolveObjectRefs(Scene* scene) {
    if (auto* _loaded_sprite = sprite.Get()) {
        const char* _frameGuid = sprite.source.empty()
            ? sprite.guid.c_str()
            : sprite.source.c_str();
        OnAssetRefResolved("sprite", _loaded_sprite, _frameGuid);
    }
}
```

这样 `OnAssetRefResolved` 就能收到**有意义的标识符**（精灵表为帧 GUID，普通精灵为精灵 GUID）。

### FindFrame

```cpp
const SpriteFrame* Sprite::FindFrame(const std::string& guid) const {
    for (size_t i = 0; i < frames.size(); i++) {
        if (guid == frames[i].guid) return &frames[i];
    }
    return nullptr;
}
```

线性查找：帧数量很少（通常 < 100）。

## 在编辑器中指定

**文件：** `src/panels/inspector/AssetPropertyEditor.cpp`

当用户在检视器中将一帧拖放到 SpriteComponent 上时：

```cpp
// Frame dropped (SubAsset)
assetRefBase.guid = subAsset->parentGuid;   // Parent for loading
assetRefBase.source = droppedGuid;          // Frame for coordinate lookup

// Plain sprite dropped
assetRefBase.guid = droppedGuid;            // Sprite for loading
assetRefBase.source = "";                   // No frame
```

## GUID 注册（编辑器）

SubAsset GUID 会注册到 AssetManager，以便 `LookupPath` 能够工作：

```cpp
// EditorApp.cpp
AssetManager::Get()->RegisterGuid(subAsset.guid, info.guid);
// Maps: frameGuid -> parentGuid
// LookupPath(frameGuid) -> cacheDir/parentGuid -> loads parent .tex
```

## 导出管线

在 `AssetExporter::ExportToStorage()` 期间：

```
1. CollectUsedAssets()
   -> Scans all scenes for SpriteComponents
   -> Collects parent GUIDs (sprite.guid)

2. CopyUsedAssetsToOutput()
   -> Copies cache/{parentGuid} -> storage/{parentGuid}
   -> Only the parent .tex is copied (frames are embedded)

3. ExportAssetLookupTable()
   -> Adds parent GUID to binary lookup table
   -> Frame GUIDs are NOT in the lookup table
   -> At runtime, frames are found via FindFrame() after loading parent
```

**与字体的关键区别：** 字体变体是单独的文件，因此需要在查找表中有单独的条目。精灵帧则不需要：它们在父资源加载后通过 `FindFrame()` 找到。

## 比较：字体与精灵

| 方面 | 字体 | 精灵 |
|--------|-------|---------|
| 虚拟资源 | 单独的文件（变体 + 图集） | 同一个文件中的区域 |
| `guid` 字段 | 变体 GUID（已烘焙的文件） | 父 GUID（.tex 文件） |
| `source` 字段 | TTF 源文件 GUID | 帧 GUID |
| 确定性种子 | `source + ":" + size` | `parent + ":" + index` |
| 导出的文件 | 每个字号一个变体 + 一个图集 | 每个精灵表一个 .tex |
| 查找表条目 | 变体 + 图集 GUID | 仅父 GUID |
| 运行时解析 | `LookupPath(variantGuid)` -> 文件 | `LookupPath(parentGuid)` -> 文件，然后 `FindFrame(frameGuid)` |

## 关键文件

| 文件 | 用途 |
|------|---------|
| `deki-editor/src/TextureImporter.cpp` | PNG -> .tex 转换、SubAsset 生成 |
| `deki-editor/include/deki-editor/TextureImporter.h` | TextureImporter 接口、SpriteFrameData |
| `../../modules/deki-2d/Sprite.h` | Sprite 类、SpriteFrame 结构体、FindFrame |
| `../../modules/deki-2d/Sprite.cpp` | Sprite 加载、FrameList 解析 |
| `../../modules/deki-2d/SpriteComponent.h` | 带帧矩形字段的 SpriteComponent |
| `../../modules/deki-2d/generated/Deki2DReflection0.gen.cpp` | 生成的、使用 source 模式的 ResolveObjectRefs（每个包一个文件，而不是每个组件一个） |
| `src/panels/inspector/AssetPropertyEditor.cpp` | 帧指定 UI |
| `src/services/AssetExporter.cpp` | 导出管线 |
