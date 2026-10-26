# 메모리

## 규칙

**장치에서 실행되는 코드의 모든 할당은 엔진을 거칩니다.** 엔진의 런타임 코드에서도, 패키지의 런타임 코드에서도 `new[]`, `malloc`, `heap_caps_*`를 쓰지 않습니다. `Deki::Memory`가 전체를 조율하며, 거의 언제나 필요한 것은 `Deki::Buffer<T>`입니다.

이것은 스타일 취향이 아니며, [`tools/ci/check_allocations.py`](../../tools/ci/check_allocations.py)가 강제합니다. 이 규칙이 지켜져야 하는 이유는 세 가지입니다.

1. **`new[]`는 실패를 우아하게 처리할 수 없습니다.** ESP-IDF는 예외를 끈 상태로 빌드하므로(`CONFIG_COMPILER_CXX_EXCEPTIONS`가 설정되지 않음), 할당이 실패하면 `__cxa_allocate_exception`에 도달하여 `abort()`를 호출하고 보드가 재부팅됩니다. 추가할 catch 지점이 없습니다. `std::nothrow`도 도움이 *되지 않습니다*. libstdc++는 nothrow 형식을 `-fno-exceptions`가 제거해 버린 `try`/`catch` 안에서 throw 형식을 호출하는 방식으로 구현하므로, 똑같이 중단됩니다. 이는 `operator new[](unsigned int, std::nothrow_t const&)`가 곧바로 throw 형식을 호출하는 백트레이스로 진단되었습니다.
2. **메모리가 어디에 있어야 하는지는 엔진만 압니다.** 이 보드에 외부 RAM이 있는지, 얼마나 느린지, 주변 장치가 특정 영역에 접근할 수 있는지는 호출하는 쪽이 아니라 플랫폼의 속성입니다. 영역을 직접 고르는 호출자는 다음 보드에서 틀리게 됩니다.
3. **`Deki::Memory`는 사용량 집계이자 누수 검사이기도 합니다.** 통계, 이중 해제 방지, (디버그 빌드에서의) 누수 테이블이 모두 그 뒤에 있습니다. 이를 건너뛴 할당은 보이지 않습니다.

## `Deki::Buffer<T>` - 이것을 사용하십시오

`<deki/providers/Buffer.h>`. `Deki::Memory`를 통해 할당하고 스스로 해제하는 소유 배열입니다.

```cpp
#include <deki/providers/Buffer.h>

Deki::Buffer<uint8_t> baked;

if (!baked.Allocate(width * height * 2, Deki::Memory::External))
{
    DEKI_LOG_WARNING("no room for a %dx%d bake; not drawing it", width, height);
    return false;          // carry on without the thing, do not abort
}

RenderInto(baked.Data());  // Data(), Count(), Bytes(), operator[], begin/end
```

- **이동만 가능합니다.** 복사하려면 실패할 수 있는 두 번째 할당이 필요한데, 복사 생성자는 그 실패를 알릴 수 없습니다.
- **`T`는 트리비얼하게 복사 가능하고 트리비얼하게 소멸 가능해야 하며**, 둘 다 `static_assert`로 강제됩니다. 여기서는 어떤 요소도 생성하거나 소멸시키지 않습니다. 이것은 엔진이 블리터와 드라이버에 넘기는 원시 픽셀, 글리프, 스팬, 샘플 버퍼입니다. 소멸자가 있는 `T`라면 그 소멸자는 조용히 한 번도 실행되지 않을 것입니다.
- 다른 모든 곳과 마찬가지로 여기서도 **영역은 필수입니다**.
- **개수가 바뀌지 않은 `Allocate`는 아무 일도 하지 않으므로**, 렌더 경로에서 매 프레임 호출해도 안전합니다. 개수가 다르면 복사하지 않고 먼저 이전 버퍼를 해제합니다. 모든 호출자가 직후에 버퍼를 덮어쓰며, 두 할당을 동시에 들고 있는 것이야말로 제약이 많은 칩이 감당할 수 없는 일입니다.
- **`Allocate(0)`은 실패가 아니라 성공입니다.** 코드 포인트가 없는 희소 폰트와 스팬이 없는 스프라이트는 둘 다 정상입니다.
- **요소 개수는 오버플로를 검사합니다.** 장치에서 `count * sizeof(T)`는 32비트 `size_t`로 계산되므로, 파일이나 씬에서 읽은 개수가 랩어라운드하여 작은 할당이 되고 나중에 범위를 넘어설 수 있습니다.
- `Reset()`은 일찍 해제합니다. `Release()`는 원시 포인터를 넘겨주며, 이후 호출자가 `Deki::Memory::Free`를 책임집니다. `Release`는 의도적으로 드물게 쓰이도록 설계되었습니다.

