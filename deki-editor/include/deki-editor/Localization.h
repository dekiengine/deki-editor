#pragma once

// The editor's languages. Source code is written in English; each other
// language is a table from the English text to the translation, one JSON file
// per language (resources/lang/ja.json for the editor, editor/lang/ja.json in
// a package). Text without a translation shows in English.
//
// Tr() is for text that is only shown. TrLabel() is for an ImGui label, which
// is also the widget's ID: it keeps the ID the same in every language, so
// switching language keeps open popups, tree nodes and window layouts.
//
//     ImGui::TextUnformatted(Tr("No scene open"));
//     if (ImGui::Button(TrLabel("Save")))
//     ImGui::OpenPopup(TrLabel("Add Screen Size"));   // the same on both sides
//     BeginSchematicModal(TrLabel("Add Screen Size"), ...);
//
// Text kept in a table is marked where it is written and translated where
// it is shown:
//
//     constexpr const char* kModes[] = { TrMark("Automatic"), TrMark("Off") };
//     ImGui::TextUnformatted(Tr(kModes[i]));
//
// A format string is translated before formatting:
//
//     snprintf(buf, sizeof(buf), Tr("%d objects"), count);
//
// Lives in deki-editor.dll so package editors share the one table and the
// one current language. Main thread only.

#include <cstdint>
#include <string>
#include <vector>

#include <deki-editor/EditorExport.h>

namespace DekiEditor
{

/// `english` in the current language, or `english` itself when there is no
/// translation. Icons and spaces in front of the text are kept and not
/// looked up: ICON_TI_PLUS "  New Project" uses the entry for "New Project".
/// The result lives as long as the editor.
DEKI_EDITOR_API const char* Tr(const char* english);

/// An ImGui label in the current language: the translated text before any
/// "##", then "###" and the English label, so the ID never changes. A label
/// that already has "###" keeps its own ID part. A label that starts with
/// "##" shows nothing and is returned as it is.
DEKI_EDITOR_API const char* TrLabel(const char* label);

/// Marks English text for translation where it is written, in a table of
/// labels, without translating it: whatever shows it later passes it to Tr().
/// tools/ci/check_translations.py finds the text by this mark.
constexpr const char* TrMark(const char* english)
{
    return english;
}

namespace Localization
{

struct Language
{
    std::string code;  // "en", "ja", "zh" (Simplified Chinese), "ko"
    std::string name;  // in its own language: "English", "日本語", "简体中文", "한국어"
};

/// Every language the editor offers, English first.
DEKI_EDITOR_API const std::vector<Language>& Languages();

/// The language text is shown in, such as "en" or "ja".
DEKI_EDITOR_API const std::string& Current();

/// Shows text in another language from now on and remembers it for the next
/// session. An unknown code means English.
DEKI_EDITOR_API void SetLanguage(const std::string& code);

/// Shows text in another language for this session only, without saving it.
/// An unknown code means English.
DEKI_EDITOR_API void UseLanguage(const std::string& code);

/// Reads the language saved in the editor settings and the editor's own
/// tables. Called once at startup, after the settings load.
DEKI_EDITOR_API void Load();

/// Adds the translations in `dir`/<code>.json and in every file of
/// `dir`/<code>/. They go into one shared table, where a key the editor (or an
/// earlier package) already translates keeps that translation. With a
/// `package` ("deki-fsm"), they are also that package's own table, which wins
/// inside its ScopedPackage.
DEKI_EDITOR_API void AddTableDirectory(const std::string& dir, const std::string& package = "");

/// Changes whenever the language or a table changes, for caches of
/// translated text.
DEKI_EDITOR_API uint32_t Generation();

/// While it lives, Tr and TrLabel look in `package`'s own table first, so
/// text a package's class shows (its component, property and enum names) reads
/// as that package translates it even when the editor or another package
/// translates the same English word differently: deki-fsm's "Add" (an
/// arithmetic operation) is not the editor's "Add" button. Draw each
/// component or node inside one, with Deki::ComponentMeta::package or
/// DekiNodeMeta::package. nullptr or "" means the shared table only. Scopes
/// nest.
class DEKI_EDITOR_API ScopedPackage
{
public:
    explicit ScopedPackage(const char* package);
    ~ScopedPackage();
    ScopedPackage(const ScopedPackage&) = delete;
    ScopedPackage& operator=(const ScopedPackage&) = delete;
};

}  // namespace Localization

}  // namespace DekiEditor
