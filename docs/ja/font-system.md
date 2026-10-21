# フォントシステム

## 概要

フォントはコンパイルのパイプラインを通ります。TTF ソースファイル -> ベイク済みの BitmapFont (.dfont) + テクスチャアトラス、という流れです。フォントサイズごとに、決定的な GUID を持つ**別々のファイルの組**が生成されます。

```
assets/fonts/MyFont.ttf (source)
  -> cache/{variantGuid}  (compiled .dfont for 16px)
  -> cache/{atlasGuid}    (texture atlas for 16px)
  -> cache/{variantGuid2} (compiled .dfont for 24px)
  -> cache/{atlasGuid2}   (texture atlas for 24px)
```

## GUID の生成

フォントのバリアントの GUID は、ソースの GUID とフォントサイズから**決定的に**決まります。

```
variantGuid = GenerateDeterministicGuid(sourceGuid + ":" + fontSize)
atlasGuid   = GenerateDeterministicGuid(sourceGuid + ":" + fontSize + ":atlas")
```

つまり、次のようになります。
- 同じソースとサイズからは常に同じ GUID が生成されます
- 参照を壊さずにキャッシュを一から作り直せます
- サイズが異なれば衝突することはありません

## .data サイドカーファイル

各フォントには `.data` という JSON のサイドカーがあります (例: `MyFont.ttf.data`)。

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

- `fontSettings.sizes`: このフォントに設定されているサイズ
- `variants`: 各サイズを、そのバリアントとアトラスの GUID に対応付けます (常に決定的に再生成されます)

## ベイクの担当: FontSyncHandler

**ファイル:** `../../modules/deki-2d/editor/FontSyncHandler.cpp`

FontSyncHandler はフォントのベイクを担う**唯一の担当**です。ベイクの要求はすべてここを通ります。

```
EnsureFontSizeBaked(sourceGuid, fontSize)
  -> Updates .data file with the requested size
  -> Calls pipeline->RefreshAsset() to trigger HandleFontSync
  -> HandleFontSync reads .data, compiles TTF for each size
  -> Writes cache/{variantGuid} (dfont) and cache/{atlasGuid} (atlas)
  -> Registers variant GUID with AssetManager
```

### EnsureFontSizeBaked を呼び出すもの:
- `TextComponentEditor.cpp` - フォントプレビューの「保存して使用」ボタン
- `AssetPropertyEditor.cpp` - アセットのプロパティエディターの「Save & Use」ボタン

### 登録
ベイクの後、FontSyncHandler はバリアントを AssetManager に登録します。
```cpp
AssetManager::Get()->RegisterGuid(variantGuid, variantGuid);
```
これにより、エディターで `LookupPath(variantGuid)` -> `cacheDir/variantGuid` が使えるようになります。

## フォントの AssetRef フィールド

TextComponent では次のようになります。
```
font.guid   = variant GUID (baked .dfont file for a specific size)
font.source = TTF source GUID (the original font file)
```

`font.Get()` は `font.guid` -> `AssetManager::LoadByGuidAndType` -> `BitmapFont::Load` の順に読み込みます。

### font.guid が同期される仕組み

`TextComponent::RenderContent` (エディターモード) では次のようにしています。
```cpp
std::string expectedGuid = GenerateDeterministicGuid(font.source + ":" + std::to_string(fontSize));
if (font.guid != expectedGuid) {
    font.guid = expectedGuid;
    font.ptr = nullptr;
    font.loadAttempted = false;
}
```

これにより、ユーザーがインスペクターで `fontSize` を変更すると、次の描画フレームで正しいバリアントが読み込まれます。

## プレビューモード (エディターのみ)

エディターでは、ディスクにベイクせずに任意のサイズでフォントをプレビューできます。

1. ユーザーが TextComponentEditor でプレビューを有効にします
2. `GetEditorFontVariant()` が TTF をその場でメモリ上の BitmapFont にコンパイルします
3. プレビューの枠は 1 つだけで、同時に存在するプレビューは 1 つです
4. キャッシュには保存されず、試すためだけのものです
5. 「保存して使用」ボタンで、プレビューしたサイズを `EnsureFontSizeBaked` によってベイクします

## エクスポートのパイプライン

`AssetExporter::ExportToStorage()` の間に次の処理が行われます。

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

## BitmapFont のファイル形式 (.dfont)

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

dfont 内に格納されるアトラスのパスは、dfont ファイルのディレクトリからの相対パスです。両方のファイルは同じ cache/ または storage/ ディレクトリにあるので、パスは atlasGuid の文字列そのものになります。

## 主なファイル

| ファイル | 用途 |
|------|---------|
| `../../modules/deki-2d/TextComponent.h` | fontSize、previewEnabled、previewSize のフィールド |
| `../../modules/deki-2d/TextComponent.cpp` | RenderContent でのフォントの読み込み、プレビューのコンパイル |
| `../../modules/deki-2d/BitmapFont.cpp` | フォントの読み込み、グリフの描画 |
| `../../modules/deki-2d/editor/FontSyncHandler.cpp` | ベイクの担当 |
| `../../modules/deki-2d/editor/FontCompiler.cpp` | TTF -> BitmapFont のコンパイル |
| `../../modules/deki-2d/editor/TextComponentEditor.cpp` | インスペクターの UI、フォントサイズのドロップダウン |
| `src/services/AssetExporter.cpp` | エクスポート時のベイクとコピー |