자동화되지 않는 단 한 가지는 *메모리가 실제로 있었는지*입니다. 이후의 모든 분기에서 되감는 대신, 버퍼를 만드는 곳에서 `if (!buffer)`를 한 번 검사하십시오.

### 아레나를 쓰지 않는 이유

프레임 아레나도 실패 처리를 없애 주지만, 크기를 미리 정해야 합니다. 전체 화면 그라디언트 베이크 하나를 담을 만큼 큰 아레나는 프로젝트에 그라디언트가 있든 없든 150 KB를 예약하는데, 힙이 수백 KB인 칩에서는 존재하지 않을 수도 있는 기능을 위해 메모리의 절반을 잡아 두는 셈입니다. 아레나는 여유가 있는 곳에서 효과가 있습니다. 여기서 대부분의 할당은 수명이 길고 어차피 무언가가 소유합니다. 글리프 테이블, 픽셀 데이터, 파티클 열, 프레임버퍼 등이며, 이것이 바로 RAII가 다루는 경우이고 아레나는 다루지 않는 경우입니다.

## 영역 - 매번 직접 고릅니다

`<deki/providers/Memory.h>`. **기본값은 없고 아무것도 추론하지 않습니다.** 할당하는 모든 호출은 영역을 지정하며, 생략하면 컴파일되지 않습니다.

| 영역 | 할당되는 곳 |
| --- | --- |
| `Deki::Memory::Internal` | 내부 RAM. 빠르고, 부족한 쪽입니다 |
| `Deki::Memory::External` | 보드에 PSRAM이 있으면 PSRAM. 메가바이트 단위이며 SPI를 통해 접근하므로 눈에 띄게 느립니다 |

`Deki::Memory::AllocateDma(bytes, region)`는 DMA 엔진이 그 블록을 읽을 수 있어야 한다는 제약을 추가합니다. 이것은 배치와는 별개입니다. 프레임버퍼는 보드가 내부에 두든 PSRAM에 두든 접근 가능해야 하므로, 세 번째 영역이 아니라 별도의 진입점으로 되어 있습니다.

### 영역은 이름이므로 패키지가 직접 추가할 수 있습니다

엔진은 두 가지를 제공합니다. 그 집합은 열려 있습니다.

```cpp
// in a board-support package - no engine change, no registration call
namespace DekiEsp32
{
inline constexpr Deki::Memory::Region Rtc{"esp32.rtc"};
}

battery.Allocate(count, DekiEsp32::Rtc);
```

패키지는 자신의 영역을 `Deki::Memory`가 아니라 **자신의 네임스페이스**에 둡니다. 클래스는 닫혀 있으므로 어차피 그렇게 할 수도 없으며, 바로 그것이 핵심입니다. 둘 다 `Rtc` 상수를 원하는 두 보드 패키지는 언어가 둘을 분리해 주므로 충돌할 수 없습니다. 상수가 어디에 있는지는 결코 중요하지 않습니다. 영역의 정체성은 해시된 이름이므로, 네임스페이스 사이에서 옮겨도 아무것도 바뀌지 않습니다.

`Region()`은 엔진의 다른 곳에서 쓰는 것과 같은 FNV-1a로 컴파일 시점에 이름을 해시하므로, 영역은 프로바이더가 switch로 분기할 수 있는 상수 표현식입니다.

