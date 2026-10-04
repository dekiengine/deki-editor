#include "BoardPackManager.h"
#include <deki-editor/SafeNames.h>
#include <deki-editor/EditorPaths.h>
#include <deki-editor/EditorHttpUtils.h>
#include <deki-editor/build/PlatformConfig.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace DekiEditor
{

BoardPackManager::BoardPackManager()
{
    // No default sources - user adds sources via Boards Manager
}

BoardPackManager::~BoardPackManager()
{
    if (m_FetchThread.joinable())
    {
        m_FetchThread.join();
    }
    if (m_InstallThread.joinable())
    {
        m_InstallThread.join();
    }
}

// ============================================================================
// Directory helpers
// ============================================================================

std::string BoardPackManager::GetEditorDataDir() const
{
    return EditorPaths::GetEditorDataDir();
}

std::string BoardPackManager::GetBoardPacksDir() const
{
    return EditorPaths::GetBoardPacksDir();
}

// ============================================================================
// Sources
// ============================================================================

void BoardPackManager::AddSource(const std::string& name, const std::string& url)
{
    // Check for duplicate
    for (const auto& src : m_Sources)
    {
        if (src.url == url)
        {
            return;
        }
    }
    m_Sources.push_back({ name, url });
    SaveRegistry();
}

void BoardPackManager::RemoveSource(const std::string& name)
{
    m_Sources.erase(std::remove_if(m_Sources.begin(), m_Sources.end(),
                                   [&name](const BoardPackSource& s) { return s.name == name; }),
                    m_Sources.end());
    SaveRegistry();
}

// ============================================================================
// Discovery
// ============================================================================

void BoardPackManager::FetchAvailablePacks(BoardPackFetchCallback callback)
{
    if (m_IsFetching)
    {
        return;
    }

    if (m_FetchThread.joinable())
    {
        m_FetchThread.join();
    }

    m_IsFetching = true;

    m_FetchThread = std::thread(
        [this, callback]()
        {
            std::vector<BoardPackInfo> remotePacks;
            std::string lastError;

            for (const auto& source : m_Sources)
            {
                std::string content = EditorHttpUtils::FetchUrl(source.url);
                if (content.empty())
                {
                    lastError = "Failed to fetch from " + source.name;
                    continue;
                }

                std::vector<BoardPackInfo> packs;
                if (ParseRemoteIndex(content, packs))
                {
                    remotePacks.insert(remotePacks.end(), packs.begin(), packs.end());
                }
            }

            // Merge with installed packs
            {
                std::lock_guard<std::mutex> lock(m_PacksMutex);

                // Keep installed pack state
                std::map<std::string, BoardPackInfo> installedMap;
                for (const auto& pack : m_Packs)
                {
                    if (pack.isInstalled)
                    {
                        installedMap[pack.id] = pack;
                    }
                }

                m_Packs.clear();

                // Add remote packs, merging with installed state
                for (auto& pack : remotePacks)
                {
                    auto it = installedMap.find(pack.id);
                    if (it != installedMap.end())
                    {
                        pack.isInstalled = true;
                        pack.installedVersion = it->second.installedVersion;
                        pack.installPath = it->second.installPath;
                        installedMap.erase(it);
                    }
                    m_Packs.push_back(pack);
                }

                // Add remaining installed-only packs (not in remote index)
                for (auto& [id, pack] : installedMap)
                {
                    m_Packs.push_back(pack);
                }
            }

            m_IsFetching = false;

            if (callback)
            {
                callback(!remotePacks.empty(), lastError);
            }
        });
}

std::vector<BoardPackInfo> BoardPackManager::GetAllPacks() const
{
    std::lock_guard<std::mutex> lock(m_PacksMutex);
    return m_Packs;
}

std::vector<BoardPackInfo> BoardPackManager::GetInstalledPacks() const
{
    std::lock_guard<std::mutex> lock(m_PacksMutex);
    std::vector<BoardPackInfo> result;
    for (const auto& pack : m_Packs)
    {
        if (pack.isInstalled)
        {
            result.push_back(pack);
        }
    }
    return result;
}

// ============================================================================
// Installation
// ============================================================================

void BoardPackManager::InstallPack(const std::string& packId, BoardPackInstallCallback callback)
{
    if (m_IsInstalling)
    {
        return;
    }

    if (m_InstallThread.joinable())
    {
        m_InstallThread.join();
    }

    m_IsInstalling = true;

    // Find pack info
    BoardPackInfo packInfo;
    {
        std::lock_guard<std::mutex> lock(m_PacksMutex);
        for (const auto& pack : m_Packs)
        {
            if (pack.id == packId)
            {
                packInfo = pack;
                break;
            }
        }
    }

    if (packInfo.downloadUrl.empty())
    {
        m_IsInstalling = false;
        if (callback)
        {
            callback(false, "Pack not found: " + packId);
        }
        return;
    }

    m_InstallThread = std::thread(
        [this, packInfo, callback]()
        {
            {
                std::lock_guard<std::mutex> lock(m_StatusMutex);
                m_InstallStatus = "Downloading " + packInfo.name + "...";
            }

            bool success = DoInstallPack(packInfo.downloadUrl, packInfo.id);

            if (success)
            {
                // Update pack state
                std::lock_guard<std::mutex> lock(m_PacksMutex);
                for (auto& pack : m_Packs)
                {
                    if (pack.id == packInfo.id)
                    {
                        pack.isInstalled = true;
                        pack.installedVersion = pack.version;
                        pack.installPath = GetBoardPacksDir() + "/" + pack.id;
                        break;
                    }
                }
            }

            {
                std::lock_guard<std::mutex> lock(m_StatusMutex);
                m_InstallStatus = success ? "Installed " + packInfo.name : "Failed to install " + packInfo.name;
            }

            m_IsInstalling = false;

            if (callback)
            {
                callback(success, success ? "" : "Download or extraction failed");
            }
        });
}

bool BoardPackManager::UninstallPack(const std::string& packId)
{
    std::string reason;
    if (!SafeNames::IsSafeName(packId, reason))
    {
        return false;
    }
    std::string packDir = GetBoardPacksDir() + "/" + packId;

    try
    {
        if (fs::exists(packDir))
        {
            fs::remove_all(packDir);
        }
    }
    catch (const std::exception&)
    {
        return false;
    }

    // Update state
    {
        std::lock_guard<std::mutex> lock(m_PacksMutex);
        for (auto& pack : m_Packs)
        {
            if (pack.id == packId)
            {
                pack.isInstalled = false;
                pack.installedVersion.clear();
                pack.installPath.clear();
                break;
            }
        }
    }

    return true;
}

std::string BoardPackManager::GetInstallStatus() const
{
    std::lock_guard<std::mutex> lock(m_StatusMutex);
    return m_InstallStatus;
}

// ============================================================================
// Import to project
// ============================================================================

bool BoardPackManager::ImportPlatformToProject(const std::string& packId, const std::string& platformId,
                                               const std::string& projectPlatformsDir)
{
    std::string reason;
    if (!SafeNames::IsSafeName(packId, reason) || !SafeNames::IsSafeName(platformId, reason))
    {
        return false;
    }
    std::string packDir = GetBoardPacksDir() + "/" + packId;
    std::string srcFile = packDir + "/" + platformId + ".json";

    if (!fs::exists(srcFile))
    {
        return false;
    }

    try
    {
        fs::create_directories(projectPlatformsDir);
        fs::copy_file(srcFile, projectPlatformsDir + "/" + platformId + ".json", fs::copy_options::overwrite_existing);

        // Also register in PlatformRegistry
        PlatformRegistry::Instance().LoadPlatformsFromDirectory(projectPlatformsDir);
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool BoardPackManager::ImportAllPlatformsToProject(const std::string& packId, const std::string& projectPlatformsDir)
{
    std::string reason;
    if (!SafeNames::IsSafeName(packId, reason))
    {
        return false;
    }
    std::string packDir = GetBoardPacksDir() + "/" + packId;

    if (!fs::exists(packDir))
    {
        return false;
    }

    bool anyImported = false;

    try
    {
        fs::create_directories(projectPlatformsDir);

        for (const auto& entry : fs::directory_iterator(packDir))
        {
            if (entry.path().extension() == ".json" && entry.path().filename() != "pack.json")
            {
                fs::copy_file(entry.path(), fs::path(projectPlatformsDir) / entry.path().filename(),
                              fs::copy_options::overwrite_existing);
                anyImported = true;
            }
        }

        if (anyImported)
        {
            PlatformRegistry::Instance().LoadPlatformsFromDirectory(projectPlatformsDir);
        }
    }
    catch (const std::exception&)
    {
        return false;
    }

    return anyImported;
}

// ============================================================================
// Persistence
// ============================================================================

void BoardPackManager::LoadRegistry()
{
    std::string editorDir = GetEditorDataDir();
    if (editorDir.empty())
    {
        return;
    }

    std::string registryPath = editorDir + "/board-pack-sources.json";

    if (!fs::exists(registryPath))
    {
        return;
    }

    try
    {
        std::ifstream file(registryPath);
        json j = json::parse(file);

        if (j.contains("sources") && j["sources"].is_array())
        {
            m_Sources.clear();
            for (const auto& src : j["sources"])
            {
                BoardPackSource source;
                source.name = src.value("name", "");
                source.url = src.value("url", "");
                if (!source.url.empty())
                {
                    m_Sources.push_back(source);
                }
            }
        }
    }
    catch (const std::exception&)
    {
        // Keep default sources
    }

    ScanInstalledPacks();
}

void BoardPackManager::SaveRegistry()
{
    std::string editorDir = GetEditorDataDir();
    if (editorDir.empty())
    {
        return;
    }

    try
    {
        fs::create_directories(editorDir);

        json j;
        json sourcesArr = json::array();
        for (const auto& src : m_Sources)
        {
            sourcesArr.push_back({ { "name", src.name }, { "url", src.url } });
        }
        j["sources"] = sourcesArr;

        std::ofstream file(editorDir + "/board-pack-sources.json");
        file << j.dump(4);
    }
    catch (const std::exception&)
    {
        // Silently fail
    }
}

void BoardPackManager::ScanInstalledPacks()
{
    std::string boardPacksDir = GetBoardPacksDir();
    if (boardPacksDir.empty() || !fs::exists(boardPacksDir))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_PacksMutex);

    for (const auto& entry : fs::directory_iterator(boardPacksDir))
    {
        if (!entry.is_directory())
        {
            continue;
        }

        BoardPackInfo pack;
        if (ParsePackJson(entry.path().string(), pack))
        {
            pack.isInstalled = true;
            pack.installPath = entry.path().string();

            // Merge: update existing or add new
            bool found = false;
            for (auto& existing : m_Packs)
            {
                if (existing.id == pack.id)
                {
                    existing.isInstalled = true;
                    existing.installedVersion = pack.version;
                    existing.installPath = pack.installPath;
                    existing.platforms = pack.platforms;
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                m_Packs.push_back(pack);
            }
        }
    }
}

// ============================================================================
// Parsing
// ============================================================================

bool BoardPackManager::ParseRemoteIndex(const std::string& jsonStr, std::vector<BoardPackInfo>& outPacks)
{
    try
    {
        json j = json::parse(jsonStr);

        if (!j.contains("packs") || !j["packs"].is_array())
        {
            return false;
        }

        for (const auto& packJson : j["packs"])
        {
            BoardPackInfo pack;
            pack.id = packJson.value("id", "");
            {
                // The id becomes a directory under the board-packs folder and
                // the archive name beside it; a remote index does not get to
                // pick a path (SafeNames.h).
                std::string reason;
                if (!SafeNames::IsSafeName(pack.id, reason))
                {
                    continue;
                }
            }
            pack.name = packJson.value("name", "");
            pack.version = packJson.value("version", "");
            pack.author = packJson.value("author", "");
            pack.description = packJson.value("description", "");
            pack.downloadUrl = packJson.value("url", "");

            if (packJson.contains("platforms") && packJson["platforms"].is_array())
            {
                for (const auto& platJson : packJson["platforms"])
                {
                    BoardPackPlatform plat;
                    plat.id = platJson.value("id", "");
                    plat.displayName = platJson.value("displayName", "");
                    plat.family = platJson.value("family", "");
                    pack.platforms.push_back(plat);
                }
            }

            if (!pack.id.empty())
            {
                outPacks.push_back(pack);
            }
        }

        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool BoardPackManager::ParsePackJson(const std::string& packDir, BoardPackInfo& outPack)
{
    std::string packJsonPath = packDir + "/pack.json";

    if (!fs::exists(packJsonPath))
    {
        // No pack.json - try to infer from directory contents
        // Scan for platform JSON files
        outPack.id = fs::path(packDir).filename().string();
        outPack.name = outPack.id;
        outPack.version = "local";

        for (const auto& entry : fs::directory_iterator(packDir))
        {
            if (entry.path().extension() == ".json" && entry.path().filename() != "pack.json")
            {
                BoardPackPlatform plat;
                plat.id = entry.path().stem().string();
                plat.displayName = plat.id;
                outPack.platforms.push_back(plat);
            }
        }

        return !outPack.platforms.empty();
    }

    try
    {
        std::ifstream file(packJsonPath);
        json j = json::parse(file);

        outPack.id = j.value("id", fs::path(packDir).filename().string());
        outPack.name = j.value("name", outPack.id);
        outPack.version = j.value("version", "");
        outPack.author = j.value("author", "");
        outPack.description = j.value("description", "");
        outPack.installedVersion = outPack.version;

        if (j.contains("platforms") && j["platforms"].is_array())
        {
            for (const auto& platJson : j["platforms"])
            {
                BoardPackPlatform plat;
                plat.id = platJson.value("id", "");
                plat.displayName = platJson.value("displayName", "");
                plat.family = platJson.value("family", "");
                outPack.platforms.push_back(plat);
            }
        }
        else
        {
            // Scan directory for platform JSON files
            for (const auto& entry : fs::directory_iterator(packDir))
            {
                if (entry.path().extension() == ".json" && entry.path().filename() != "pack.json")
                {
                    BoardPackPlatform plat;
                    plat.id = entry.path().stem().string();
                    plat.displayName = plat.id;
                    outPack.platforms.push_back(plat);
                }
            }
        }

        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

// ============================================================================
// Network / Download
// ============================================================================

bool BoardPackManager::DoInstallPack(const std::string& url, const std::string& packId)
{
    std::string boardPacksDir = GetBoardPacksDir();
    if (boardPacksDir.empty())
    {
        return false;
    }

    try
    {
        fs::create_directories(boardPacksDir);
    }
    catch (const std::exception&)
    {
        return false;
    }

    {
        std::string reason;
        if (!SafeNames::IsSafeName(packId, reason))
        {
            return false;
        }
    }
    std::string zipPath = boardPacksDir + "/" + packId + ".zip";
    std::string packDir = boardPacksDir + "/" + packId;

    // Download
    {
        std::lock_guard<std::mutex> lock(m_StatusMutex);
        m_InstallStatus = "Downloading...";
    }

    std::string content = EditorHttpUtils::FetchUrl(url);
    if (content.empty())
    {
        return false;
    }

    // Write to file
    {
        std::ofstream file(zipPath, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }
        file.write(content.data(), content.size());
    }

    // Extract
    {
        std::lock_guard<std::mutex> lock(m_StatusMutex);
        m_InstallStatus = "Extracting...";
    }

    bool extracted = EditorHttpUtils::ExtractZip(zipPath, packDir);

    // Clean up zip
    try
    {
        fs::remove(zipPath);
    }
    catch (...)
    { /* the download archive is a temp file; a failed cleanup costs disk, not correctness */
    }

    return extracted;
}

}  // namespace DekiEditor
