#pragma once

// The Targets section of an asset's inspector: what an asset stores on each of
// the project's platforms. A stack of cards, Default first, holding what every
// target uses, then one per target, each able to override those values. A
// folded card shows its header and a summary line of what it stores. A value
// that follows Default is dimmed and says what that gives the target; a
// target with an override has a dot. Each property is a row, so a new one is a
// new row, not a narrower control.
//
// The image inspector draws its Format and Max Size with these, and a package
// draws its own asset type's per-target settings the same way (deki-3d's
// models: Max Size, filtering, triangles).

#include <deki-editor/EditorExport.h>

#include <functional>
#include <string>
#include <vector>

namespace DekiEditor
{

struct PlatformConfig;

/// The platforms this project can build for, in the registry's order: its
/// own (platforms/<id>/), those its packages offer, and the installed board
/// packs', as ReloadProjectPlatforms fills the registry for it. The same list
/// as the Build panel's, so every platform a build can target has its card.
DEKI_EDITOR_API std::vector<const PlatformConfig*> ProjectTargets(const std::string& projectPath);

/// Starts the Targets section. False when it is folded: draw nothing more and
/// do not call TargetCardsEnd.
DEKI_EDITOR_API bool TargetCardsBegin();
DEKI_EDITOR_API void TargetCardsEnd();

/// One card's header.
struct TargetCardInfo
{
    const char* id = "";           // unique within the section
    bool overridden = false;       // shows the dot
    std::string name;              // "Default", or the platform's name
    bool dimName = false;          // a target the project no longer has
    const char* note = nullptr;    // right of the name ("all targets", "12.0 KB")
    const char* noteTip = nullptr;  // tooltip over the note
    const char* nameTip = nullptr;  // tooltip over the rest of the header
    std::string summary;           // the folded card's one line
};

/// A card. `body` draws its rows when it is open, given where labels start,
/// where values start, and how wide a value may be.
DEKI_EDITOR_API void TargetCard(const TargetCardInfo& info, bool& open,
                                const std::function<void(float labelX, float valueX, float valueW)>& body);

/// A row inside a card: the label (translated), then the cursor at valueX for
/// its control.
DEKI_EDITOR_API void TargetCardRow(const char* label, float labelX, float valueX);

/// A dropdown whose closed box shows `preview`, dimmed when it comes from
/// Default, and whose list starts with `defaultLabel` (choosing it sets
/// `index` to firstIndex - 1), then label(firstIndex) .. label(count - 1),
/// translated. True when `index` changed.
DEKI_EDITOR_API bool TargetOverrideCombo(const char* id, float width, const char* preview, bool fromDefault,
                                         const std::string& defaultLabel, const std::function<const char*(int)>& label,
                                         int count, int firstIndex, int& index);

}  // namespace DekiEditor