**두 패키지가 같은 영역 이름을 쓰면 같은 영역을 뜻합니다.** 두 보드 패키지가 서로 조율하지 않고도 `"rtc"`에 합의하는 방식이 이것이며, 두 패키지가 모두 `spi` 케이퍼빌리티를 요구하는 것과 똑같습니다. 이 방식이 감당하지 못하는 것은 두 패키지가 한 단어로 *서로 다른* 것을 뜻하는 경우이므로, 직접 만든 이름이 아니라면 한정하십시오: `particles.scratch`, `esp32.rtc`. 할당 헤더는 ID만 저장하므로, 디버그 빌드는 해시가 같은 서로 다른 두 이름을 보고합니다.

### 영역을 제공하는 것은 보드의 일입니다

패키지는 영역에 *이름을 붙일* 수 있지만, 영역을 제공할 수 있는 것은 보드의 `IMemoryProvider`뿐입니다. `esp32.rtc`를 모르는 프로바이더는 nullptr를 반환하며, 실패 메시지에 영역 이름이 나옵니다.

```
Failed to allocate 64 bytes in 'esp32.rtc' for 'void Battery::Save()'
at Battery.cpp:88 (0 bytes free there)
```

다른 영역으로 대체되지 않습니다. 특히 `Rtc`의 경우에는 이것이 더욱 중요합니다. 이 영역의 목적은 딥 슬립을 넘어 살아남는 것이므로, 조용히 일반 RAM을 돌려주면 데이터가 유지되어야 할 바로 그 순간에 데이터를 잃게 됩니다.

### 영역 이름은 케이퍼빌리티 이름입니다

타겟은 자신이 가진 영역을 `memory:` 접두사를 붙인 케이퍼빌리티로 알립니다.

| 케이퍼빌리티 | 출처 |
| --- | --- |
| `memory:internal` | 모든 플랫폼 - 자동으로 도출됨 |
| `memory:external` | 플랫폼의 `psramSize` - 자동으로 도출됨 |
| `memory:esp32.rtc` | 보드의 `provides`. 보드만 알기 때문입니다 |

따라서 영역이 필요한 패키지는 이를 선언하고, 빌드는 그것이 없는 타겟을 아무것도 실행하기 전에 이름을 들어 거부합니다.

```json
"platformRequires": ["memory:esp32.rtc"]
```

`Region("esp32.rtc")`와 `"memory:esp32.rtc"`는 누군가 맞춰 관리해야 하는 두 목록이 아니라 접두사만 붙은 같은 토큰이므로, 양쪽이 어긋날 수 없습니다. 케이퍼빌리티 이름은 소문자와 `.`, `-`, `:`, `_`로 이루어집니다. `"WiFi"`를 선언한 플랫폼은 로드할 때 경고를 받습니다. 정확한 문자열 일치로는 조용히 영원히 일치하지 않기 때문입니다.

### 배치는 한 번에 결정됩니다. 대체는 없습니다.

선택한 영역이 가득 차 있으면 할당은 **실패하고 null을 반환합니다**. 다른 영역에서 재시도하지 않습니다. 이는 의도된 것입니다.

- `Hot` 버퍼를 조용히 외부 RAM으로 옮기면, 배치 실수가 아무도 원인을 설명할 수 없는 프레임 드롭이 됩니다.
- `Buffer`를 조용히 내부 RAM으로 옮기면, 다음 `Hot` 할당에 필요했던 힙이 고갈되어 실패가 엉뚱한 곳에서 드러납니다.

실패 시에는 무엇이었는지, 크기, 영역, 그리고 대체를 시도하지 않았다는 사실을 로그로 남깁니다. 모든 호출자는 이미 null을 처리해야 합니다.

외부 RAM이 없는 보드는 모든 것을 내부로 결정합니다. 이것은 대체가 아니라, 존재하는 영역이 그것뿐인 것입니다.

## 나머지 API

`Buffer<T>`가 다루지 않는 경우를 위한 `Deki::Memory`(`<deki/providers/Memory.h>`):

```cpp
void* Allocate(size_t size, Memory::Region region, AllocContext ctx = {});
void* AllocateDma(size_t size, Memory::Region region, AllocContext ctx = {});
T*    AllocateArray<T>(size_t count, Memory::Region region, AllocContext ctx = {});
void  Free(void* ptr, const char* context = nullptr);
```

