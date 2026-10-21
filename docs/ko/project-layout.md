# 프로젝트 구조

Deki 프로젝트가 디스크에서 어떻게 생겼는지, 에디터가 무엇을 생성하는지, 그리고 생성된 상태가 잘못되었을 때 어떻게 복구하는지 설명합니다.

## 구조

```
<project>/
├── deki.json                    project identity and entry points
├── deki-packages.json           declared package dependencies
├── deki-packages-lock.json      resolved versions
│
├── src/                         gameplay code
├── assets/                      sprites, fonts, scenes, data, rendering
├── packages/                    installed package sources
├── platforms/                   build targets
│
├── settings/                    project settings
│
└── generated/                   everything the editor generates
    ├── runtime/                 package DLLs the editor loads
    ├── build/                   CMake/Ninja tree
    ├── cache/                   asset cache
    ├── shadow/                  hot-reload copies of runtime/
    └── CMakeLists.txt           regenerated on open/build
```

`platforms/`에는 패키지에서 복사해 온 것을 포함하여 빌드 타겟마다 디렉터리가 하나씩 있습니다. [platforms.md](platforms.md)를 참조하십시오.

`generated/` 아래의 모든 것은 지워도 됩니다. 그래서 프로젝트의 `.gitignore`는 목록이 아니라 한 줄입니다. 목록은 실제와 어긋나기 마련이고, 실제로 어긋났습니다. 아무것도 만들지 않는 `build_refl/`을 나열하면서, 추적 대상이던 `settings/` 폴더를 숨기고 있었습니다.

## 이식성

프로젝트 디렉터리는 다른 컴퓨터에 복사하여 그대로 열 수 있습니다. 프로젝트의 어떤 것도 절대 경로를 기록하지 않습니다.

- 경로는 런타임에 프로젝트 루트로부터 도출됩니다. 구조의 유일한 정의인 `src/core/ProjectPaths.h`를 참조하십시오.
- 생성된 `CMakeLists.txt`는 자신의 위치로부터 `DEKI_PROJECT_ROOT`를 도출하고, 엔진과 에디터의 위치는 구성 시점에 에디터로부터 받습니다(`-DDEKI_ENGINE_PATH` / `-DDEKI_EDITOR_DIR`, `EditorSettings::GetCMakeLocationArgs`가 만듦). 예전에는 이것들을 파일에 직접 기록했기 때문에, 에디터 설치 위치가 바뀌는 순간 깨졌습니다.
- `src/generated/` 아래에 커밋되는 리플렉션 출력은 상대 소스 경로를 사용합니다.

**에디터**를 옮기는 것도 똑같이 안전합니다. 엔진 경로는 호출할 때마다 실행 중인 실행 파일로부터 결정되며(`EditorSettings::GetEnginePath`) 저장되지 않습니다. 다운로드한 에디터는 옆에 있는 `sdk/` 폴더에서 경로를 찾고(`EditorSettings::GetSdkDir`), 빌드 트리에서 실행한 에디터는 빌드 폴더를 둘러싼 소스 체크아웃에서 찾습니다.

## 복구

**편집 → 유지 관리**를 사용하십시오. 작업은 비용이 낮은 순서로 나열되어 있으며, 낮은 비용의 작업으로 해결되지 않을 때만 다음 단계로 넘어가십시오.

| 증상 | 작업 | 비용 |
|---|---|---|
| 패키지가 오래되었거나, 제거한 패키지가 여전히 로드됨 | 패키지 사본 초기화 | 몇 초 - 다음 로드 때 다시 복사됨 |
| 가져온 뒤 에셋이 잘못되었거나 오래되어 보임 | 에셋 캐시 지우기 | 에셋을 다시 가져옴 |
| 링커나 CMake의 혼란, 구성 실패 | 클린 빌드 | 전체 재컴파일 |
| 처음부터 다시 시작 | 전체 정리 | 재빌드 + 다시 가져오기 |

각 작업은 `generated/` 아래의 디렉터리 하나에 대응합니다. 직접 삭제해도 같은 결과가 되며, 메뉴는 구조를 외우지 않아도 되도록 존재합니다. 플레이 모드나 빌드가 실행 중일 때는 비활성화됩니다. 매핑된 DLL을 제거하면 도중에 실패하여 시작하기 전보다 더 나쁜 상태가 되기 때문입니다.

## shadow/가 runtime/과 나란히 있는 이유

패키지는 `generated/runtime/`에서 직접 로드되지 않고 `generated/shadow/`에 있는 사본에서 로드됩니다. Windows는 매핑된 DLL을 잠그므로, 사본을 로드하면 원본은 쓰기 가능한 상태로 남아 에디터가 현재 로드된 패키지를 다시 빌드할 수 있습니다.

`shadow/`는 의도적으로 `runtime/` 안이 아니라 옆에 있습니다. `runtime/`은 복사 *원본*이며, 대상을 원본 안에 두면 로더가 자신의 사본까지 열거하게 됩니다. `shadow/owner.pid`는 소유 프로세스를 기록하므로, 비정상 종료된 인스턴스가 남긴 것은 수동으로 삭제하지 않아도 자동으로 회수됩니다.

## 구조 버전

`deki.json`에는 `"structure"`가 있습니다. 에디터는 로드할 때 이를 `ProjectPaths::kStructureVersion`과 비교하여 구조 마이그레이션을 한 번 실행한 뒤 새 버전을 기록합니다. 따라서 각 마이그레이션이 디렉터리 모양으로부터 상태를 추측할 필요 없이, 마이그레이션되지 않은 프로젝트도 항상 자신을 설명할 수 있습니다.
