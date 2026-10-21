# 语言

编辑器以英语编写，可以显示为英语、日语（`ja`）、简体中文（`zh`）或韩语（`ko`），在“编辑 > 编辑器设置 > 语言”中或项目列表中选择。所选语言会立即生效，并在下次会话中保留。

除英语外，每种语言都是一张从英文文本到译文的对照表，每种语言一个 JSON 文件：

- 编辑器的：`resources/lang/<code>.json`，例如 `resources/lang/ja.json`，以及 `resources/lang/<code>/` 中的所有文件（编辑器的日语按区域拆分：`ja/app.json`、`ja/views.json`、...）
- 包的：包内的 `editor/lang/<code>.json`

```json
{
    "Save": "保存",
    "%d objects": "%d 個のオブジェクト"
}
```

以图标开头的标签在查找时会去掉图标：`"New Project"` 的条目同样适用于 `ICON_TI_PLUS "  New Project"`。

没有条目的文本会以英语显示，因此对照表可以一点一点地补充。允许使用注释（`//`），用于给下一位译者留言。当编辑器和某个包翻译了同一段文本时，使用编辑器的译文。

## 翻译对照表

用以下命令查找缺少的内容：

```
python tools/ci/check_translations.py --missing
python tools/ci/check_translations.py --missing --package ../packages/deki-2d
```

它会列出代码中显示、但对照表中尚未包含的每段文本，以及其文本已不在代码中的每个条目。

要查看正在运行的编辑器中哪些内容未翻译，请使用 `DEKI_LOC_MISSING=<file>` 启动编辑器：每段没有译文的绘制文本都会写入该文件一次。

译文中的 `%d`、`%s` 和 `%.1f` 要保持与英文相同的顺序；这些值会按该顺序填入。

## 在代码中显示文本

用英语编写，并在绘制时通过 `Tr` 传递（`<deki-editor/Localization.h>`）：

```cpp
ImGui::TextUnformatted(Tr("No scene open"));
snprintf(buf, sizeof(buf), Tr("%d objects"), count);
```

ImGui 标签同时也是控件的 ID，因此要通过 `TrLabel` 传递，它能让 ID 在所有语言中保持不变。凡是按名称查找的东西（例如弹出窗口），两侧都要使用它：

```cpp
if (ImGui::Button(TrLabel("Save")))
ImGui::OpenPopup(TrLabel("Add Screen Size"));
BeginSchematicModal(TrLabel("Add Screen Size"), 360.0f);
```

保存在标签表中的文本，在编写处用 `TrMark` 标记，在绘制处传给 `Tr`：

```cpp
constexpr const char* kModes[] = { TrMark("Automatic"), TrMark("Off") };
```

共享的辅助函数会翻译传给它们的内容，因此调用方传入普通英文即可：分区标题（`SchematicSectionBegin`）、属性行（`BeginPropertyRow`、`EditorUI::PropertyRow`）、行的工具提示、`Tooltip`、`SchematicCombo` 的选项以及 `SchematicDialogButtons` 的按钮。如果给它们传入 `Tr(...)`，就会翻译两次。

来自项目的名称和路径（对象、资源、文件）绝不通过 `Tr` 传递。

## 包自身的名称

检视器中显示的包组件名称来自反射，并由编辑器翻译：组件名称（"Sprite Component"）、类别、描述、属性名称（"tintColor" 显示为 "Tint Color"）、工具提示、分组、按 C++ 中写法的枚举值（"TopLeft"）、创建菜单项以及资源类型名称。请将它们放入包的 `editor/lang/<code>.json`；编辑器会在项目打开时读取项目中每个包的对照表。`check_translations.py --package` 会根据包生成的反射代码（`generated/*Reflection*.gen.cpp`，包的任何一次构建都会写入）列出这些名称。

所有对照表还会进入同一个共享查找表，其中编辑器的对照表优先读取。当编辑器绘制某个包的组件或节点时（检视器、添加组件、项目设置、节点图窗口），会优先查找该包自己的对照表，因此包可以按自己的方式翻译某个词：deki-fsm 的 "Add" 是算术运算 加算，而编辑器的 "Add" 按钮仍是 追加。自行绘制包中某个类的代码也用 `Localization::ScopedPackage scope(meta->package);` 做同样的事，其中 `Deki::ComponentMeta::package` 和 `DekiNodeMeta::package` 指明该类所在的包。

## 字体

Noto Sans JP、SC 和 KR 合并在每种编辑器字体之后，因此在任何语言下都能绘制日文、中文和韩文文本，包括英文编辑器中的文件名和项目名。字形在首次显示时绘制。汉字、CJK 标点和全角字符在这三种字体中都有，但字形不同；由当前语言的字体绘制它们（`SetCjkPreference`，其他语言均使用日文字体）。

Tabler 图标字体仅限编辑器使用的图标（私用区，以及 U+FA17）：它在全角标点的位置上也有图标，否则会替换掉 "（" 和 "，"。