- 메모리는 반환되기 전에 **0으로 채워지므로**, 버퍼가 쓰레기 값으로 시작하는 일이 없고 호출자가 직접 지우는 루프를 작성할 필요도 없습니다.
- `Free`는 사용자 포인터 바로 앞에 놓인 헤더를 읽어 블록이 어느 영역에서 왔는지 알아내므로, 호출자가 고를 일이 없습니다. 이 헤더에는 매직 값도 들어 있어서, 이중 해제나 외부 포인터를 힙 손상 대신 로그에 남는 오류로 바꿉니다.
- **할당에 이름을 붙이는 것은 선택 사항입니다.** 아래의 `AllocContext`를 참조하십시오.
- 실패 시에는 시도한 영역에 남은 여유 공간을 보고하므로, "이 에셋이 이 보드에 너무 크다"와 "무언가가 해제했어야 할 메모리를 붙잡고 있다"를 구별할 수 있습니다.
- `Memory::Initialize`는 시작할 때 내부 및 외부 RAM의 여유 공간을 한 번 로그로 남기므로, 첫 메모리 부족 메시지가 아니라 실제 숫자를 기준으로 프로젝트 크기를 정할 수 있습니다.
- `AllocateInternal` / `FreeInternal`은 배치 결정을 우회하며 플랫폼 계층을 위해 존재합니다. 컴포넌트에서 사용하지 마십시오.

### 메모리가 어디에 쓰였는지 알아내기

```cpp
Deki::Memory::LogStats("after level load");
// Memory at after level load: 48216 bytes allocated (48216 internal, 0 external);
//                             237096 free internal
```

- `LogStats(label)`는 "메모리를 얼마나 쓰고 있는가?"에 한 번의 호출로 답합니다. 디버그 빌드에서는 현재 할당 수와 최대 할당 수도 보고합니다. 호출하지 않으면 비용이 들지 않습니다.
- `GetStats()`는 같은 숫자를 구조체로 반환합니다. `GetAvailableInternal()`과 `GetAvailablePSRAM()`은 여유 공간을 알려 줍니다. `IsPSRAMAvailable()`은 외부 영역이 있는지 여부를 알려 줍니다. 이들 중 어느 것도 메모리를 직접 배치해도 된다는 허가가 아닙니다. 프로젝트 크기를 정하고, 해제했어야 할 메모리를 붙잡고 있는 것을 찾기 위한 것입니다.

### 씬 바꾸기

```cpp
Deki::Engine::GetInstance().OpenScene(nextScene);  // AssetRef<Scene>
```

`OpenScene`은 프레임이 끝날 때 떠나는 씬을 먼저 해제한 뒤 다음 씬을 로드하므로, 두 씬이 동시에 메모리에 있는 일이 없습니다. 다음 씬을 직접 로드하고 `SetRootScene`을 호출하면 둘 다 들고 있게 되며, T-Deck Plus에서 데모의 메뉴에서 초원으로 그렇게 넘어가면 내부 RAM이 16바이트만 남았습니다. 존재하지 않는 씬은 열리지 않고, 게임은 지금 씬에 머뭅니다.

장치에서는 그다음 떠난 씬만 쓰던 에셋(스프라이트, 폰트, 애니메이션)을 해제하므로, 게임의 메모리는 방문한 모든 씬이 아니라 지금 씬에 필요한 만큼이 됩니다. 많은 오브젝트를 제거한 뒤 같은 일을 하려면 `Engine::ReleaseUnusedAssets()`를 호출합니다. 살아 있는 오브젝트에 있는 컴포넌트의 `AssetRef` 필드가 가리키는 동안 에셋은 남아 있습니다. 에셋은 `AssetRef`로 들고 있으세요. 다른 곳에서 들고 있는 `AssetRef`는 다음에 요청될 때 에셋을 다시 로드하지만, `Get()`으로 얻은 원시 포인터를 프레임을 넘겨 들고 있으면 그 에셋을 쓰는 것이 있는 동안만 유효합니다.

## `AllocContext` - 레이블은 저절로 작성됩니다

할당하는 모든 호출이 이를 받지만, 직접 작성하는 사람은 거의 없습니다.

```cpp
posX.Allocate(capacity, Deki::Memory::Internal);
```

