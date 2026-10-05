#include "EditorRecovery.h"

#include <deki-editor/EditorPaths.h>
#include <deki-editor/FileIO.h>
#include <deki/LogSystem.h>
#include <deki/Scene.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <functional>
#include <vector>
#include <system_error>

namespace DekiEditor
{

namespace fs = std::filesystem;

std::string EditorRecovery::RecoveryDirFor(const std::string& projectPath)
{
    const std::string dataDir = EditorPaths::GetEditorDataDir();
    if (dataDir.empty() || projectPath.empty())
    {
        return "";
    }

    // Normalised before hashing so the same project always lands in the same
    // folder however it was typed: "C:\Foo" and "c:/foo/" are one project.
    std::string norm = projectPath;
    for (char& c : norm)
    {
        if (c == '\\')
        {
            c = '/';
        }
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!norm.empty() && norm.back() == '/')
    {
        norm.pop_back();
    }

    std::string name = fs::path(norm).filename().string();
    for (char& c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
        {
            c = '_';
        }
    }
    if (name.empty())
    {
        name = "project";
    }

    // FNV-1a, not std::hash: std::hash may change with the compiler or its
    // library, and then an updated editor could not find a recovery the old
    // one left.
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : norm)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    char hash[17];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(h));

    return dataDir + "/Recovery/" + name + "-" + hash;
}

void EditorRecovery::RemoveRecoveryDir(const std::string& projectPath)
{
    std::error_code ec;
    const std::string dir = RecoveryDirFor(projectPath);
    if (!dir.empty())
    {
        fs::remove_all(fs::path(dir), ec);
    }
}

void EditorRecovery::ScanProject(const std::string& projectPath)
{
    m_Files.clear();
    m_Selected = 0;

    const std::string recoveryDir = RecoveryDirFor(projectPath);
    std::error_code dirEc;
    if (recoveryDir.empty() || !fs::is_directory(recoveryDir, dirEc))
    {
        return;
    }

    for (const auto& entry : fs::directory_iterator(recoveryDir, dirEc))
    {
        if (entry.path().extension() != ".scene")
        {
            continue;  // skips .meta.json sidecars and any stray files
        }

        RecoveryEntry rec;
        rec.recoveryPath = entry.path().string();
        rec.displayName = entry.path().filename().string();

        // The sidecar holds the original scene path; the snapshot's file
        // name alone cannot say where the user's file lives.
        fs::path metaPath = entry.path();
        metaPath += ".meta.json";
        if (fs::exists(metaPath))
        {
            try
            {
                std::ifstream f(metaPath);
                if (f.is_open())
                {
                    nlohmann::json meta;
                    f >> meta;
                    rec.originalPath = meta.value("originalPath", std::string{});
                    rec.savedAt = static_cast<std::time_t>(meta.value("savedAt", 0LL));
                }
            }
            catch (const std::exception& e)
            {
                // The entry is still offered, but Save will ask for a
                // location.
                DEKI_LOG_WARNING("Recovery: sidecar '%s' unreadable (%s); original path unknown",
                                 metaPath.string().c_str(), e.what());
            }
        }

        // No savedAt (unreadable or older sidecar): use the file's mtime.
        // Nothing rewrites these files, so it is the crash time.
        if (rec.savedAt == 0)
        {
            std::error_code ec;
            const auto ftime = fs::last_write_time(entry.path(), ec);
            if (!ec)
            {
                const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                rec.savedAt = std::chrono::system_clock::to_time_t(sys);
            }
        }

        std::error_code sizeEc;
        rec.fileSize = fs::file_size(entry.path(), sizeEc);
        if (sizeEc)
        {
            rec.fileSize = 0;
        }

        // The object count tells the user whether this is worth recovering.
        // Counted here, not in the crash handler, which must only get the
        // file written.
        try
        {
            std::ifstream pf(entry.path());
            if (pf.is_open())
            {
                nlohmann::json doc;
                pf >> doc;
                rec.objectCount = CountSceneObjects(doc);
            }
        }
        catch (const std::exception&)
        {
            rec.objectCount = -1;  // shown as "unreadable" - still offered, the loader may cope
        }

        // Shown as its place in the project ("assets/level1/main.scene"): the
        // snapshot's own file name is that path flattened.
        if (!rec.originalPath.empty())
        {
            const fs::path rel = fs::path(rec.originalPath)
                                     .lexically_normal()
                                     .lexically_relative(fs::path(projectPath).lexically_normal());
            if (!rel.empty() && *rel.begin() != "..")
            {
                rec.displayName = rel.generic_string();
            }
        }
        m_Files.push_back(std::move(rec));
    }

    // Newest first: after several crashes the most recent is almost always the
    // one wanted, and it becomes the default selection.
    std::sort(m_Files.begin(), m_Files.end(),
              [](const RecoveryEntry& a, const RecoveryEntry& b) { return a.savedAt > b.savedAt; });

    if (!m_Files.empty())
    {
        m_ProjectPath = projectPath;
        m_DialogRequested = true;
        // Logged because the files live outside the project, so users may
        // need to find them.
        DEKI_LOG_EDITOR("Recovery: %zu scene(s) pending in %s", m_Files.size(), recoveryDir.c_str());
    }
}

void EditorRecovery::SetSelectedIndex(int index)
{
    if (m_Files.empty())
    {
        m_Selected = 0;
        return;
    }
    m_Selected = std::clamp(index, 0, static_cast<int>(m_Files.size()) - 1);
}

const RecoveryEntry* EditorRecovery::Selected() const
{
    if (m_Selected < 0 || m_Selected >= static_cast<int>(m_Files.size()))
    {
        return nullptr;
    }
    return &m_Files[static_cast<size_t>(m_Selected)];
}

