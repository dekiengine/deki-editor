# スプライトシステム

## 概要

スプライトシートはインポートのパイプラインを通ります。PNG ソースファイル -> フレームのメタデータを埋め込んだコンパイル済みの .tex ファイル、という流れです。すべてのフレームは領域として**親の .tex ファイルの中**にあり、フレームごとに別のファイルが生成されることはありません。

```
assets/sprites/player.png (source)
  -> cache/{parentGuid}  (compiled .tex with FrameList chunk)
```

フォント (サイズごとに別のファイルが生成される) と異なり、スプライトのフレームは 1 枚のテクスチャの部分領域です。親の .tex ファイルには、ピクセルデータと、すべてのフレームの座標を列挙した FrameList メタデータチャンクの両方が含まれています。

## GUID の生成

フレームの GUID は、親の GUID とフレームのインデックスから**決定的に**決まります。

```
frameGuid = GenerateDeterministicGuid(parentGuid + ":" + frameIndex)
```

つまり、次のようになります。
- 同じ親とインデックスからは常に同じフレームの GUID が生成されます
- 参照を壊さずにキャッシュを一から作り直せます
- フレームが異なれば衝突することはありません

## .tex ファイルの形式 (2DTX)

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

### Sprite チャンク (type 1)

グリッドベースのスプライトシートは、デフォルトのフレームの寸法を保存します。

```
frameWidth:  int32_t
frameHeight: int32_t
```

### FrameList チャンク (type 2)

すべてのフレームとその GUID および座標です。

```
frameCount: uint16_t

For each frame (52 bytes):
  guid:   36 bytes (no null terminator)
  x:      int32_t
  y:      int32_t
  width:  int32_t
  height: int32_t
```

### SourceSize チャンク (type 4)

テクスチャが元の画像より小さく保存されている場合 (最大サイズ) にだけ存在します。

```
width:  uint32_t   the image's size
height: uint32_t
```

Sprite チャンクと FrameList チャンク、9 スライスの境界、タイルセットは、画像のピクセル単位のままです。`Sprite::ApplySourceSize` は読み込み時にフレーム、境界、`pixelsPerMeter` を保存されたピクセルに合わせるので、スプライトはワールドでのサイズを保ちます。タイルセットは `Sprite::SourceToStoredX/Y` でタイルの矩形を対応付けます。どちらも矩形の辺に round(v x stored / image) を使います。これはエンコーダーが各フレームをリサンプリングしたときと同じ規則なので、接しているフレームは接したままで、どのフレームもほかのフレームににじみません。

## .data サイドカーファイル

各スプライトシートは `.data` という JSON のサイドカーを持てます (例: `player.png.data`)。

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

アトラスモード (フレームを個別に定義) の場合:

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

## インポートのパイプライン: TextureImporter

**ファイル:** `deki-editor/src/TextureImporter.cpp`

TextureImporter は、フレームのメタデータを含めて PNG -> .tex の変換を行います。

### 2 パスのインポート

**パス 1: 検出**
- assets/ 内のすべての画像ファイルを走査します
- 各フレームの決定的な SubAsset の GUID を生成します
- フレームの座標を `s_FrameDataRegistry` に登録します

**パス 2: コンパイル**
- stb_image でソース画像を読み込みます
- ターゲットのフォーマット (RGB565 など) に変換します
- ヘッダー + ピクセルデータ + メタデータチャンクで .tex ファイルを書き出します
- FrameList チャンクには、すべてのフレームの GUID と座標が含まれます

### SubAsset の生成

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

### フレームデータのレジストリ

エディターでフレームの座標をすばやく検索するためのメモリ上のキャッシュです。

```cpp
static std::unordered_map<std::string, SpriteFrameData> s_FrameDataRegistry;

// API:
TextureImporter::RegisterFrameData(frameGuid, data);
TextureImporter::GetFrameData(frameGuid);
TextureImporter::ClearFrameData();
```

## スプライトの AssetRef フィールド

SpriteComponent では次のようになります。

```
sprite.guid   = parent sprite GUID (the .tex file to load)
sprite.source = frame GUID (which sub-region to render)
```

### guid = 親、source = フレームである理由

スプライトのフレームは別々のファイルではなく、**1 つのファイル内の領域**です。親の .tex には、すべてのフレームが FrameList チャンクに埋め込まれています。そのため、次のようになります。