디버그 빌드에서는 기본 인수가 호출 위치를 캡처하므로, 실패 메시지는 다음과 같습니다.

```
Failed to allocate 153600 bytes in internal RAM for
'void ParticlePool::SetCapacity(size_t)' at ParticlePool.h:47
(126556 bytes free there)
```

전달할 이름도, 전달할 방법도 없습니다. 손으로 쓴 태그는 같은 할당에 대한 두 번째 정체성이 되어 누군가 첫 번째와 맞춰 관리해야 하며, 컴파일러는 이미 답을 알고 있습니다. 한 함수 안의 두 할당은 줄 번호로 구별됩니다.

```
'void ParticlePool::SetCapacity(size_t)' at ParticlePool.h:47
'void ParticlePool::SetCapacity(size_t)' at ParticlePool.h:48
```

**엔진이 아니라 호출자를 가리킵니다.** `Buffer<T>::Allocate`와 `Memory::Allocate`는 모두 컨텍스트를 받아 기본값을 다시 적용하지 않고 그대로 전달하므로, 위치는 `Buffer.h`가 아니라 *여러분이* 호출을 작성한 곳이 됩니다. 테스트가 바로 이것을 검사합니다. 이를 잘못 구현한 버전은 엔진의 모든 할당에서 그럴듯해 보이는 파일을 보고하면서 자신만만하게 쓸모없을 것이기 때문입니다.

**아무것도 복사하지 않습니다.** 모든 멤버는 정적 저장 기간을 가진 문자열 리터럴을 가리키며, 그 덕분에 디버그 누수 테이블이 포인터를 보관할 수 있습니다.

### 제거하기

캡처는 모든 빌드에서 켜져 있습니다. 비용은 **ESP32-S3 펌웨어에서 4,272 바이트, 즉 1.5 MB 이미지의 0.28%**이며 호출 위치당 약 40 바이트로, 신경 쓰지 않아도 될 만큼 저렴합니다.

플래시 용량을 되찾아야 하는 빌드에서는 `DEKI_STRIP_ALLOC_CONTEXT`를 정의하십시오. `AllocContext`는 빈 구조체가 되고, 경로나 함수 이름이 바이너리에 들어가지 않으며, 실패 시에는 출처를 밝히지 않고 무엇을 하려 했는지만 보고합니다.

```
Failed to allocate 153600 bytes in internal RAM (126556 bytes free there)
```

제거한 빌드와 제거하지 않은 빌드는 레이아웃이 다르므로, 엔진과 엔진을 호출하는 패키지는 일치해야 합니다. 이는 `MemoryStats`가 이미 부과하는 것과 같은 제약입니다.

## `Deki::Vector<T>` - 런타임에 개수가 바뀔 때

`<deki/providers/Vector.h>`. `Buffer<T>`는 할당할 때 알고 있는 개수를 다룹니다. 개수가 바뀌고 고정된 상한이 없을 때, 장치에서 `std::vector`는 잘못된 답입니다. `push_back`은 `operator new`를 호출하는데, 예외가 꺼진 상태에서는 고갈을 알릴 수 없으므로 중단되고 보드가 재부팅됩니다.

`Deki::Vector`는 같은 형태에서 여기서 중요한 한 가지만 바꾼 것입니다. 할당할 수 있는 모든 것이 `bool`을 반환합니다.

```cpp
Deki::Vector<Enemy> enemies(Deki::Memory::Internal);

if (!enemies.Reserve(32))            // out of memory, said once
    return;

if (!enemies.PushBack(spawned))      // and per growth, if it has to grow
    DEKI_LOG_WARNING("dropped a spawn");
```

- **영역은 생성자 인수이므로**, 이 목록이 어떤 메모리를 차지하는지 선언에서 알 수 있고 모든 증가가 같은 곳에 할당됩니다. 기본 생성자는 없습니다. 증가하는 벡터는 새 블록을 어디에 둘지 알아야 하며, 엔진은 대신 결정하지 않습니다.