bool EditorRecovery::WriteSnapshot(Deki::Scene* scene, const std::string& scenePath, const std::string& projectPath,
                                   std::string* outWrittenPath)
{
    if (!scene || projectPath.empty())
    {
        return false;
    }

    try
    {
        const std::string dir = RecoveryDirFor(projectPath);
        if (dir.empty())
        {
            return false;  // no home directory to write to; nothing useful to do here
        }
        fs::path recoveryDir = fs::path(dir);
        fs::create_directories(recoveryDir);

        // Named by the scene's path inside the project ("scenes~main.scene"),
        // not its file name alone, so two scenes called main.scene do not
        // overwrite each other's snapshot.
        std::string filename = "recovered.scene";
        if (!scenePath.empty())
        {
            fs::path rel =
                fs::path(scenePath).lexically_normal().lexically_relative(fs::path(projectPath).lexically_normal());
            std::string flat = (rel.empty() || *rel.begin() == "..") ? fs::path(scenePath).filename().generic_string()
                                                                     : rel.generic_string();
            for (char& c : flat)
            {
                if (c == '/' || c == ':')
                {
                    c = '~';
                }
            }
            filename = flat;
        }

        fs::path recoveryPath = recoveryDir / filename;
        if (!scene->SaveToFile(recoveryPath.string()))
        {
            return false;
        }

        // The sidecar records the original absolute path, so a restored scene
        // saves to the user's asset, not to the recovery copy.
        nlohmann::json meta;
        meta["originalPath"] = scenePath;
        // When the snapshot was taken, so the dialog can say how old it is.
        // Only a time() call: this also runs inside the crash handler, where
        // anything expensive risks losing the snapshot. Size and object count
        // are worked out when the entry is read.
        meta["savedAt"] = static_cast<long long>(std::time(nullptr));
        fs::path metaPath = recoveryDir / (filename + ".meta.json");
        std::string err;
        AtomicWriteFile(metaPath.string(), meta.dump(2), err);

        if (std::find(m_SessionSnapshots.begin(), m_SessionSnapshots.end(), recoveryPath.string()) ==
            m_SessionSnapshots.end())
        {
            m_SessionSnapshots.push_back(recoveryPath.string());
        }

        if (outWrittenPath)
        {
            *outWrittenPath = recoveryPath.string();
        }
        return true;
    }
    catch (...)
    {
        // Best effort and silent: this runs inside the crash handler, where
        // logging may be unsafe. A failure must not stop the crash dialog or
        // interrupt an ordinary editing frame.
        return false;
    }
}

bool EditorRecovery::TickAutosaveTimer(float deltaSeconds, bool ready)
{
    if (!ready)
    {
        m_AutosaveTimer = 0.0f;
        return false;
    }

    m_AutosaveTimer += deltaSeconds;
    if (m_AutosaveTimer < kAutosaveIntervalSeconds)
    {
        return false;
    }

    m_AutosaveTimer = 0.0f;
    return true;
}

void EditorRecovery::BuildPreview(const RecoveryEntry& entry)
{
    m_Preview.clear();
    m_PreviewName = entry.displayName;

    try
    {
        std::ifstream f(entry.recoveryPath);
        if (!f.is_open())
        {
            return;
        }
        nlohmann::json doc;
        f >> doc;
        m_Preview = BuildSceneOutline(doc);
    }
    catch (const std::exception&)
    {
        // Unreadable: the popup says so, rather than showing an empty tree
        // that looks like an empty scene.
        m_Preview.clear();
    }
}

void EditorRecovery::ClearPreview()
{
    m_Preview.clear();
    m_PreviewName.clear();
}

bool EditorRecovery::ConsumeEntry(int index)
{
    if (index < 0 || index >= static_cast<int>(m_Files.size()))
    {
        return false;
    }

    const RecoveryEntry rec = m_Files[static_cast<size_t>(index)];  // copy: the vector is edited below

    std::error_code ec;
    fs::remove(rec.recoveryPath, ec);
    fs::remove(rec.recoveryPath + ".meta.json", ec);

    m_Files.erase(m_Files.begin() + index);
    m_Selected = 0;

    // Anything left is still unresolved, so keep offering it.
    if (m_Files.empty())
    {
        RemoveRecoveryDir(m_ProjectPath);
        m_DialogRequested = false;
    }
    return true;
}

void EditorRecovery::DiscardAll()
{
    std::error_code ec;
    for (const RecoveryEntry& rec : m_Files)
    {
        fs::remove(rec.recoveryPath, ec);
        fs::remove(rec.recoveryPath + ".meta.json", ec);
    }
    m_Files.clear();
    m_Selected = 0;
    RemoveRecoveryDir(m_ProjectPath);
    m_DialogRequested = false;
}

void EditorRecovery::DiscardSessionSnapshots()
{
    std::error_code ec;
    for (const std::string& path : m_SessionSnapshots)
    {
        const fs::path written = fs::path(path).lexically_normal();
        const bool pending = std::any_of(m_Files.begin(), m_Files.end(), [&](const RecoveryEntry& e)
                                         { return fs::path(e.recoveryPath).lexically_normal() == written; });
        if (pending)
        {
            continue;
        }

        fs::remove(written, ec);
        fs::remove(fs::path(path + ".meta.json"), ec);

        // The directory goes with its last file, as it does after DiscardAll.
        const fs::path dir = written.parent_path();
        if (fs::is_empty(dir, ec) && !ec)
        {
            fs::remove(dir, ec);
        }
    }
    m_SessionSnapshots.clear();
}

}  // namespace DekiEditor
