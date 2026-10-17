# 字体系统

## 概述

字体要经过一条编译管线：TTF 源文件 -> 已烘焙的 BitmapFont（.dfont）+ 纹理图集。每个字号都会生成**一对单独的文件**，并使用确定性 GUID。

```
assets/fonts/MyFont.ttf (source)
  -> cache/{variantGuid}  (compiled .dfont for 16px)
  -> cache/{atlasGuid}    (texture atlas for 16px)
  -> cache/{variantGuid2} (compiled .dfont for 24px)
  -> cache/{atlasGuid2}   (texture atlas for 24px)
```

## GUID 生成

字体变体 GUID 由源文件 GUID + 字号**确定性地**生成：

```
variantGuid = GenerateDeterministicGuid(sourceGuid + ":" + fontSize)
atlasGuid   = GenerateDeterministicGuid(sourceGuid + ":" + fontSize + ":atlas")
```

这意味着：
- 相同的源文件 + 字号始终生成相同的 GUID
- 缓存可以从头重建，而不会破坏引用
- 不同字号之间绝不会冲突

## .data 附属文件

每个字体都有一个 `.data` JSON 附属文件（例如 `MyFont.ttf.data`）：

```json
{
  "guid": "random-uuid-for-ttf-file",
  "fontSettings": {
    "sizes": [12, 16, 24],
    "firstChar": 32,
    "lastChar": 126
  },
  "variants": {
    "12": { "guid": "deterministic-variant-guid", "atlasGuid": "deterministic-atlas-guid" },
    "16": { "guid": "...", "atlasGuid": "..." },
    "24": { "guid": "...", "atlasGuid": "..." }
  }
}
```

- `fontSettings.sizes`：为此字体配置了哪些字号
- `variants`：将每个字号映射到其变体 GUID 和图集 GUID（始终以确定性方式重新生成）

## 烘焙的唯一负责者：FontSyncHandler

**文件：** `../../modules/deki-2d/editor/FontSyncHandler.cpp`

FontSyncHandler 是字体烘焙的**唯一负责者**。所有烘焙请求都经过它：

```
EnsureFontSizeBaked(sourceGuid, fontSize)
  -> Updates .data file with the requested size
  -> Calls pipeline->RefreshAsset() to trigger HandleFontSync
  -> HandleFontSync reads .data, compiles TTF for each size
  -> Writes cache/{variantGuid} (dfont) and cache/{atlasGuid} (atlas)
  -> Registers variant GUID with AssetManager
```

### 调用 EnsureFontSizeBaked 的位置：
- `TextComponentEditor.cpp` - 字体预览中的“保存并使用”按钮
- `AssetPropertyEditor.cpp` - 资源属性编辑器中的“Save & Use”按钮

### 注册
烘焙完成后，FontSyncHandler 会向 AssetManager 注册该变体：
```cpp
AssetManager::Get()->RegisterGuid(variantGuid, variantGuid);
```
这样在编辑器中就可以通过 `LookupPath(variantGuid)` -> `cacheDir/variantGuid` 进行查找。

## 字体的 AssetRef 字段

在 TextComponent 上：
```
font.guid   = variant GUID (baked .dfont file for a specific size)
font.source = TTF source GUID (the original font file)
```

`font.Get()` 通过 `font.guid` -> `AssetManager::LoadByGuidAndType` -> `BitmapFont::Load` 进行加载。

### font.guid 如何保持同步

在 `TextComponent::RenderContent` 中（编辑器模式）：
```cpp
std::string expectedGuid = GenerateDeterministicGuid(font.source + ":" + std::to_string(fontSize));
if (font.guid != expectedGuid) {
    font.guid = expectedGuid;
    font.ptr = nullptr;
    font.loadAttempted = false;
}
```

这样，当用户在检视器中更改 `fontSize` 时，下一个渲染帧就会加载正确的变体。

## 预览模式（仅限编辑器）

编辑器支持以任意字号预览字体，而无需烘焙到磁盘：

1. 用户在 TextComponentEditor 中启用预览
2. `GetEditorFontVariant()` 按需将 TTF 编译为内存中的 BitmapFont
3. 只有一个预览槽位：同一时间只存在一个预览
4. 不会保存到缓存：纯粹用于测试
5. “保存并使用”按钮通过 `EnsureFontSizeBaked` 烘焙所预览的字号

## 导出管线

在 `AssetExporter::ExportToStorage()` 期间：

```
1. CollectUsedAssets()
   -> Scans all scenes for TextComponents
   -> CollectFontUsage() extracts font.source + fontSize
   -> Creates FontUsageInfo {sourceGuid, fontSize}

2. BakeFontsForExport(fontUsages)
   -> For each usage, generates deterministic variant + atlas GUIDs
   -> Checks cache; bakes if missing
   -> Returns map of key -> GUID for both variants and atlases
   -> Adds ALL GUIDs (variant + atlas) to usedGuids

3. CopyUsedAssetsToOutput()
   -> Copies cache/{guid} -> storage/{guid} for all used GUIDs

4. ExportAssetLookupTable()
   -> Adds variant + atlas GUIDs to binary lookup table
   -> Runtime resolves: hash(variantGuid) -> variantGuid -> storage/{variantGuid}
```

## BitmapFont 文件格式（.dfont）

```
[Header]
  magic: "DFNT" (4 bytes)
  version: uint8_t (currently 1)
  first_char, last_char: uint8_t
  line_height, baseline: uint8_t
  glyph_count: uint16_t
  atlas_path_len: uint16_t

[Glyph Data]
  glyph_count x GlyphInfo structs

[Atlas Path]
  Relative path to atlas file (e.g., just the atlasGuid string)
```

dfont 中存储的图集路径是相对于 dfont 文件所在目录的。由于这两个文件位于同一个 cache/ 或 storage/ 目录中，该路径就是 atlasGuid 字符串本身。

## 关键文件

| 文件 | 用途 |
|------|---------|
| `../../modules/deki-2d/TextComponent.h` | fontSize、previewEnabled、previewSize 字段 |
| `../../modules/deki-2d/TextComponent.cpp` | RenderContent 中的字体加载、预览编译 |
| `../../modules/deki-2d/BitmapFont.cpp` | 字体加载、字形渲染 |
| `../../modules/deki-2d/editor/FontSyncHandler.cpp` | 烘焙的唯一负责者 |
| `../../modules/deki-2d/editor/FontCompiler.cpp` | TTF -> BitmapFont 编译 |
| `../../modules/deki-2d/editor/TextComponentEditor.cpp` | 检视器 UI、字号下拉列表 |
| `src/services/AssetExporter.cpp` | 导出时的烘焙和复制 |
