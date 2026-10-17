# 에셋 시스템

## AssetRef\<T\> 템플릿

**파일:** `deki-engine/include/deki/assets/AssetRef.h`

에셋을 참조하는 모든 컴포넌트는 `AssetRef<T>`를 사용합니다. 필드는 네 개입니다.

| 필드 | 타입 | 용도 |
|-------|------|---------|
| `guid` | `std::string` | **디스크에서 에셋을 로드하는 데** 사용하는 GUID입니다. 항상 실제 파일을 가리킵니다. |
| `source` | `std::string` | 상황에 따라 의미가 달라지는 메타데이터입니다. 아래의 "이중 필드 패턴"을 참조하십시오. |
| `ptr` | `void*` | 로드된 에셋을 캐시한 포인터입니다. 처음 `Get()`을 호출할 때 설정됩니다. |
| `loadAttempted` | `bool` | 재시도가 반복되는 것을 막습니다. 첫 로드 시도 후 `true`로 설정됩니다. |

### 지연 로딩

`Get()`은 한 번만 로드를 수행합니다.

```
if (!ptr && !guid.empty() && !loadAttempted) {
    loadAttempted = true;
    ptr = static_cast<T*>(AssetManager::Get()->LoadByGuidAndType(guid, T::kAssetTypeName));
}
return ptr;
```

`guid`를 바꾼 뒤 강제로 다시 로드하려면 `ptr`과 `loadAttempted`를 모두 초기화해야 합니다.
```cpp
ref.guid = newGuid;
ref.ptr = nullptr;
ref.loadAttempted = false;
```

## 이중 필드 패턴 (guid와 source)

`guid` 필드와 `source` 필드는 에셋 타입에 따라 **의미가 다릅니다**. 이는 의도된 것이며, 실제 아키텍처상의 차이를 반영합니다.

### 규칙: `guid`는 항상 "디스크에서 로드할 파일"

| 에셋 타입 | `guid` | `source` | 이유 |
|------------|--------|----------|-----|
| **스프라이트** (일반) | 스프라이트 GUID | (비어 있음) | 파일 하나이며 간접 참조가 없습니다 |
| **스프라이트** (프레임) | **부모** 스프라이트 GUID | **프레임** GUID | 부모 .tex 파일에 모든 프레임이 들어 있습니다. `guid`로 파일을 로드하고, `source`로 프레임 영역을 식별합니다. |
| **폰트** | **베리언트** GUID (베이크됨) | **TTF** 소스 GUID | 폰트 크기마다 별도의 파일입니다. `guid`로 베이크된 베리언트를 직접 로드합니다. |
| **애니메이션** | 애니메이션 GUID | (비어 있음) | 파일 하나 |

### 스프라이트와 폰트가 다른 이유

- **폰트 베리언트는 별도의 파일입니다.** 16px 폰트와 24px 폰트는 서로 다른 .dfont + 아틀라스 파일을 만듭니다. 베리언트 GUID는 해당 파일을 직접 가리킵니다.
- **스프라이트 프레임은 한 파일의 영역입니다.** 프레임 0과 프레임 5는 같은 .tex 파일의 하위 영역입니다. 부모 GUID는 텍스처 전체를 가리킵니다. 프레임 GUID는 `FindFrame()`으로 좌표를 조회하는 데에만 사용합니다.

### 생성 코드 패턴

코드 생성기는 다음과 같은 `ResolveObjectRefs`를 생성합니다.
```cpp
const char* _frameGuid = ref.source.empty() ? ref.guid.c_str() : ref.source.c_str();
OnAssetRefResolved(propertyName, loadedAsset, _frameGuid);
```

이렇게 하면 값이 어느 필드에 저장되어 있든 `OnAssetRefResolved`는 **의미 있는 식별자**(스프라이트는 프레임 GUID, 폰트는 TTF 소스)를 받습니다.

## 결정적 GUID 생성

가상 에셋과 파생 에셋은 시드 문자열로부터 `GenerateDeterministicGuid()`로 생성한 결정적 GUID를 사용합니다(64비트 FNV-1a 해시 두 개를 UUID 형식으로 만든 것).

### 시드 형식 (절대 변경하지 마십시오)