- **`Reserve`를 미리 호출하면 이후의 푸시는 실패하지 않습니다.** 요소마다가 아니라 한 번만 검사하면 되며, 장치에서 작성할 가치가 있는 패턴입니다.
- 증가는 16이 아니라 **4로 시작하는 첫 블록에서 두 배씩** 늘어납니다. 장치 프로젝트에는 짧은 목록이 많으며, 각각에 큰 기본값을 두면 목록 자체보다 더 많은 메모리를 차지하게 됩니다.
- **증가에 실패해도 이미 저장된 것은 모두 그대로 남으므로**, 새 요소를 버린 호출자도 여전히 제대로 동작하는 벡터를 갖게 됩니다.
- `RemoveUnordered(i)`는 O(1)이며 빈자리를 마지막 요소로 채웁니다. `ParticlePool::KillSwap`이 쓰는 것과 같은 기법으로, 살아 있는 엔티티 목록에 적합합니다. `Clear()`는 다음 프레임에 다시 채울 수 있도록 메모리를 유지하고, `Reset()`은 해제합니다.
- `T`에 대한 제약은 `Buffer`와 같습니다. 트리비얼하게 복사 가능하고 트리비얼하게 소멸 가능해야 합니다. 증가 시 요소는 `memcpy`로 이동합니다.

먼저 `Buffer`를 고려하십시오. 여기서 "동적"인 데이터 대부분은 프로젝트가 이미 아는 상한이 있는 것으로 밝혀지며, 고정 할당은 반복적인 증가와 달리 작은 힙을 단편화시키지 않습니다.

### `std::vector`, `std::string`, 일반 `new`

이것들은 `Deki::Buffer<T>`가 대체할 수 없는 `operator new`를 호출합니다. 장치 빌드에서는 엔진이 연산자 자체를 대체하며([`src/providers/OperatorNew.cpp`](../../deki-engine/src/providers/OperatorNew.cpp)), 이로 인해 두 가지가 바뀝니다.

- **할당이 실패하면 그 사실을 알립니다.** 요청한 크기를 로그로 남긴 뒤 중단합니다. 예외가 꺼진 상태에서 throw하는 `operator new`에는 다른 선택지가 없기 때문입니다. "Out of memory asking for 40960 bytes"는 진단이지만, 이것이 대체한 재부팅은 진단이 아니었습니다.
- **`std::nothrow`가 동작합니다.** `new (std::nothrow) T[n]`은 중단하지 않고 `nullptr`를 반환하므로, 고갈을 직접 처리하려는 코드가 그렇게 할 수 있습니다. 대체가 없으면 중단됩니다. libstdc++는 nothrow 형식을 제거된 `try`/`catch` 안에서 throw 형식을 호출하는 방식으로 구현하기 때문입니다.

이것들도 엔진을 **거칩니다**. `operator new`는 `IMemoryProvider::AllocateRaw`를 호출하므로, 보드는 장치의 모든 할당을 보고 그 전부에 대해 자신의 힙으로 대체할 수 있습니다.

다만 이것은 `Memory::Allocate`가 *아닙니다*. 그렇게 하면 작은 문자열 버퍼를 넘어서는 모든 `std::string` 앞에 16바이트 헤더가 붙고, 벡터가 곧 덮어쓸 메모리를 memset하고, 작은 블록에도 ESP32 프로바이더의 16바이트 SIMD 정렬 비용을 치르게 됩니다. 게다가 ESP-IDF는 정렬된 할당을 초과 할당으로 구현하므로 헤더보다 낭비가 더 큽니다. `AllocateRaw`에는 그런 것이 전혀 없습니다. 자연 정렬, 헤더 없음, 힙이 반환한 포인터 그대로입니다.

측정 결과(48바이트 할당/해제 쌍 160만 회, 엔진 `-O2`):

| | ns/op |
|---|---|
| `malloc` + `free` | 47-52 |
| 프로바이더를 거침 | 46-51 - 구별할 수 없음 |
| 프로바이더를 거침 + `DEKI_MEMORY_TRACK_RAW` | 63-71 (+35%) |

