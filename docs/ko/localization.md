# 언어

에디터는 영어로 작성되며 영어, 일본어(`ja`), 중국어 간체(`zh`), 한국어(`ko`) 중 하나로 표시됩니다. 언어는 편집 > 에디터 설정 > 언어 또는 프로젝트 목록에서 선택합니다. 선택은 즉시 적용되며 다음 세션에도 유지됩니다.

영어 이외의 각 언어는 영어 텍스트를 번역문에 대응시키는 테이블이며, 언어마다 JSON 파일 하나입니다.

- 에디터의 테이블: `resources/lang/<code>.json`(예: `resources/lang/ja.json`)과 `resources/lang/<code>/` 안의 모든 파일(에디터의 일본어는 영역별로 나뉘어 있습니다: `ja/app.json`, `ja/views.json`, ...)
- 패키지의 테이블: 패키지 안의 `editor/lang/<code>.json`

```json
{
    "Save": "保存",
    "%d objects": "%d 個のオブジェクト"
}
```

아이콘으로 시작하는 레이블은 아이콘을 뺀 텍스트로 조회됩니다. `"New Project"` 항목은 `ICON_TI_PLUS "  New Project"`에도 적용됩니다.

항목이 없는 텍스트는 영어로 표시되므로 테이블을 조금씩 채워 나갈 수 있습니다. 다음 번역자를 위한 메모로 주석(`//`)을 쓸 수 있습니다. 에디터와 패키지가 같은 텍스트를 번역하면 에디터의 번역이 사용됩니다.

## 테이블 번역하기

빠진 항목은 다음 명령으로 찾습니다.

```
python tools/ci/check_translations.py --missing
python tools/ci/check_translations.py --missing --package ../packages/deki-2d
```

코드가 표시하는 텍스트 중 테이블에 아직 없는 것을 모두 나열하고, 텍스트가 더 이상 코드에 없는 항목도 모두 나열합니다.

실행 중인 에디터에서 번역되지 않은 채 표시되는 텍스트를 보려면 `DEKI_LOC_MISSING=<file>`을 지정하여 에디터를 시작합니다. 번역 없이 그려진 각 텍스트가 해당 파일에 한 번씩 기록됩니다.

번역문의 `%d`, `%s`, `%.1f`는 영어와 같은 순서로 유지하십시오. 값은 그 순서대로 채워집니다.

## 코드에서 텍스트 표시하기

영어로 작성하고, 그릴 때 `Tr`을 거치게 합니다(`<deki-editor/Localization.h>`).

```cpp
ImGui::TextUnformatted(Tr("No scene open"));
snprintf(buf, sizeof(buf), Tr("%d objects"), count);
```

ImGui 레이블은 위젯의 ID이기도 하므로 `TrLabel`을 거칩니다. `TrLabel`은 모든 언어에서 ID를 동일하게 유지합니다. 팝업처럼 이름으로 조회하는 것에는 양쪽 모두에 사용하십시오.

```cpp
if (ImGui::Button(TrLabel("Save")))
ImGui::OpenPopup(TrLabel("Add Screen Size"));
BeginSchematicModal(TrLabel("Add Screen Size"), 360.0f);
```

레이블 테이블에 보관하는 텍스트는 작성하는 곳에서 `TrMark`로 표시하고, 그리는 곳에서 `Tr`에 전달합니다.

```cpp
constexpr const char* kModes[] = { TrMark("Automatic"), TrMark("Off") };
```

공용 헬퍼는 전달받은 텍스트를 직접 번역하므로 호출하는 쪽은 그냥 영어를 전달합니다. 섹션 헤더(`SchematicSectionBegin`), 프로퍼티 행(`BeginPropertyRow`, `EditorUI::PropertyRow`), 행 툴팁, `Tooltip`, `SchematicCombo`의 항목, `SchematicDialogButtons`의 버튼이 이에 해당합니다. 여기에 `Tr(...)`을 전달하면 두 번 번역됩니다.

프로젝트에서 온 이름과 경로(오브젝트, 에셋, 파일)는 절대 `Tr`에 전달하지 않습니다.

## 패키지 고유의 이름

인스펙터에 표시되는 패키지 컴포넌트의 이름은 리플렉션에서 가져오며 에디터가 번역합니다. 컴포넌트 이름("Sprite Component"), 카테고리, 설명, 프로퍼티 이름("tintColor"는 "Tint Color"로 표시됨), 툴팁, 그룹, C++에 작성된 그대로의 열거형 값("TopLeft"), 생성 메뉴 항목, 에셋 타입 이름이 이에 해당합니다. 이것들은 패키지의 `editor/lang/<code>.json`에 넣으십시오. 에디터는 프로젝트를 열 때 프로젝트의 모든 패키지의 테이블을 읽습니다. `check_translations.py --package`는 패키지의 생성된 리플렉션(`generated/*Reflection*.gen.cpp`, 패키지를 빌드하면 작성됨)에서 이것들을 나열합니다.

모든 테이블은 하나의 공유 조회 테이블에도 들어가며, 여기서는 에디터의 테이블을 먼저 읽습니다. 에디터가 패키지의 컴포넌트나 노드를 그리는 동안(인스펙터, 컴포넌트 추가, 프로젝트 설정, 노드 그래프 창)에는 해당 패키지의 테이블을 먼저 조회하므로, 패키지는 어떤 단어를 자기 방식으로 번역할 수 있습니다. deki-fsm의 "Add"는 산술 연산인 加算이고, 에디터의 "Add" 버튼은 그대로 追加입니다. 패키지의 클래스를 직접 그리는 코드도 `Localization::ScopedPackage scope(meta->package);`로 같은 일을 합니다. 여기서 `Deki::ComponentMeta::package`와 `DekiNodeMeta::package`는 클래스가 속한 패키지를 가리킵니다.

## 폰트

Noto Sans JP, SC, KR이 모든 에디터 폰트 뒤에 병합되어 있으므로, 영어 에디터의 파일 이름과 프로젝트 이름을 포함하여 일본어, 중국어, 한국어 텍스트가 어떤 언어에서든 그려집니다. 글리프는 처음 표시될 때 그려집니다. 한자, CJK 문장 부호, 전각 문자는 세 폰트 모두에 있지만 모양이 다릅니다. 이들은 현재 언어의 폰트로 그려집니다(`SetCjkPreference`, 그 밖의 모든 언어에서는 일본어).

Tabler 아이콘 폰트는 에디터가 사용하는 아이콘(사설 영역(Private Use Area)과 U+FA17)으로 한정됩니다. 이 폰트에는 전각 문장 부호 위치에도 아이콘이 있어서, 한정하지 않으면 "（"와 "，"를 대체해 버립니다.
