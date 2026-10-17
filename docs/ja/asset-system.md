# アセットシステム

## AssetRef\<T\> テンプレート

**ファイル:** `deki-engine/include/deki/assets/AssetRef.h`

アセットを参照するコンポーネントはすべて `AssetRef<T>` を使います。フィールドは 4 つあります。

| フィールド | 型 | 用途 |
|-------|------|---------|
| `guid` | `std::string` | **ディスクからアセットを読み込む**ための GUID です。常に実在するファイルを指します。 |
| `source` | `std::string` | 文脈によって意味が変わるメタデータです。後述の「二重フィールドのパターン」を参照してください。 |
| `ptr` | `void*` | 読み込んだアセットへのキャッシュ済みポインターです。最初の `Get()` で設定されます。 |
| `loadAttempted` | `bool` | 再試行の繰り返しを防ぎます。最初の読み込みを試みた後に `true` になります。 |

### 遅延読み込み

`Get()` は 1 回だけ読み込みを行います。

```
if (!ptr && !guid.empty() && !loadAttempted) {
    loadAttempted = true;
    ptr = static_cast<T*>(AssetManager::Get()->LoadByGuidAndType(guid, T::kAssetTypeName));
}
return ptr;
```

`guid` を変更した後に強制的に読み込み直すには、`ptr` と `loadAttempted` の両方をクリアする必要があります。
```cpp
ref.guid = newGuid;
ref.ptr = nullptr;
ref.loadAttempted = false;
```

## 二重フィールドのパターン (guid と source)

`guid` と `source` のフィールドは、アセットの種類によって**意味が異なります**。これは意図したもので、アーキテクチャ上の実際の違いを反映しています。

### ルール: `guid` は常に「ディスクから読み込むファイル」

| アセットの種類 | `guid` | `source` | 理由 |
|------------|--------|----------|-----|
| **スプライト** (単体) | スプライトの GUID | (空) | 単一のファイルで、間接参照はありません |
| **スプライト** (フレーム) | **親**スプライトの GUID | **フレーム**の GUID | 親の .tex ファイルにすべてのフレームが含まれています。`guid` でファイルを読み込み、`source` でフレームの領域を特定します。 |
| **フォント** | **バリアント**の GUID (ベイク済み) | **TTF** ソースの GUID | フォントサイズごとに別のファイルです。`guid` でベイク済みのバリアントを直接読み込みます。 |
| **アニメーション** | アニメーションの GUID | (空) | 単一のファイル |

### スプライトとフォントが異なる理由

- **フォントのバリアントは別々のファイルです。** 16px と 24px のフォントは、それぞれ別の .dfont とアトラスのファイルを生成します。バリアントの GUID はそのファイルを直接指します。
- **スプライトのフレームは 1 つのファイル内の領域です。** フレーム 0 とフレーム 5 は同じ .tex ファイルの部分領域です。親の GUID はテクスチャ全体を指します。フレームの GUID は `FindFrame()` による座標の検索にだけ使われます。

### 生成されるコードのパターン

コードジェネレーターは次のような `ResolveObjectRefs` を生成します。
```cpp
const char* _frameGuid = ref.source.empty() ? ref.guid.c_str() : ref.source.c_str();
OnAssetRefResolved(propertyName, loadedAsset, _frameGuid);
```

これにより、どちらのフィールドに格納されていても、`OnAssetRefResolved` は**意味のある識別子** (スプライトならフレームの GUID、フォントなら TTF ソース) を受け取ります。

## 決定的な GUID の生成

仮想アセットや派生アセットは、`GenerateDeterministicGuid()` (64 ビット FNV-1a ハッシュを 2 つ使い、UUID 形式に整形) によってシード文字列から生成される決定的な GUID を使います。

### シードの形式 (絶対に変更しないでください)

| 仮想アセット | シードの形式 | 例 |
|---------------|-------------|---------|
| フォントのバリアント | `sourceGuid + ":" + fontSize` | `"abc-123:16"` |
| フォントのアトラス | `sourceGuid + ":" + fontSize + ":atlas"` | `"abc-123:16:atlas"` |
| スプライトのフレーム | `parentGuid + ":" + frameIndex` | `"abc-123:0"` |
| プレビュー用フォント | `"preview:" + sourceGuid + ":" + fontSize` | `"preview:abc-123:16"` |