| 가상 에셋 | 시드 형식 | 예 |
|---------------|-------------|---------|
| 폰트 베리언트 | `sourceGuid + ":" + fontSize` | `"abc-123:16"` |
| 폰트 아틀라스 | `sourceGuid + ":" + fontSize + ":atlas"` | `"abc-123:16:atlas"` |
| 스프라이트 프레임 | `parentGuid + ":" + frameIndex` | `"abc-123:0"` |
| 미리보기 폰트 | `"preview:" + sourceGuid + ":" + fontSize` | `"preview:abc-123:16"` |

`"preview:"` 접두사는 실제 폰트 베리언트 GUID와 충돌하는 것을 막습니다.

### 결정적 GUID를 생성하는 곳

- `deki-engine/src/Guid.cpp` - 핵심 구현
- `../../modules/deki-2d/editor/FontSyncHandler.cpp` - 폰트 베이크
- `../../modules/deki-2d/TextComponent.cpp` - 편집 모드의 font.guid 동기화
- `src/services/AssetExporter.cpp` - 익스포트 시 베이크
- `src/PlayMode.cpp` - 플레이 모드의 폰트 설정
- `deki-editor/src/AssetPipeline.cpp` - 캐시 관리
- `deki-editor/src/TextureImporter.cpp` - 스프라이트 프레임 생성

## 에셋 조회 테이블 (asset_table.bin)

런타임에 O(log n)으로 조회하기 위한 **바이너리 형식**입니다.

```
[4 bytes]  entry count (uint32_t)
[N entries, 44 bytes each, sorted by hash]:
  [4 bytes]  FNV-1a hash of key (uint32_t)
  [37 bytes] GUID string (null-terminated)
  [3 bytes]  padding
```

### 해시 함수 (FNV-1a 32비트)

```
offset_basis = 2166136261
prime = 16777619
```

**이 상수는 절대 변경하지 마십시오.** 다음 위치에서 값이 일치해야 합니다.
- `src/services/AssetExporter.cpp` (익스포트 측)
- `deki-engine/include/deki/assets/AssetLookupTable.h` (런타임 측)
- `deki-editor/src/AssetPipeline.cpp` (에디터 캐시)

### 테이블의 키 종류

| 키 형식 | 매핑 대상 | 사용처 |
|------------|---------|---------|
| `"path:type"` (예: `"textures/player:Sprite"`) | 에셋 GUID | `AssetManager::Load<T>("path")` |
| GUID 문자열 (예: `"abc-123-..."`) | 같은 GUID | 베이크된 폰트의 직접 GUID 조회 |

### 테이블의 두 가지 버전

| 파일 | 범위 | 작성하는 곳 |
|------|-------|------------|
| `cache/asset_table.bin` | 프로젝트의 모든 에셋 | `AssetPipeline::UpdateAssetTable()` |
| `<build's assets>/asset_table.bin` | 사용하는 에셋만 | `AssetExporter::ExportAssetLookupTable()` |

빌드의 에셋은 타겟이 에셋을 내부 스토리지에 두는 경우 부트 페이로드 옆(`assets/`, `F:/assets/`로 읽음)에, SD 카드에 두는 경우 `<project>/storage/`(`S:/`)에 익스포트됩니다. `AssetManager::LoadAssetRoot`는 그곳에서 테이블, 팩 인덱스, 모든 에셋을 로드합니다. [platforms.md](platforms.md#where-the-assets-go)를 참조하십시오.

## 에셋 로딩 흐름

```
AssetRef::Get()
  -> AssetManager::LoadByGuidAndType(guid, T::kAssetTypeName)
    -> LookupPath(guid)
      -> Check binary asset_table.bin (binary search by FNV-1a hash)
      -> Check guidToPath_ map (runtime registrations)
      -> Return: cacheDirectory + "/" + resolved path
    -> Load<T>(resolvedPath)  // Sprite::Load, BitmapFont::Load, etc.
```

## 주요 파일

| 파일 | 용도 |
|------|---------|
| `deki-engine/include/deki/assets/AssetRef.h` | AssetRef 템플릿 |
| `deki-engine/src/assets/AssetManager.cpp` | GUID 조회, 에셋 로딩 |
| `deki-engine/include/deki/assets/AssetLookupTable.h` | 바이너리 테이블 형식 + FNV-1a |
| `deki-engine/src/Guid.h` / `.cpp` | 결정적 GUID 생성 |
| `deki-editor/src/AssetPipeline.cpp` | GUID 할당, 캐시 관리 |
| `src/services/AssetExporter.cpp` | 익스포트 파이프라인 |
