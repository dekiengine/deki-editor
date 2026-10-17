# 폰트 시스템

## 개요

폰트는 컴파일 파이프라인을 거칩니다. TTF 소스 파일 -> 베이크된 BitmapFont(.dfont) + 텍스처 아틀라스. 폰트 크기마다 결정적 GUID를 가진 **별도의 파일 쌍**이 만들어집니다.

```
assets/fonts/MyFont.ttf (source)
  -> cache/{variantGuid}  (compiled .dfont for 16px)
  -> cache/{atlasGuid}    (texture atlas for 16px)
  -> cache/{variantGuid2} (compiled .dfont for 24px)
  -> cache/{atlasGuid2}   (texture atlas for 24px)
```

## GUID 생성

폰트 베리언트 GUID는 소스 GUID + 폰트 크기로부터 **결정적으로** 만들어집니다.

```
variantGuid = GenerateDeterministicGuid(sourceGuid + ":" + fontSize)
atlasGuid   = GenerateDeterministicGuid(sourceGuid + ":" + fontSize + ":atlas")
```

따라서 다음이 성립합니다.
- 같은 소스 + 크기는 항상 같은 GUID를 만듭니다
- 참조를 깨뜨리지 않고 캐시를 처음부터 다시 만들 수 있습니다
- 크기가 다르면 절대 충돌하지 않습니다

## .data 사이드카 파일

각 폰트에는 `.data` JSON 사이드카(예: `MyFont.ttf.data`)가 있습니다.

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

- `fontSettings.sizes`: 이 폰트에 설정된 크기
- `variants`: 각 크기를 해당 베리언트 + 아틀라스 GUID에 매핑합니다(항상 결정적으로 다시 생성됨)

## 베이크 권한: FontSyncHandler

**파일:** `../../modules/deki-2d/editor/FontSyncHandler.cpp`

FontSyncHandler는 폰트 베이크의 **유일한 권한자**입니다. 모든 베이크 요청은 이를 거칩니다.

```
EnsureFontSizeBaked(sourceGuid, fontSize)
  -> Updates .data file with the requested size
  -> Calls pipeline->RefreshAsset() to trigger HandleFontSync
  -> HandleFontSync reads .data, compiles TTF for each size
  -> Writes cache/{variantGuid} (dfont) and cache/{atlasGuid} (atlas)
  -> Registers variant GUID with AssetManager
```

### EnsureFontSizeBaked를 호출하는 곳:
- `TextComponentEditor.cpp` - 폰트 미리보기의 "저장 후 사용" 버튼
- `AssetPropertyEditor.cpp` - 에셋 프로퍼티 에디터의 "저장 후 사용" 버튼

### 등록
베이크가 끝나면 FontSyncHandler는 베리언트를 AssetManager에 등록합니다.
```cpp
AssetManager::Get()->RegisterGuid(variantGuid, variantGuid);
```
이를 통해 에디터에서 `LookupPath(variantGuid)` -> `cacheDir/variantGuid`가 가능해집니다.

## 폰트의 AssetRef 필드

TextComponent에서는 다음과 같습니다.
```
font.guid   = variant GUID (baked .dfont file for a specific size)
font.source = TTF source GUID (the original font file)
```

`font.Get()`은 `font.guid` -> `AssetManager::LoadByGuidAndType` -> `BitmapFont::Load` 순서로 로드합니다.

### font.guid가 동기화되는 방식

`TextComponent::RenderContent`(에디터 모드)에서:
```cpp
std::string expectedGuid = GenerateDeterministicGuid(font.source + ":" + std::to_string(fontSize));
if (font.guid != expectedGuid) {
    font.guid = expectedGuid;
    font.ptr = nullptr;
    font.loadAttempted = false;
}
```

이렇게 하면 사용자가 인스펙터에서 `fontSize`를 바꿨을 때 다음 렌더 프레임에서 올바른 베리언트를 로드합니다.

## 미리보기 모드 (에디터 전용)

에디터에서는 디스크에 베이크하지 않고 임의의 크기로 폰트를 미리 볼 수 있습니다.

1. 사용자가 TextComponentEditor에서 미리보기를 켭니다
2. `GetEditorFontVariant()`가 필요할 때 TTF를 메모리 내 BitmapFont로 컴파일합니다
3. 미리보기 슬롯은 하나뿐이며, 한 번에 하나의 미리보기만 존재합니다
4. 캐시에 저장되지 않으며, 순전히 테스트용입니다
5. "저장 후 사용" 버튼은 미리 본 크기를 `EnsureFontSizeBaked`로 베이크합니다

## 익스포트 파이프라인

`AssetExporter::ExportToStorage()` 동안:

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

## BitmapFont 파일 형식 (.dfont)

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

dfont 안에 저장된 아틀라스 경로는 dfont 파일이 있는 디렉터리를 기준으로 한 상대 경로입니다. 두 파일은 같은 cache/ 또는 storage/ 디렉터리에 있으므로 atlasGuid 문자열 그대로입니다.

## 주요 파일

| 파일 | 용도 |
|------|---------|
| `../../modules/deki-2d/TextComponent.h` | fontSize, previewEnabled, previewSize 필드 |
| `../../modules/deki-2d/TextComponent.cpp` | RenderContent의 폰트 로딩, 미리보기 컴파일 |
| `../../modules/deki-2d/BitmapFont.cpp` | 폰트 로딩, 글리프 렌더링 |
| `../../modules/deki-2d/editor/FontSyncHandler.cpp` | 베이크 권한자 |
| `../../modules/deki-2d/editor/FontCompiler.cpp` | TTF -> BitmapFont 컴파일 |
| `../../modules/deki-2d/editor/TextComponentEditor.cpp` | 인스펙터 UI, 폰트 크기 드롭다운 |
| `src/services/AssetExporter.cpp` | 익스포트 시 베이크 및 복사 |