따라서 경유 자체는 비용이 없고 항상 켜져 있으며, **바이트 집계는 옵트인입니다**. 헤더가 없으므로 블록 크기를 힙에서 가져와야 하는데, Windows에서는 그것이 잠금 아래에서 `HeapSize`를 호출하는 `_msize`이기 때문입니다. 메모리가 어디에 쓰였는지 추적할 때는 `DEKI_MEMORY_TRACK_RAW`로 빌드하십시오. 그러면 `Memory::LogStats`가 컨테이너가 보유한 양을 보고합니다. 이 옵션이 없으면 해당 줄은 오해를 부르는 0을 출력하지 않고 집계하지 않는다고 표시합니다.

헤더와 함께 매직 값도 포기하므로, **컨테이너 메모리의 이중 해제는** 이 경로가 일반 `malloc`이던 때와 마찬가지로 **진단할 수 없습니다**. 엔진이 설명할 수 있는 것은 모두 `Allocate`를 거치며 방지 장치를 유지합니다.

배치는 의도적으로 여기서 결정하지 않습니다. 제거된 `Auto` 규칙처럼 크기에 따라 경로를 정하면 큰 `std::vector`가 조용히 PSRAM으로 옮겨져, 코드에서는 보이지 않는 이유로 핫 루프가 느려질 것입니다. 외부 메모리를 원하는 호출자는 `Deki::Memory::External`을 지정하고 `Deki::Buffer`를 사용합니다. `operator new`는 알 수 없으므로 추측하지 않습니다.

이것은 `__cpp_exceptions`가 정의되지 않은 곳에서만, 즉 정확히 중단 문제가 존재하는 빌드에서만 컴파일됩니다. 호스트 빌드와 에디터 빌드는 표준 연산자를 유지합니다. 초과 정렬 형식(`align_val_t`)은 짝이 맞도록 libstdc++에 맡깁니다.

정리하면, 런타임에 바뀌는 개수에는 `Deki::Vector<T>`를 쓰고 `false`를 처리하십시오. 크기가 파일, 씬, 화면에서 오는 것에는 `Deki::Buffer<T>`를 쓰십시오. 에디터와 도구 코드에는 `std::vector`가 여전히 적합하며, 장치에서도 그 고갈은 이제 조용한 중단이 아니라 로그가 남는 중단입니다.

## 플랫폼 계층: `IMemoryProvider`

`<deki/providers/IMemoryProvider.h>`. 플랫폼은 이를 하나 제공하여 `Memory::Initialize()` 전에 넘겨줍니다.

```cpp
Deki::Memory::SetBackend(new MyBoardMemoryProvider());  // takes ownership
Deki::Memory::Initialize();
```

이 인터페이스는 외부 RAM(`HasExternalRAM`, 총량, 여유 공간)을 보고하고 각 영역에서 할당합니다: `AllocateExternal` / `FreeExternal`, `AllocateInternal` / `FreeInternal`, `AllocateDMA`.

**배치 관련 가상 함수는 모두 순수 가상 함수이므로 전부 구현하십시오.** 프로바이더는 각 영역이 자신의 플랫폼에서 무엇을 뜻하는지 명시합니다. 답이 "여기서는 다른 것과 같다"인 경우에도 마찬가지입니다. `AllocateDMA`에 `return malloc(size);`를 작성한 데스크톱 프로바이더는 읽고 확인할 수 있는 주장을 한 것입니다.

중요한 것은 `AllocateDMA`입니다. 이를 `AllocateInternal`로부터 상속할 수 있는 프로바이더는, 둘이 다른 칩이라면 어디서든 디스플레이 주변 장치가 접근할 수 없는 메모리를 넘겨주게 됩니다. 빌드에서 그것을 알려 주는 것은 아무것도 없는 조용한 손상입니다.

`GetAvailableInternalRAM`도 같은 이유로 필수입니다. 의도 없이 0을 답하는 프로바이더는 시작 시 로그가 고갈된 보드를 보고하게 만듭니다.

순수 가상 함수는 구현된 가상 함수와 같은 vtable 슬롯을 차지하므로 ABI 버전을 올릴 필요가 없습니다. 프로바이더가 결정할 때까지 컴파일되지 않게 할 뿐입니다.

