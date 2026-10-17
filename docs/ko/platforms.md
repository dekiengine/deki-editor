# 플랫폼

**플랫폼**은 프로젝트를 빌드하는 대상 하나입니다. 보드, 데스크톱 시뮬레이터, 콘솔 등이 해당합니다. 이 페이지에서는 플랫폼이 무엇으로 이루어져 있는지, 플랫폼이 어디에서 오는지, 패키지가 보드를 어떻게 제공하는지, 그리고 프로젝트가 보드를 사용하기 시작한 뒤 보드가 어떻게 되는지 설명합니다.

## 플랫폼이란

프로젝트의 `platforms/` 아래에 있는, 플랫폼 ID로 이름 붙은 디렉터리입니다.

```
<project>/platforms/<id>/<id>.json    the platform
<project>/platforms/<id>/boot.scene   the scene a build of it boots
<project>/platforms/<id>/origin.json  only if it came from a package (see below)
```

JSON은 에디터가 알아야 할 것을 기술하며, 빌드 방법에 대해서는 아무것도 기술하지 않습니다.

| 키 | 의미 |
| --- | --- |
| `id`, `displayName` | ID는 디렉터리 이름이기도 합니다. |
| `framework` | 어떤 빌드 백엔드가 빌드하는지(`espidf`, `native`, ...). 기본값은 없습니다. 이를 지정하지 않은 플랫폼에는 백엔드가 없으며, 빌드가 그렇게 알려 줍니다. |
| `frameworkOptions` | 해당 백엔드 고유의 설정: 칩, 플래시 크기, sdkconfig 줄, 핀. 에디터는 이를 보관만 하고 읽지 않으며, 백엔드가 빌드할 때 검증합니다. |
| `family` | 설명용일 뿐입니다. 케이퍼빌리티 `family:<x>`가 됩니다. |
| `screenWidth`, `screenHeight`, `colorFormat` | 이 타겟의 화면입니다. 에디터는 이를 미리 보여 주고, 데스크톱 시뮬레이터는 이 크기의 창을 열며, 자동으로 설정된 이미지는 이 타겟용으로 `colorFormat`으로 저장됩니다. 장치는 디스플레이 고유의 크기와 포맷으로 렌더링합니다. [screens.md](screens.md)를 참조하십시오. |
| `externalMemorySize` | 내부 힙 외의 메모리 바이트 수(마이크로컨트롤러의 PSRAM). 패키지에 `memory:external`을 부여합니다. |
| `provides` | 타겟이 패키지에 제공할 수 있는 것: `wifi`, `spi`, `desktop-os`, ... |
| `requiresPackages` | 씬이 무엇을 사용하든 타겟이 항상 필요로 하는 패키지. 일반적으로 하드웨어 추상화 계층입니다. |
| `defines`, `cFlags`, `cxxFlags`, `linkFlags` | 게임 자체 코드의 컴파일러 설정. |
| `bootScene` | 프로젝트 기준 부트 씬의 경로. |
| `assetStorage` | 빌드가 게임의 에셋을 두는 곳: `internal`(기본값) 또는 `external`. 아래를 참조하십시오. |

**부트 씬**에는 `Deki::PlatformSetupComponent`가 있으며, 그 `setupComponents` 목록은 하드웨어를 기동하는 설정 단계를 순서대로 지정합니다: 전원 핀, 디스플레이, I2C 버스, 터치. 각 항목은 `{id, typeId}`이며, 단계를 담은 자식 오브젝트의 ID와 단계의 정규화된 클래스 이름의 해시(`Deki::HashString`)입니다. `typeId`가 틀려도 실패하지 않으며, 그 단계가 조용히 실행되지 않을 뿐입니다. 에디터 테스트의 `ShippedPlatformTemplates`가 패키지가 제공하는 모든 부트 씬을 검사합니다.

## 에셋이 놓이는 곳

빌드는 게임의 에셋을 두 곳 중 하나에 둡니다.

