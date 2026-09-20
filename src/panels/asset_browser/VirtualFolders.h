#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace DekiEditor
{

/// The Scripts tab's virtual folders.
///
/// Scripts live wherever they live on disk; these folders are a presentation
/// layer over them, persisted to `script_folders.json` at the project root. A
/// script's assignment is a plain string, and the empty string means root - so
/// deleting a folder reassigns its scripts rather than losing them.
///
/// Split out of AssetBrowserPanel, which had grown to 2904 lines covering the
/// tree UI, the scripts list, virtual folders, GUID reassignment, file moves,
/// script generation and scene saving. This part touches no ImGui at all, which
/// is what lets it be tested.
class VirtualFolders
{
public:
    /// Load from `<projectRoot>/script_folders.json`. Missing or malformed file
    /// leaves the set empty rather than failing - a broken presentation file
    /// must never stop the panel from listing the actual scripts.
    void Load(const std::string& projectRoot);

    /// Write back if anything changed. No-op when the project root is empty.
    void Save(const std::string& projectRoot) const;

    const std::vector<std::string>& Names() const { return m_Names; }
    bool Exists(const std::string& name) const;

    /// Folder a script is assigned to; "" for root (including unknown scripts).
    const std::string& AssignmentFor(const std::string& relativePath) const;
    const std::map<std::string, std::string>& Assignments() const { return m_Assignments; }

    /// Create @p name. Ignored when it already exists or is empty. Names stay
    /// sorted so the list renders in a stable order.
    void Create(const std::string& name);

    /// Rename, carrying every assignment across. Ignored if @p oldName does not
    /// exist or @p newName is empty or already taken.
    void Rename(const std::string& oldName, const std::string& newName);

    /// Delete @p name and return its scripts to root. The scripts themselves
    /// are untouched - this is a view, not storage.
    void Remove(const std::string& name);

    /// Assign a script to a folder, or to root when @p folder is empty.
    void Assign(const std::string& relativePath, const std::string& folder);

    /// Drop assignments whose script no longer exists. Call after rescanning:
    /// otherwise a deleted script leaves a stale row that would silently claim
    /// a new file that later takes the same path.
    void PruneMissing(const std::function<bool(const std::string&)>& stillExists);

    // Expansion state - presentation only, not persisted.
    bool IsExpanded(const std::string& name) const;
    void SetExpanded(const std::string& name, bool expanded);

    /// True when something changed since the last Save.
    bool Dirty() const { return m_Dirty; }
    void ClearDirty() { m_Dirty = false; }

private:
    std::map<std::string, std::string> m_Assignments;  // script relative path -> folder ("" = root)
    std::vector<std::string> m_Names;                  // sorted
    std::set<std::string> m_Expanded;
    bool m_Dirty = false;
};

}  // namespace DekiEditor
