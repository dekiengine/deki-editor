#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace DekiEditor
{

// One remembered project, as shown by the project manager screen.
struct ProjectEntry
{
    std::string path;           // project root directory (the folder holding deki.json)
    std::string name;           // deki.json "name", or the folder name if absent
    std::string platformId;     // active build target, shown as a chip
    std::string engineVersion;  // deki.json "engineVersion": the engine it was last saved with
    int64_t lastOpened = 0;     // unix seconds; 0 = never opened by this editor
    bool missing = false;       // deki.json is gone from disk (set by Refresh)
};

/**
 * @brief The list of projects the project manager screen offers.
 *
 * Persisted to projects.json next to settings.json, deliberately NOT inside
 * EditorSettings: that file is a flat bag of scalars and this is a growing
 * ordered list with its own per-entry metadata. Kept sorted most-recently-
 * opened first, which is the order the screen renders and never has to sort.
 */
class ProjectRegistry
{
public:
    static ProjectRegistry& Get();

    // Most-recently-opened first.
    const std::vector<ProjectEntry>& Entries() const { return m_Entries; }

    // Record a project as opened now: adds it when unknown, re-reads its
    // name/platform from deki.json, moves it to the front, saves. This is the
    // only way an entry is created - the manager screen's Import and the new
    // project dialog both reach it through EditorApp::OpenProject.
    void Touch(const std::string& path);

    // Add without opening (Import picks a folder, then opens it - this exists
    // for callers that want the entry to survive a failed open).
    void Add(const std::string& path);

    // By value on purpose: call sites pass the path of the very entry this
    // erases, so a reference would dangle the moment the vector shrinks.
    void Remove(std::string path);

    // Re-read every entry's metadata from disk and update the missing flags.
    // Cheap enough to call when the screen is shown; not called per frame.
    void Refresh();

    // A directory is a project if it holds a deki.json.
    static bool IsProjectDir(const std::string& path);

private:
    ProjectRegistry() { Load(); }

    void Load();
    void Save();

    // Fills name/platformId/missing from the project's deki.json.
    static void ReadMetadata(ProjectEntry& entry);

    std::vector<ProjectEntry> m_Entries;

    // Upper bound on remembered projects. The list is a convenience, not an
    // archive - past this the oldest entry falls off the back.
    static constexpr size_t kMaxEntries = 64;
};

}  // namespace DekiEditor
