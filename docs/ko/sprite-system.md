# 스프라이트 시스템

## 개요

스프라이트 시트는 임포트 파이프라인을 거칩니다. PNG 소스 파일 -> 프레임 메타데이터가 포함된 컴파일된 .tex 파일. 모든 프레임은 **부모 .tex 파일 안에** 영역으로 존재하며, 프레임마다 별도의 파일이 생성되지는 않습니다.

```
assets/sprites/player.png (source)
  -> cache/{parentGuid}  (compiled .tex with FrameList chunk)
```

폰트(크기마다 별도의 파일이 만들어짐)와 달리, 스프라이트 프레임은 하나의 텍스처의 하위 영역입니다. 부모 .tex 파일에는 픽셀 데이터와 모든 프레임 좌표를 나열한 FrameList 메타데이터 청크가 함께 들어 있습니다.

## GUID 생성

프레임 GUID는 부모 GUID + 프레임 인덱스로부터 **결정적으로** 만들어집니다.

```
frameGuid = GenerateDeterministicGuid(parentGuid + ":" + frameIndex)
```

따라서 다음이 성립합니다.
- 같은 부모 + 인덱스는 항상 같은 프레임 GUID를 만듭니다
- 참조를 깨뜨리지 않고 캐시를 처음부터 다시 만들 수 있습니다
- 프레임이 다르면 절대 충돌하지 않습니다

## .tex 파일 형식 (2DTX)

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

### Sprite 청크 (타입 1)

격자 기반 스프라이트 시트는 기본 프레임 크기를 저장합니다.

```
frameWidth:  int32_t
frameHeight: int32_t
```

### FrameList 청크 (타입 2)

모든 프레임과 그 GUID 및 좌표:

```
frameCount: uint16_t

For each frame (52 bytes):
  guid:   36 bytes (no null terminator)
  x:      int32_t
  y:      int32_t
  width:  int32_t
  height: int32_t
```

### SourceSize 청크 (타입 4)

텍스처가 이미지보다 작게 저장될 때(최대 크기)에만 존재합니다.

```
width:  uint32_t   the image's size
height: uint32_t
```

Sprite 청크와 FrameList 청크, 나인 슬라이스 경계, 타일셋은 이미지의 픽셀 기준으로 유지됩니다. `Sprite::ApplySourceSize`는 로드할 때 프레임, 경계, `pixelsPerMeter`를 저장된 픽셀 기준으로 변환하므로 스프라이트는 월드에서의 크기를 유지합니다. 타일셋은 `Sprite::SourceToStoredX/Y`로 타일 사각형을 매핑합니다. 둘 다 사각형의 변에 round(v x stored / image)를 사용하는데, 이는 인코더가 각 프레임을 리샘플링할 때 쓴 규칙이므로, 맞닿아 있던 프레임은 계속 맞닿아 있고 어느 프레임도 다른 프레임으로 번지지 않습니다.

## .data 사이드카 파일

각 스프라이트 시트에는 `.data` JSON 사이드카(예: `player.png.data`)가 있을 수 있습니다.

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

아틀라스 모드(사용자 지정 프레임 정의)의 경우:

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

## 임포트 파이프라인: TextureImporter

**파일:** `deki-editor/src/TextureImporter.cpp`

TextureImporter는 프레임 메타데이터와 함께 PNG -> .tex 변환을 처리합니다.

### 2단계 임포트

**1단계: 탐색**
- assets/ 안의 모든 이미지 파일을 스캔합니다
- 각 프레임에 대해 결정적 SubAsset GUID를 생성합니다
- 프레임 좌표를 `s_FrameDataRegistry`에 등록합니다

**2단계: 컴파일**
- stb_image로 소스 이미지를 로드합니다
- 타겟 포맷(RGB565 등)으로 변환합니다
- 헤더 + 픽셀 데이터 + 메타데이터 청크로 .tex 파일을 작성합니다
- FrameList 청크에는 모든 프레임 GUID + 좌표가 들어 있습니다

### SubAsset 생성

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

### 프레임 데이터 레지스트리

에디터에서 프레임 좌표를 빠르게 조회하기 위한 메모리 내 캐시:

```cpp
static std::unordered_map<std::string, SpriteFrameData> s_FrameDataRegistry;

// API:
TextureImporter::RegisterFrameData(frameGuid, data);
TextureImporter::GetFrameData(frameGuid);
TextureImporter::ClearFrameData();
```

## 스프라이트의 AssetRef 필드

SpriteComponent에서는 다음과 같습니다.

```
sprite.guid   = parent sprite GUID (the .tex file to load)
sprite.source = frame GUID (which sub-region to render)
```

### guid = 부모, source = 프레임인 이유

