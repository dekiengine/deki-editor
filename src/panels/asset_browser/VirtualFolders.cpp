#include "VirtualFolders.h"

#include <deki/LogSystem.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace DekiEditor
{

namespace fs = std::filesystem;

namespace
{
const std::string kRoot;  // the empty string, returned by reference
constexpr const char* kFileName = "script_folders.json";
}  // namespace

void VirtualFolders::Load(const std::string& projectRoot)
{
    m_Assignments.clear();
    m_Names.clear();
    m_Dirty = false;

    if (projectRoot.empty())
    {
        return;
    }

    fs::path filePath = fs::path(projectRoot) / kFileName;
    if (!fs::exists(filePath))
    {
        return;
    }

    std::ifstream file(filePath);
    if (!file.is_open())
    {
        return;
    }

    try
    {
        nlohmann::json j;
        file >> j;

        if (j.contains("folders") && j["folders"].is_array())
        {
            for (const auto& name : j["folders"])
            {
                m_Names.push_back(name.get<std::string>());
            }
        }

        if (j.contains("assignments") && j["assignments"].is_object())
        {
            for (auto& [key, val] : j["assignments"].items())
            {
                m_Assignments[key] = val.get<std::string>();
            }
        }
    }
    catch (const std::exception& e)
    {
        // These folders are a view over the scripts, not the scripts
        // themselves, so a corrupt file costs the user their grouping and
        // nothing more. Say so rather than silently presenting an empty set as
        // though they had never made any.
        DEKI_LOG_WARNING("VirtualFolders: could not parse %s (%s) - script grouping reset", filePath.string().c_str(),
                         e.what());
        m_Assignments.clear();
        m_Names.clear();
    }

    std::sort(m_Names.begin(), m_Names.end());
}

void VirtualFolders::Save(const std::string& projectRoot) const
{
    if (projectRoot.empty())
    {
        return;
    }

    nlohmann::json j;
    j["folders"] = m_Names;

    nlohmann::json assignments = nlohmann::json::object();
    for (const auto& [key, val] : m_Assignments)
    {
        if (!val.empty())  // root is the default; no need to store it
        {
            assignments[key] = val;
        }
    }
    j["assignments"] = assignments;

    fs::path filePath = fs::path(projectRoot) / kFileName;
    std::ofstream file(filePath);
    if (file.is_open())
    {
        file << j.dump(2);
    }
}

bool VirtualFolders::Exists(const std::string& name) const
{
    return std::find(m_Names.begin(), m_Names.end(), name) != m_Names.end();
}

const std::string& VirtualFolders::AssignmentFor(const std::string& relativePath) const
{
    auto it = m_Assignments.find(relativePath);
    return it == m_Assignments.end() ? kRoot : it->second;
}

void VirtualFolders::Create(const std::string& name)
{
    if (name.empty() || Exists(name))
    {
        return;
    }
    m_Names.push_back(name);
    std::sort(m_Names.begin(), m_Names.end());
    m_Dirty = true;
}

void VirtualFolders::Rename(const std::string& oldName, const std::string& newName)
{
    if (oldName == newName || newName.empty() || !Exists(oldName) || Exists(newName))
    {
        return;
    }

    auto it = std::find(m_Names.begin(), m_Names.end(), oldName);
    *it = newName;
    std::sort(m_Names.begin(), m_Names.end());

    // Carry the scripts across. Missing this is how a rename used to look like
    // a delete: the folder reappeared under the new name, empty.
    for (auto& [script, folder] : m_Assignments)
    {
        if (folder == oldName)
        {
            folder = newName;
        }
    }

    if (m_Expanded.erase(oldName))
    {
        m_Expanded.insert(newName);
    }

    m_Dirty = true;
}

void VirtualFolders::Remove(const std::string& name)
{
    auto it = std::find(m_Names.begin(), m_Names.end(), name);
    if (it == m_Names.end())
    {
        return;
    }
    m_Names.erase(it);

    // Back to root, not deleted. The scripts are real files.
    for (auto& [script, folder] : m_Assignments)
    {
        if (folder == name)
        {
            folder.clear();
        }
    }

    m_Expanded.erase(name);
    m_Dirty = true;
}

void VirtualFolders::Assign(const std::string& relativePath, const std::string& folder)
{
    if (relativePath.empty())
    {
        return;
    }
    m_Assignments[relativePath] = folder;
    m_Dirty = true;
}

void VirtualFolders::PruneMissing(const std::function<bool(const std::string&)>& stillExists)
{
    for (auto it = m_Assignments.begin(); it != m_Assignments.end();)
    {
        if (stillExists(it->first))
        {
            ++it;
        }
        else
        {
            it = m_Assignments.erase(it);
        }
    }
}

bool VirtualFolders::IsExpanded(const std::string& name) const
{
    return m_Expanded.count(name) != 0;
}

void VirtualFolders::SetExpanded(const std::string& name, bool expanded)
{
    if (expanded)
    {
        m_Expanded.insert(name);
    }
    else
    {
        m_Expanded.erase(name);
    }
}

}  // namespace DekiEditor