- **내부 스토리지**(기본값): 장치의 일부입니다. 빌드가 에셋을 부트 씬 옆의 `assets/`(런타임에는 `F:/assets/`)에 담아 갑니다. ESP32에서는 플래시이며, 데이터 파티션이 칩의 나머지 공간(16 MB 보드에서 약 13 MB)을 차지하고, 게임이 들어가지 않으면 빌드가 실패합니다. 데스크톱 시뮬레이터와 Tactility 앱도 같은 방식으로 동작합니다.
- **외부 스토리지**: 이동식인 SD 카드입니다. 빌드는 부트 씬과 `project_data.bin`만 담아 갑니다. **SD 카드 배포** 패널이 에셋을 카드에 복사하고, 카드가 마운트되면 카드의 스토리지 패키지가 에셋을 로드합니다(`S:/`).

외부를 선택할 수 있는 것은 `external-storage`를 `provides`하는 플랫폼뿐이며, 플랫폼 에디터는 그런 플랫폼에만 이 선택지(**스토리지 > 에셋 위치**)를 표시합니다. 플래시에 들어가지 않을 만큼 큰 게임에만 선택하십시오. 외부 스토리지를 선택했는데 슬롯에 카드가 없으면 게임에는 에셋이 없습니다. LilyGO T-Deck Plus는 선택할 수 있으며, 따로 지정하지 않으면 에셋을 플래시에 둡니다.

`project_data.bin`이 이 선택을 기록하므로, 엔진은 어디를 봐야 하는지 압니다.

## 보드 시뮬레이션

백엔드에 시뮬레이터가 있는 보드(ESP32 또는 ESP32-S3의 QEMU)는 시뮬레이터에서 실행되도록 빌드할 수 있습니다. 빌드 패널에서 **QEMU에서 시뮬레이션**을 체크한 다음 빌드하고 **플레이**를 누릅니다. 명령줄에서는 다음과 같습니다.

```
DekiEditor --build-firmware --deploy --simulate --platform lilygo_t_deck_plus <project>
```

시뮬레이션 빌드는 보드 부트 씬의 모든 단계를 유지하고, 그 뒤에 시뮬레이터 고유의 단계(QEMU 화면)를 추가한 사본을 빌드 디렉터리 아래(`generated/build/<platform>_qemu/simulation/boot.scene`)에 만듭니다. 시뮬레이터가 단계를 다른 방식으로 실행해야 하는 경우 사본에 그렇게 기록됩니다. 예를 들어 SPI로 연결된 SD 카드는 QEMU의 SD 호스트로 옮겨집니다. 빌드 로그에 각 변경 사항이 나열됩니다. 플랫폼의 `boot.scene`, 일반 빌드, 보드에 플래시되는 펌웨어는 변경되지 않으며, 시뮬레이터에 필요한 것은 그 어디에도 컴파일되어 들어가지 않습니다.

시뮬레이션 보드에는 빌드할 때마다 `simulation/sd_card`에 새로 만들어지는 SD 카드가 있습니다. 에셋을 카드에 두는 타겟이라면 SD 카드 배포 패널이 복사하는 것과 같은 에셋이 들어가고, 내부에 두는 타겟이라면 비어 있습니다.

백엔드는 `ITargetBuilder::SupportsSimulation`, `GetSimulatorName`, `MakeSimulationBootScene`(`deki-editor/build/SimulationBootScene.h`의 `AddSimulationSteps`와 함께)으로 이 기능을 제공하며, 나머지는 `BuildOptions::simulate`를 읽어 처리합니다: 자체 빌드 디렉터리, 카드, 그리고 배포가 하는 일.

## 플랫폼은 어디에서 오는가

빌드 패널의 타겟 목록은 세 가지 출처로 이루어지며, 뒤의 것이 앞의 것을 덮어씁니다.

1. `~/.deki-editor/board-packs/` 아래의 **보드 팩**.
2. 프로젝트에 설치된 **패키지**: 모든 `<package>/platforms/<id>/`.
3. **프로젝트 자신의** `platforms/`.

패키지를 설치하거나 제거하면 목록이 즉시 업데이트됩니다.

