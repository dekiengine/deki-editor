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
/// Scripts stay where they are on disk; these folders only group them in the
/// view, and are saved to `script_folders.json` at the project root. A
/// script's folder is a plain string, and the empty string means root, so
/// deleting a folder moves its scripts to root rather than losing them.
///
/// Uses no ImGui, so it can be tested on its own.
class VirtualFolders
{
public:
    /// Loads `<projectRoot>/script_folders.json`. A missing or broken file
    /// leaves the set empty, so the panel still lists the scripts.
    void Load(const std::string& projectRoot);

    /// Writes the file if anything changed. Does nothing when the project root
    /// is empty.
    void Save(const std::string& projectRoot) const;

    const std::vector<std::string>& Names() const { return m_Names; }
    bool Exists(const std::string& name) const;

    /// The folder a script is in; "" for root and for unknown scripts.
    const std::string& AssignmentFor(const std::string& relativePath) const;
    const std::map<std::string, std::string>& Assignments() const { return m_Assignments; }

    /// Creates folder `name`, unless it exists or is empty. Names stay sorted
    /// so the list keeps a stable order.
    void Create(const std::string& name);

    /// Renames a folder and moves its scripts along. Does nothing if `oldName`
    /// does not exist or `newName` is empty or taken.
    void Rename(const std::string& oldName, const std::string& newName);

    /// Deletes folder `name` and moves its scripts to root. The script files
    /// stay as they are.
    void Remove(const std::string& name);

    /// Puts a script in a folder, or in root when `folder` is empty.
    void Assign(const std::string& relativePath, const std::string& folder);

    /// Drops entries whose script no longer exists. Call it after a rescan, or
    /// a deleted script's entry would claim a new file that later takes the
    /// same path.
    void PruneMissing(const std::function<bool(const std::string&)>& stillExists);

    // Which folders are open in the view. Not saved.
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