`AllocateRaw` / `FreeRaw` / `GetRawBlockSize`는 네 번째 쌍과 조회 함수입니다. 전역 `operator new`가 경유하는 일반 자연 정렬 할당자로, 보드의 힙이 엔진이 배치하는 모든 것뿐 아니라 `std::vector`와 스칼라 `new`도 처리하게 합니다. 기본값은 `malloc`/`free`이며, `GetRawBlockSize`는 플랫폼이 답할 수 없으면 0을 반환합니다. 이 경우 `LogStats`의 정확도만 떨어집니다. 쌍은 둘 다 재정의하거나 둘 다 재정의하지 마십시오.

`AllocateDMA`가 `AllocateInternal`과 분리되어 있는 이유는, ESP32-S3에서 내부 RAM 대부분은 DMA로 접근할 수 있지만 전부는 아니며, 디스플레이 주변 장치가 접근할 수 없는 프레임버퍼를 읽으면 오류가 아니라 조용한 손상이 일어나기 때문입니다. ESP32 프로바이더는 `MALLOC_CAP_DMA`를 명시적으로 요청합니다. `AllocateInternal`/`FreeInternal` 중 하나를 재정의하는 쪽은 둘 다 재정의해야 합니다. 쌍이 일치해야 하기 때문입니다(ESP32 프로바이더는 16바이트 정렬의 `heap_caps_aligned_alloc`을 사용하며, 이는 블릿 디스패처가 S3 SIMD 커널을 사용할 수 있게 해 주기도 합니다. 그리고 `heap_caps_free`를 사용합니다).

**`IMemoryProvider`는 패키지 ABI의 일부이므로**, 그 가상 함수는 [`tools/ci/check_abi.py`](../../tools/ci/check_abi.py)로 고정됩니다. 가상 함수를 추가, 제거하거나 순서를 바꾸면 vtable 슬롯이 이동하므로, 같은 커밋에서 `kDekiReflectionAbiVersion`을 올려야 합니다.

## 린트

```bash
python tools/ci/check_allocations.py            # packages beside the repo
python tools/ci/check_allocations.py --verbose  # also list files and waivers
```

런타임 코드의 `new[]`, `delete[]`, `malloc`/`calloc`/`realloc`, `heap_caps_*`를 실패로 처리합니다. **모든 패키지의 코드뿐 아니라 엔진 자신의 코드도** 대상입니다. 엔진의 할당은 모든 보드의 모든 프로젝트에 영향을 주지만 패키지의 할당은 그 패키지를 설치한 프로젝트에만 영향을 주므로, 오히려 엔진 쪽이 더 중요한 절반입니다. 이 린트의 첫 버전에서 엔진을 제외했더니 즉시 그 점이 증명되었습니다. `ProjectSettings`가 장치용 설정 청크를 맨 `realloc`으로 유지하고 있었던 것입니다.

제외 대상: `editor/`, `tests/`, `generated/`, `third_party/`, 그리고 엔진의 `platforms/editor`와 `platforms/desktop` 계층입니다. 이들은 모두 예외가 켜져 있고 힙이 데스크톱의 힙인 호스트에서 실행됩니다. 규칙을 어겨도 되는 코드여서가 아니라 스택의 최하단이기 때문에 경로로 면제되는 것: `providers/Memory.cpp`, `providers/IMemoryProvider.h`, 모든 `*MemoryProvider.*`, 그리고 `providers/OperatorNew.cpp`입니다. `providers/OperatorNew.cpp`는 다른 어떤 것도 `operator new[]`를 정의할 필요가 없도록 직접 정의해야 합니다.

일치를 검사하기 전에 주석과 문자열 리터럴을 비워 두므로, 이 규칙을 설명하는 주석이 걸리지는 않습니다.

정말로 피할 수 없는 경우에는 **이유를 적은** 면제를 사용합니다. 이유가 없는 면제는 인정되지 않으므로, diff에는 항상 어떤 예외가 논의를 거쳤는지 드러납니다.

```cpp
void* p = malloc(n);  // deki-alloc-ok: the C API frees this itself
```

이 린트는 이미 제 몫을 했습니다. 수동 점검에서 놓쳤던, `Standard2DRenderer`에서 `Deki::Memory` 버퍼에 대해 `delete[]`를 호출하던 코드를 찾아냈습니다.