`"preview:"` の接頭辞により、実際のフォントのバリアントの GUID と衝突しないようにしています。

### 決定的な GUID が生成される場所

- `deki-engine/src/Guid.cpp` - コアの実装
- `../../modules/deki-2d/editor/FontSyncHandler.cpp` - フォントのベイク
- `../../modules/deki-2d/TextComponent.cpp` - 編集モードでの font.guid の同期
- `src/services/AssetExporter.cpp` - エクスポート時のベイク
- `src/PlayMode.cpp` - プレイモードでのフォントの準備
- `deki-editor/src/AssetPipeline.cpp` - キャッシュの管理
- `deki-editor/src/TextureImporter.cpp` - スプライトのフレームの生成

## アセット検索テーブル (asset_table.bin)

実行時に O(log n) で検索するための**バイナリ形式**です。

```
[4 bytes]  entry count (uint32_t)
[N entries, 44 bytes each, sorted by hash]:
  [4 bytes]  FNV-1a hash of key (uint32_t)
  [37 bytes] GUID string (null-terminated)
  [3 bytes]  padding
```

### ハッシュ関数 (FNV-1a 32 ビット)

```
offset_basis = 2166136261
prime = 16777619
```

**これらの定数は絶対に変更しないでください。** 次の箇所の間で一致している必要があります。
- `src/services/AssetExporter.cpp` (エクスポート側)
- `deki-engine/include/deki/assets/AssetLookupTable.h` (ランタイム側)
- `deki-editor/src/AssetPipeline.cpp` (エディターのキャッシュ)

### テーブル内のキーの種類

| キーの形式 | 対応先 | 使用箇所 |
|------------|---------|---------|
| `"path:type"` (例: `"textures/player:Sprite"`) | アセットの GUID | `AssetManager::Load<T>("path")` |
| GUID 文字列 (例: `"abc-123-..."`) | 同じ GUID | ベイク済みフォントの GUID による直接検索 |

### テーブルの 2 つのバージョン

| ファイル | 範囲 | 書き込み元 |
|------|-------|------------|
| `cache/asset_table.bin` | プロジェクトのすべてのアセット | `AssetPipeline::UpdateAssetTable()` |
| `<build's assets>/asset_table.bin` | 使用されているアセットのみ | `AssetExporter::ExportAssetLookupTable()` |

ビルドのアセットは、ターゲットが内部ストレージに保存する場合はブートペイロードの隣 (`assets/`、`F:/assets/` として読み込み) に、SD カードの場合は `<project>/storage/` (`S:/`) にエクスポートされます。`AssetManager::LoadAssetRoot` はテーブル、パックのインデックス、すべてのアセットをそこから読み込みます。[platforms.md](platforms.md#where-the-assets-go) を参照してください。

## アセットの読み込みの流れ

```
AssetRef::Get()
  -> AssetManager::LoadByGuidAndType(guid, T::kAssetTypeName)
    -> LookupPath(guid)
      -> Check binary asset_table.bin (binary search by FNV-1a hash)
      -> Check guidToPath_ map (runtime registrations)
      -> Return: cacheDirectory + "/" + resolved path
    -> Load<T>(resolvedPath)  // Sprite::Load, BitmapFont::Load, etc.
```

## 主なファイル

| ファイル | 用途 |
|------|---------|
| `deki-engine/include/deki/assets/AssetRef.h` | AssetRef テンプレート |
| `deki-engine/src/assets/AssetManager.cpp` | GUID の検索、アセットの読み込み |
| `deki-engine/include/deki/assets/AssetLookupTable.h` | バイナリテーブルの形式と FNV-1a |
| `deki-engine/src/Guid.h` / `.cpp` | 決定的な GUID の生成 |
| `deki-editor/src/AssetPipeline.cpp` | GUID の割り当て、キャッシュの管理 |
| `src/services/AssetExporter.cpp` | エクスポートのパイプライン |