## 패키지로 보드 제공하기

패키지 안에 보드마다 디렉터리를 하나씩 둡니다.

```
deki-lilygo-boards/
├── package.json
└── platforms/
    ├── lilygo_t_deck_plus/
    │   ├── lilygo_t_deck_plus.json
    │   └── boot.scene
    └── lilygo_t_display_s3_amoled/
        ├── lilygo_t_display_s3_amoled.json
        └── boot.scene
```

이것이 메커니즘의 전부입니다. 패키지에는 코드가 필요 없습니다. `package.cmake`도, 진입점도 필요 없습니다. 보드를 설치하면 함께 설치되도록 `package.json`의 `requires`에 보드의 부트 씬이 사용하는 패키지를 나열해야 하며, 각 플랫폼은 `requiresPackages`에 하드웨어 추상화 계층을 다시 적어야 합니다.

검사되는 규칙:

- **디렉터리 이름이 플랫폼 ID이며**, JSON은 그 안의 `<id>.json`입니다. 단순한 이름이어야 하며, 경로처럼 보이는 것은 건너뜁니다.
- **설치된 두 패키지가 같은 ID를 제공하면 오류이며**, 누군가 그것을 선택할 때 보고됩니다. ID 앞에 제조사 이름을 붙이십시오(`lilygo_...`).
- **`defines`에 `DEKI_PACKAGE_*`나 `DEKI_FEATURE_*`를 지정할 수 없습니다.** 빌드에 어떤 패키지가 있는지는 리졸버가 답합니다. 항상 어떤 패키지가 필요한 보드는 `requiresPackages`에 그렇게 적습니다.
- **`bootScene`은 패키지가 아니라 프로젝트 기준입니다**: `platforms/<id>/boot.scene`. 빌드되는 것은 채택된 사본입니다.

### 다른 패키지가 필요한 기능

패키지 기능은 기능 자체의 `requires`에 필요한 패키지를 지정할 수 있습니다.

```json
{ "id": "trackball", "componentTypes": ["TrackballComponent"],
  "requires": ["deki-gpio"] }
```

이것들은 패키지와 함께 설치되지 않습니다. 그 기능을 사용하는 보드나 프로젝트가 가져옵니다. T-Deck에는 I2C 버스와 핀이 있으므로 `deki-lilygo-boards`는 `deki-i2c`와 `deki-gpio`를 필요로 하며, 그 덕분에 `deki-input`의 키보드와 트랙볼이 빌드됩니다. `deki-input`을 설치한 데스크톱 프로젝트는 둘 다 받지 않습니다.

빌드는 타겟이 기능을 사용하는 동안에만 그 기능의 패키지를 가져옵니다. 사용 중인 기능의 패키지가 없으면, 무엇을 설치해야 하는지 로그로 남기는 단계로 빌드되고 빌드가 경고합니다. 기능의 코드는 `__has_include`로 패키지를 확인하므로 어느 쪽이든 컴파일됩니다.

본보기로 삼을 것은 `deki-lilygo-boards`입니다. `deki-desktop-integration`은 같은 방식으로 시뮬레이터를, `deki-tactility-integration`은 두 개의 타겟을 제공합니다.

## 채택: 프로젝트가 사본을 받습니다

패키지의 플랫폼을 선택하면(빌드 패널에서, `set_active_platform`으로, 또는 그 플랫폼용으로 빌드하여) 그 디렉터리가 프로젝트의 `platforms/`에 복사됩니다. 그 뒤로는 프로젝트의 파일입니다. 플랫폼 에디터에서 편집하고, 부트 씬을 다른 씬처럼 열고, 커밋하십시오. 프로젝트는 결코 패키지 안에서 빌드하지 않습니다.

프로젝트가 이미 정의한 플랫폼은 채택으로 덮어써지지 않으며, 그 디렉터리에 이미 있는 파일도 유지됩니다.

## 출처: 편집, 업데이트, 복원