- `sprite.guid` は親の .tex (ディスクから実際に読み込むファイル) を指す必要があります
- `sprite.source` はそのファイル内のどのフレームの領域かを特定します

これは、各バリアントが別のファイルであるフォント (`font.guid` = バリアントのファイル、`font.source` = TTF ソース) とは異なります。

### 単体のスプライト (フレームなし) の場合

```
sprite.guid   = sprite GUID (the .tex file)
sprite.source = (empty)
```

## フレームの解決の流れ

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

### 生成される ResolveObjectRefs

コードジェネレーターは次のパターンを生成します。

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

これにより、`OnAssetRefResolved` は**意味のある識別子** (スプライトシートならフレームの GUID、単体のスプライトならスプライトの GUID) を受け取ります。

### FindFrame

```cpp
const SpriteFrame* Sprite::FindFrame(const std::string& guid) const {
    for (size_t i = 0; i < frames.size(); i++) {
        if (guid == frames[i].guid) return &frames[i];
    }
    return nullptr;
}
```

線形探索です。フレーム数は少ない (通常 100 未満) ためです。

## エディターでの割り当て

**ファイル:** `src/panels/inspector/AssetPropertyEditor.cpp`

ユーザーがインスペクターで SpriteComponent にフレームをドロップしたとき:

```cpp
// Frame dropped (SubAsset)
assetRefBase.guid = subAsset->parentGuid;   // Parent for loading
assetRefBase.source = droppedGuid;          // Frame for coordinate lookup

// Plain sprite dropped
assetRefBase.guid = droppedGuid;            // Sprite for loading
assetRefBase.source = "";                   // No frame
```

## GUID の登録 (エディター)

`LookupPath` が機能するように、SubAsset の GUID は AssetManager に登録されます。

```cpp
// EditorApp.cpp
AssetManager::Get()->RegisterGuid(subAsset.guid, info.guid);
// Maps: frameGuid -> parentGuid
// LookupPath(frameGuid) -> cacheDir/parentGuid -> loads parent .tex
```

## エクスポートのパイプライン

`AssetExporter::ExportToStorage()` の間に次の処理が行われます。

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

**フォントとの主な違い:** フォントのバリアントは別々のファイルなので、検索テーブルにそれぞれの項目が必要です。スプライトのフレームは必要ありません。親が読み込まれた後に `FindFrame()` で見つかります。

## 比較: フォントとスプライト

| 観点 | フォント | スプライト |
|--------|-------|---------|
| 仮想アセット | 別々のファイル (バリアント + アトラス) | 1 つのファイル内の領域 |
| `guid` フィールド | バリアントの GUID (ベイク済みファイル) | 親の GUID (.tex ファイル) |
| `source` フィールド | TTF ソースの GUID | フレームの GUID |
| 決定的なシード | `source + ":" + size` | `parent + ":" + index` |
| エクスポートされるファイル | サイズごとにバリアント 1 つとアトラス 1 つ | スプライトシートごとに .tex 1 つ |
| 検索テーブルの項目 | バリアントとアトラスの GUID | 親の GUID のみ |
| 実行時の解決 | `LookupPath(variantGuid)` -> ファイル | `LookupPath(parentGuid)` -> ファイル、その後 `FindFrame(frameGuid)` |

## 主なファイル

| ファイル | 用途 |
|------|---------|
| `deki-editor/src/TextureImporter.cpp` | PNG -> .tex の変換、SubAsset の生成 |
| `deki-editor/include/deki-editor/TextureImporter.h` | TextureImporter のインターフェース、SpriteFrameData |
| `../../modules/deki-2d/Sprite.h` | Sprite クラス、SpriteFrame 構造体、FindFrame |
| `../../modules/deki-2d/Sprite.cpp` | スプライトの読み込み、FrameList の解析 |
| `../../modules/deki-2d/SpriteComponent.h` | フレームの矩形のフィールドを持つ SpriteComponent |
| `../../modules/deki-2d/generated/Deki2DReflection0.gen.cpp` | source のパターンを含む生成済みの ResolveObjectRefs (コンポーネントごとではなく、パッケージごとに 1 ファイル) |
| `src/panels/inspector/AssetPropertyEditor.cpp` | フレームの割り当ての UI |
| `src/services/AssetExporter.cpp` | エクスポートのパイプライン |
