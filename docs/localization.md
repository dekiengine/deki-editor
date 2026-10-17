# Languages

The editor is written in English and shown in the language picked in
Edit > Editor Settings > Language, or on the project list. The choice applies
at once and is kept for the next session.

Each language other than English is a table from the English text to the
translation, one JSON file per language:

- the editor's: `resources/lang/<code>.json`, such as `resources/lang/ja.json`,
  plus any files in `resources/lang/<code>/` (the editor's Japanese is split by
  area: `ja/app.json`, `ja/views.json`, ...)
- a package's: `editor/lang/<code>.json` inside the package

```json
{
    "Save": "保存",
    "%d objects": "%d 個のオブジェクト"
}
```

A label that starts with an icon is looked up without it: the entry for
`"New Project"` also covers `ICON_TI_PLUS "  New Project"`.

Text with no entry shows in English, so a table can be filled in a little at a
time. Comments (`//`) are allowed, for notes to the next translator. When the
editor and a package translate the same text, the editor's translation is
used.

## Translating a table

Find what is missing with:

```
python tools/ci/check_translations.py --missing
python tools/ci/check_translations.py --missing --package ../packages/deki-2d
```

It lists every text the code shows that the table does not have yet, and every
entry whose text is no longer in the code.

To see what the running editor shows untranslated, start it with
`DEKI_LOC_MISSING=<file>`: each text drawn without a translation is written to
that file once.

Keep the translation's own `%d`, `%s` and `%.1f` in the same order as in the
English; the values are filled in that order.

## Showing text in code

Write English, and pass it through `Tr` when it is drawn
(`<deki-editor/Localization.h>`):

```cpp
ImGui::TextUnformatted(Tr("No scene open"));
snprintf(buf, sizeof(buf), Tr("%d objects"), count);
```

An ImGui label is also the widget's ID, so it goes through `TrLabel`, which
keeps the ID the same in every language. Use it on both sides of anything
looked up by name, such as a popup:

```cpp
if (ImGui::Button(TrLabel("Save")))
ImGui::OpenPopup(TrLabel("Add Screen Size"));
BeginSchematicModal(TrLabel("Add Screen Size"), 360.0f);
```

Text kept in a table of labels is marked with `TrMark` where it is written, and
passed to `Tr` where it is drawn:

```cpp
constexpr const char* kModes[] = { TrMark("Automatic"), TrMark("Off") };
```

The shared helpers translate what they are given, so their callers pass plain
English: section headers (`SchematicSectionBegin`), property rows
(`BeginPropertyRow`, `EditorUI::PropertyRow`), row tooltips, `Tooltip`, the
items of `SchematicCombo` and the buttons of `SchematicDialogButtons`.
Passing them `Tr(...)` would translate twice.

Names and paths from the project (objects, assets, files) are never passed
through `Tr`.

## A package's own names

The names the Inspector shows for a package's components come from
reflection and are translated by the editor: component names ("Sprite
Component"), categories, descriptions, property names ("tintColor" shows as
"Tint Color"), tooltips, groups, enum values as written in C++ ("TopLeft"),
create-menu entries and asset type names. Put them in the package's
`editor/lang/<code>.json`; the editor reads every project package's table
when the project opens. `check_translations.py --package` lists them from the
package's generated reflection (`generated/*Reflection*.gen.cpp`, written by
any build of the package).

Every table also goes into one shared lookup, where the editor's tables are
read first. While the editor draws a package's component or node (the
Inspector, Add Component, Project Settings, the node graph window), that
package's own table is looked up first, so a package can translate a word its
own way: deki-fsm's "Add" is the arithmetic operation 加算 while the editor's
"Add" button stays 追加. Code that draws a package's class itself does the
same with `Localization::ScopedPackage scope(meta->package);`, where
`Deki::ComponentMeta::package` and `DekiNodeMeta::package` name the package
the class is in.

## Fonts

Noto Sans JP is merged behind every editor font, so Japanese text draws in any
language, including Japanese file and project names in an English editor.
Glyphs are drawn the first time they are shown.