채택할 때 플랫폼 옆에 `origin.json`도 작성됩니다. 여기에는 패키지 이름과 **채택 당시의** 템플릿, 즉 플랫폼 JSON과 그 밖의 모든 파일의 해시가 들어 있습니다. 나머지와 함께 커밋하십시오.

채택한 것, 프로젝트가 지금 가진 것, 패키지가 지금 제공하는 것을 모두 알 수 있으므로, 빌드 패널은 타겟 목록 아래에 다음을 표시합니다.

```
From package deki-lilygo-boards 0.17.0 - modified - update available
[Update] [Restore defaults]
```

- **수정됨**: 프로젝트의 사본이 채택 당시와 다릅니다. 마우스를 올리면 무엇이 다른지 표시됩니다.
- **업데이트 있음**: 패키지가 채택 당시와 다른 것을 제공합니다.
- **업데이트**는 패키지가 바꾼 것을 가져오고 프로젝트가 바꾼 것은 유지합니다. 플랫폼 설정은 백엔드의 `frameworkOptions`를 포함하여 하나씩 병합되며, 부트 씬을 비롯한 그 밖의 파일은 통째로 가져오거나 유지합니다. 양쪽이 같은 것을 바꾼 경우에는 프로젝트의 것을 유지하고 그 사실을 표시하며, 패키지의 버전이 새 기준이 되므로 같은 질문은 한 번만 받습니다.
- **기본값으로 복원**은 확인을 거친 뒤 사본을 현재 패키지의 버전으로 교체합니다. 프로젝트에만 있는 파일(에셋 사이드카, 프로젝트 자체의 씬)은 남겨 둡니다.

수정으로 간주되지 않는 것: 에디터가 JSON을 자신의 형식으로 다시 저장하는 것(이전 파일은 백엔드 설정을 최상위에 두지만, 저장하면 `frameworkOptions` 아래로 중첩됩니다), 그리고 git이 바꾼 줄 끝 문자.

출처가 기록되기 전에 채택된 플랫폼은, 다음에 사용될 때 여전히 패키지가 제공하는 것과 일치하면 기록을 받습니다. 다르다면 그 변경이 누구의 것인지 알 수 없으므로 기준을 지어내지 않습니다. *패키지와 다름*으로 표시되며, 업데이트는 할 수 없고 복원만 할 수 있습니다.

패키지를 제거해도 플랫폼은 남아 있으며 계속 빌드됩니다. 패키지가 돌아올 때까지 해당 줄에는 *(설치되지 않음)*이 표시됩니다.

채택된 플랫폼을 삭제하면 다시 선택지에 나타나며, 다시 선택하면 패키지의 현재 버전이 복사되어 들어옵니다.

## 명령줄에서

| 도구 | 하는 일 |
| --- | --- |
| `list_platforms` (또는 `--list-platforms`) | 모든 타겟. 패키지에서 온 타겟에는 `origin`이 붙습니다: `package`, `modified`, `updateAvailable`, `canUpdate`, `canRestore`. |
| `set_active_platform` | 프로젝트의 기본 타겟을 선택하며, 필요하면 채택합니다. |
| `create_platform`, `set_platform_provides` | 프로젝트 자체의 플랫폼을 만들거나 변경합니다. `set_platform_provides`는 `asset_storage`와 `cxx_flags`를 받습니다. |
| `update_platform` | 패키지에서 업데이트합니다. `taken`과 `kept`를 반환합니다. |
| `restore_platform` | 패키지의 버전으로 복원합니다. `taken`을 반환합니다. |

```
DekiEditor --tool update_platform --args '{"platform_id":"lilygo_t_deck_plus"}' <project>
```

## 코드에서

`deki-editor/build/PlatformConfig.h`는 플랫폼과 레지스트리입니다. `deki-editor/build/PlatformTemplates.h`는 이 페이지에서 패키지에 관해 설명한 모든 것, 즉 제공되는 플랫폼 찾기, 채택, 출처 기록, 업데이트와 복원을 담고 있습니다. 둘 다 에디터 SDK에 포함되어 있으므로 패키지의 빌드 백엔드가 사용할 수 있습니다.