스프라이트 프레임은 별도의 파일이 아니라 **하나의 파일 안의 영역**입니다. 부모 .tex에는 FrameList 청크에 모든 프레임이 포함되어 있습니다. 따라서:

- `sprite.guid`는 부모 .tex(디스크에서 로드할 실제 파일)를 가리켜야 합니다
- `sprite.source`는 그 파일 안의 어느 프레임 영역인지 식별합니다

이는 각 베리언트가 별도의 파일인 폰트(`font.guid` = 베리언트 파일, `font.source` = TTF 소스)와 다릅니다.

### 일반 스프라이트(프레임 없음)의 경우

```
sprite.guid   = sprite GUID (the .tex file)
sprite.source = (empty)
```

## 프레임 결정 흐름

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

### 생성된 ResolveObjectRefs

코드 생성기는 다음 패턴을 생성합니다.

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

이렇게 하면 `OnAssetRefResolved`는 **의미 있는 식별자**(스프라이트 시트는 프레임 GUID, 일반 스프라이트는 스프라이트 GUID)를 받습니다.

### FindFrame

```cpp
const SpriteFrame* Sprite::FindFrame(const std::string& guid) const {
    for (size_t i = 0; i < frames.size(); i++) {
        if (guid == frames[i].guid) return &frames[i];
    }
    return nullptr;
}
```

선형 탐색입니다. 프레임 수는 적습니다(일반적으로 < 100).

## 에디터에서의 지정

**파일:** `src/panels/inspector/AssetPropertyEditor.cpp`

사용자가 인스펙터에서 SpriteComponent에 프레임을 드롭하면:

```cpp
// Frame dropped (SubAsset)
assetRefBase.guid = subAsset->parentGuid;   // Parent for loading
assetRefBase.source = droppedGuid;          // Frame for coordinate lookup

// Plain sprite dropped
assetRefBase.guid = droppedGuid;            // Sprite for loading
assetRefBase.source = "";                   // No frame
```

## GUID 등록 (에디터)

`LookupPath`가 동작하도록 SubAsset GUID를 AssetManager에 등록합니다.

```cpp
// EditorApp.cpp
AssetManager::Get()->RegisterGuid(subAsset.guid, info.guid);
// Maps: frameGuid -> parentGuid
// LookupPath(frameGuid) -> cacheDir/parentGuid -> loads parent .tex
```

## 익스포트 파이프라인

`AssetExporter::ExportToStorage()` 동안:

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

**폰트와의 주요 차이점:** 폰트 베리언트는 별도의 파일이므로 조회 테이블에 별도의 항목이 필요합니다. 스프라이트 프레임은 그렇지 않으며, 부모를 로드한 뒤 `FindFrame()`으로 찾습니다.

## 비교: 폰트와 스프라이트

| 항목 | 폰트 | 스프라이트 |
|--------|-------|---------|
| 가상 에셋 | 별도의 파일(베리언트 + 아틀라스) | 하나의 파일 안의 영역 |
| `guid` 필드 | 베리언트 GUID(베이크된 파일) | 부모 GUID(.tex 파일) |
| `source` 필드 | TTF 소스 GUID | 프레임 GUID |
| 결정적 시드 | `source + ":" + size` | `parent + ":" + index` |
| 익스포트되는 파일 | 크기마다 베리언트 하나 + 아틀라스 하나 | 스프라이트 시트마다 .tex 하나 |
| 조회 테이블 항목 | 베리언트 + 아틀라스 GUID | 부모 GUID만 |
| 런타임 결정 | `LookupPath(variantGuid)` -> 파일 | `LookupPath(parentGuid)` -> 파일, 그다음 `FindFrame(frameGuid)` |

## 주요 파일

| 파일 | 용도 |
|------|---------|
| `deki-editor/src/TextureImporter.cpp` | PNG -> .tex 변환, SubAsset 생성 |
| `deki-editor/include/deki-editor/TextureImporter.h` | TextureImporter 인터페이스, SpriteFrameData |
| `../../modules/deki-2d/Sprite.h` | Sprite 클래스, SpriteFrame 구조체, FindFrame |
| `../../modules/deki-2d/Sprite.cpp` | 스프라이트 로딩, FrameList 파싱 |
| `../../modules/deki-2d/SpriteComponent.h` | 프레임 사각형 필드가 있는 SpriteComponent |
| `../../modules/deki-2d/generated/Deki2DReflection0.gen.cpp` | source 패턴을 사용하는 생성된 ResolveObjectRefs(컴포넌트마다가 아니라 패키지마다 파일 하나) |
| `src/panels/inspector/AssetPropertyEditor.cpp` | 프레임 지정 UI |
| `src/services/AssetExporter.cpp` | 익스포트 파이프라인 |
