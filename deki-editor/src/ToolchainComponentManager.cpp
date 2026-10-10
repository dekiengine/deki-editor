#include <deki-editor/build/ToolchainComponentManager.h>
#include <deki-editor/ShellCommand.h>
#include <deki-editor/EditorPaths.h>
#include <deki-editor/EditorHttpUtils.h>
#include <deki-editor/EditorSettings.h>
#include <deki-editor/build/BuildActivitySink.h>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#endif

namespace fs = std::filesystem;

namespace DekiEditor
{

namespace
{
// Installed-version tracking. The version a component was installed at is kept
// in a small marker file under <toolchains>/.versions/<id>, so update checks can
// compare it with the latest release.
std::string Trim(std::string s)
{
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

// Normalize for comparison: trim + drop a leading 'v' (e.g. "v5.1" == "5.1").
std::string NormalizeVersion(std::string v)
{
    v = Trim(std::move(v));
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V'))
    {
        v.erase(v.begin());
    }
    return v;
}

std::string VersionMarkerPath(const std::string& toolchainsDir, const std::string& compId)
{
    return toolchainsDir + "/.versions/" + compId;
}

std::string ReadInstalledVersion(const std::string& toolchainsDir, const std::string& compId)
{
    std::ifstream f(VersionMarkerPath(toolchainsDir, compId));
    if (!f.is_open())
    {
        return {};
    }
    std::string v;
    std::getline(f, v);
    return Trim(v);
}

void WriteInstalledVersion(const std::string& toolchainsDir, const std::string& compId, const std::string& version)
{
    try
    {
        fs::create_directories(toolchainsDir + "/.versions");
    }
    catch (...)
    { /* best effort: the operation that needs this path fails visibly on its own if it did not happen */
    }
    std::ofstream f(VersionMarkerPath(toolchainsDir, compId), std::ios::trunc);
    if (f.is_open())
    {
        f << version;
    }
}
}  // namespace

ToolchainComponentManager::ToolchainComponentManager() = default;

ToolchainComponentManager::~ToolchainComponentManager()
{
    if (m_WorkerThread.joinable())
    {
        m_WorkerThread.join();
    }
}

void ToolchainComponentManager::Initialize(const BuilderDefinition& def)
{
    m_Definition = def;
    m_ToolchainsDir = GetToolchainsDir();
}

std::string ToolchainComponentManager::GetToolchainsDir() const
{
    return EditorPaths::GetToolchainsDir();
}

// ============================================================================
// Detection
// ============================================================================

std::string ToolchainComponentManager::ResolvePath(const std::string& tmpl, const ComponentDef& comp) const
{
    std::string result = tmpl;

    std::string placeholder = "{installPath}";
    size_t pos = result.find(placeholder);
    if (pos != std::string::npos)
    {
        result.replace(pos, placeholder.length(), comp.installPath);
    }

    return result;
}

bool ToolchainComponentManager::MatchesGlob(const std::string& resolvedPath) const
{
    size_t starPos = resolvedPath.find('*');
    if (starPos == std::string::npos)
    {
        // No wildcard: check the path itself.
        std::string fullPath = m_ToolchainsDir + "/" + resolvedPath;
        return fs::exists(fullPath);
    }

    // Split at the wildcard: everything before * is the parent dir, everything after is the suffix
    std::string prefix = resolvedPath.substr(0, starPos);
    std::string suffix = resolvedPath.substr(starPos + 1);

    std::string parentDir = m_ToolchainsDir + "/" + prefix;

    if (!parentDir.empty() && (parentDir.back() == '/' || parentDir.back() == '\\'))
    {
        parentDir.pop_back();
    }

    if (!fs::exists(parentDir) || !fs::is_directory(parentDir))
    {
        return false;
    }

    // Scan parent directory for any entry that matches when combined with suffix
    try
    {
        for (const auto& entry : fs::directory_iterator(parentDir))
        {
            std::string candidate = entry.path().string() + suffix;
            std::replace(candidate.begin(), candidate.end(), '\\', '/');
            if (fs::exists(candidate))
            {
                return true;
            }
        }
    }
    catch (const std::exception&)
    {
    }

    return false;
}

bool ToolchainComponentManager::EvaluateDetection(const ComponentDef& comp) const
{
    if (m_ToolchainsDir.empty())
    {
        return false;
    }

    for (const auto& [platform, pathPattern] : comp.detection)
    {
        // Filter by current platform
#ifdef _WIN32
        if (platform != "windows" && platform != "any")
        {
            continue;
        }
#else
        if (platform != "unix" && platform != "any")
        {
            continue;
        }
#endif

        std::string resolved = ResolvePath(pathPattern, comp);

        if (MatchesGlob(resolved))
        {
            return true;
        }
    }

    return false;
}

std::vector<std::string> ToolchainComponentManager::InstalledPathEntries() const
{
    std::vector<std::string> entries;
    if (m_ToolchainsDir.empty())
    {
        return entries;
    }
#ifdef _WIN32
    const char* platform = "windows";
#else
    const char* platform = "unix";
#endif
    for (const ComponentDef& comp : m_Definition.components)
    {
        if (comp.pathEntries.empty() || !EvaluateDetection(comp))
        {
            continue;
        }
        for (const char* key : { platform, "any" })
        {
            auto it = comp.pathEntries.find(key);
            if (it == comp.pathEntries.end())
            {
                continue;
            }
            for (const std::string& entry : it->second)
            {
                entries.push_back(m_ToolchainsDir + "/" + ResolvePath(entry, comp));
            }
        }
    }
    return entries;
}

// ============================================================================
// Component Status
// ============================================================================

std::vector<ToolchainComponent> ToolchainComponentManager::GetComponents() const
{
    std::vector<ToolchainComponent> result;

    for (const auto& compDef : m_Definition.components)
    {
        ToolchainComponent comp;
        comp.id = compDef.id;
        comp.displayName = compDef.displayName;
        comp.canInstall = compDef.canInstall;
        comp.canSetup = compDef.canSetup;
        comp.setupDone = compDef.canSetup ? EditorSettings::GetBool("setup." + compDef.id, false) : false;
        comp.tooltip = compDef.tooltip;

        bool installed = EvaluateDetection(compDef);
        comp.status = installed ? ToolchainComponentStatus::Installed : ToolchainComponentStatus::NotInstalled;

        // The pinned version is what an install puts down, so it is what the
        // backend was written against. An install of another version (left
        // behind when a package moved its pin) is out of date, and is shown so
        // without a network check. "Check for Updates" answers a different
        // question: whether a newer release exists.
        const std::string installedVer = installed ? ReadInstalledVersion(m_ToolchainsDir, compDef.id) : std::string();
        if (!installedVer.empty())
        {
            comp.installedVersion = installedVer;
        }
        if (!installedVer.empty() && !compDef.fallback.version.empty() &&
            NormalizeVersion(installedVer) != NormalizeVersion(compDef.fallback.version))
        {
            comp.status = ToolchainComponentStatus::UpdateAvailable;
        }

        // Fill version info from cache
        {
            std::lock_guard<std::mutex> lock(m_VersionMutex);
            auto it = m_VersionCache.find(compDef.id);
            if (it != m_VersionCache.end())
            {
                comp.installedVersion = it->second.installedVersion;
                comp.latestVersion = it->second.latestVersion;

                if (installed && it->second.updateAvailable)
                {
                    comp.status = ToolchainComponentStatus::UpdateAvailable;
                }
                if (!installedVer.empty())
                {
                    comp.installedVersion = installedVer;
                }
            }
            else
            {
                // Use fallback version as display
                comp.latestVersion = compDef.fallback.version;
            }
        }

        result.push_back(comp);
    }

    return result;
}

// ============================================================================
// Install
// ============================================================================

void ToolchainComponentManager::InstallComponent(const std::string& componentId, BuildProgressCallback cb)
{
    if (m_IsBusy)
    {
        return;
    }

    const ComponentDef* compDef = nullptr;
    for (const auto& c : m_Definition.components)
    {
        if (c.id == componentId)
        {
            compDef = &c;
            break;
        }
    }

    if (!compDef)
    {
        if (cb)
        {
            BuildProgress p;
            p.state = BuildState::Failed;
            p.error = "Unknown component: " + componentId;
            cb(p);
        }
        return;
    }

    if (m_WorkerThread.joinable())
    {
        m_WorkerThread.join();
    }

    m_IsBusy = true;

    ComponentDef def = *compDef;

    m_WorkerThread = std::thread(
        [this, def, cb]()
        {
            if (!def.subComponents.empty())
            {
                DoInstallSubComponents(def, cb);
            }
            else
            {
                DoInstallComponent(def, cb);
            }

            // Auto-run setup if the component has a postInstall script and installed successfully
            if (def.canSetup && EvaluateDetection(def))
            {
#ifdef _WIN32
                bool hasPostInstall = !def.postInstall.windows.empty();
#else
                bool hasPostInstall = !def.postInstall.unixLike.empty();
#endif
                if (hasPostInstall)
                    DoSetupComponent(def, nullptr);
            }

            m_IsBusy = false;
        });
}

// Downloads `url` to `zipPath`, or uses the archive already there when it
// matches the pinned `sha256`. An install keeps its archive until it succeeds
// (see the callers), so retrying a failed one does not fetch it again: ESP-IDF
// alone is close to 2 GB. Without a pinned hash nothing proves the file is the
// one wanted, so it is downloaded again.
static bool FetchArchive(const std::string& url, const std::string& zipPath, const std::string& sha256,
                         const HttpStatusCallback& status)
{
    std::error_code ec;
    if (!sha256.empty() && fs::exists(zipPath, ec))
    {
        status("Checking the archive downloaded before: " + zipPath);
        std::string have = EditorHttpUtils::Sha256File(zipPath);
        std::string want = sha256;
        for (std::string* s : { &have, &want })
        {
            std::transform(s->begin(), s->end(), s->begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        if (!have.empty() && have == want)
        {
            status("Using it: it matches the pinned SHA-256, so nothing is downloaded");
            return true;
        }
        status("It does not match the pinned SHA-256 (an interrupted download?); downloading again");
        fs::remove(zipPath, ec);
    }
    fs::create_directories(fs::path(zipPath).parent_path(), ec);
    return EditorHttpUtils::DownloadFileVerified(url, zipPath, sha256, nullptr, status);
}

// Gives every .bat and .cmd file under `root` CRLF line endings, as git does
// on Windows, and returns how many it rewrote. cmd reads a batch file's
// :labels line by line, and with LF alone it misses some, so a `goto :label`
// fails ("The system cannot find the batch label specified"). Archives made on
// Linux carry their batch files that way: ESP-IDF's export.bat could not reach
// its own error message, and a build failed with nothing to say why.
static int NormalizeBatchLineEndings(const fs::path& root)
{
#ifdef _WIN32
    int rewritten = 0;
    std::error_code ec;
    if (!fs::is_directory(root, ec))
    {
        return 0;
    }
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        std::error_code fileError;
        if (!it->is_regular_file(fileError))
        {
            continue;
        }
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".bat" && ext != ".cmd")
        {
            continue;
        }
        std::string text;
        {
            std::ifstream in(it->path(), std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        std::string fixed;
        fixed.reserve(text.size() + text.size() / 16);
        bool changed = false;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r'))
            {
                fixed += '\r';
                changed = true;
            }
            fixed += text[i];
        }
        if (changed)
        {
            std::ofstream out(it->path(), std::ios::binary | std::ios::trunc);
            out.write(fixed.data(), static_cast<std::streamsize>(fixed.size()));
            ++rewritten;
        }
    }
    return rewritten;
#else
    (void)root;
    return 0;
#endif
}

// Where an archive is kept between attempts, named by component and version
// so a newer pin never takes an older file for itself.
static std::string ArchivePath(const std::string& toolchainsDir, const std::string& name)
{
    return toolchainsDir + "/.downloads/" + name + ".zip";
}

void ToolchainComponentManager::DoInstallComponent(const ComponentDef& comp, BuildProgressCallback cb)
{
    // Status helper: forwards messages to the callback and Activity panel
    std::string actCategory = comp.displayName + " Install";
    auto status = [&cb, &actCategory](const std::string& msg)
    {
        if (cb)
        {
            BuildProgress p;
            p.state = BuildState::Building;
            p.statusText = msg;
            cb(p);
        }
        BuildActivityLog(actCategory, msg);
    };

    // Always the pinned archive from the builder definition, whose SHA-256 is
    // pinned beside it. The release CheckForUpdates finds is only shown as
    // "newer available" in the UI: a URL picked by a network response has no
    // hash to verify, and what is downloaded here runs as a build toolchain.
    // Newer toolchain versions come with editor releases that move the pin.
    const std::string url = comp.fallback.url;
    const std::string installVersion = comp.fallback.version;

    if (url.empty())
    {
        std::string err = "No download URL for " + comp.displayName;
        BuildActivityFail(actCategory, err);
        if (cb)
        {
            BuildProgress p;
            p.state = BuildState::Failed;
            p.error = err;
            cb(p);
        }
        return;
    }

    std::string destDir = m_ToolchainsDir + "/" + comp.installPath;
    std::string zipPath = ArchivePath(m_ToolchainsDir, comp.id + "-" + installVersion);

    status("Downloading " + comp.displayName + "...");
    status("URL: " + url);
    status("Saving to: " + zipPath);

    // Download and verify. The archive becomes a compiler the build runs, so
    // an unchecked download would trust both the transport and the mirror. A
    // mismatch deletes the file and fails the install rather than warning.
    bool downloaded = FetchArchive(url, zipPath, comp.sha256, status);
    if (!downloaded)
    {
        std::string err = "Download failed for " + comp.displayName;
        BuildActivityFail(actCategory, err);
        if (cb)
        {
            BuildProgress p;
            p.state = BuildState::Failed;
            p.error = err;
            cb(p);
        }
        return;
    }

    // An install replaces what is there: an update, or a component whose
    // archive changed form, would otherwise land on top of the old files and
    // leave a mix of both. Only a folder of its own below the toolchains
    // directory is cleared.
    {
        const fs::path dest = fs::path(destDir).lexically_normal();
        const fs::path rel = dest.lexically_relative(fs::path(m_ToolchainsDir).lexically_normal());
        std::error_code ec;
        if (!comp.installPath.empty() && !rel.empty() && *rel.begin() != ".." && rel != "." && fs::exists(dest, ec))
        {
            status("Removing the previous " + comp.displayName + " from " + destDir);
            fs::remove_all(dest, ec);
            if (ec)
            {
                status("Could not remove all of it: " + ec.message());
            }
        }
    }

    std::string extractDir = destDir;
    if (!comp.extractedName.empty())
    {
        fs::path parentDir = fs::path(destDir).parent_path();
        extractDir = parentDir.string();
        try
        {
            fs::create_directories(extractDir);
        }
        catch (...)
        { /* best effort: the operation that needs this path fails visibly on its own if it did not happen */
        }
    }
    else
    {
        try
        {
            fs::create_directories(destDir);
        }
        catch (...)
        { /* best effort: the operation that needs this path fails visibly on its own if it did not happen */
        }
    }

    status("Extracting " + comp.displayName + " to " + extractDir + "...");
    std::string extractError;
    bool extracted = EditorHttpUtils::ExtractZip(zipPath, extractDir, status, &extractError);

    // Kept when the extract failed, so a retry uses it (FetchArchive).
    if (extracted)
    {
        std::error_code removeError;
        fs::remove(zipPath, removeError);
        status("Removed the downloaded archive");
    }
    else
    {
        status("Kept the downloaded archive for the next attempt: " + zipPath);
    }

    // Move the extracted folder to the target path if needed.
    // An archive that wraps everything in one directory leaves the real root a
    // level below where the component says it is, and the failure shows up much
    // later as a missing tool. `extractedName` handles that when declared, but
    // it must name the folder, which for ESP-IDF includes the version, so a
    // version bump breaks it. Moving a lone directory up needs nothing declared
    // and cannot go stale.
    if (extracted && comp.extractedName.empty())
    {
        try
        {
            std::vector<fs::path> entries;
            for (const auto& entry : fs::directory_iterator(destDir))
            {
                entries.push_back(entry.path());
            }

            if (entries.size() == 1 && fs::is_directory(entries[0]))
            {
                const fs::path wrapper = entries[0];
                status("Archive wrapped everything in '" + wrapper.filename().string() + "'; lifting its contents up");
                for (const auto& child : fs::directory_iterator(wrapper))
                {
                    fs::rename(child.path(), fs::path(destDir) / child.path().filename());
                }
                fs::remove(wrapper);
            }
        }
        catch (const std::exception& e)
        {
            status(std::string("Could not lift the archive's contents: ") + e.what());
        }
    }

    if (extracted && !comp.extractedName.empty())
    {
        std::string srcPath = extractDir + "/" + comp.extractedName;
        status("Renaming " + comp.extractedName + " -> " + fs::path(destDir).filename().string());
        try
        {
            if (fs::exists(destDir))
            {
                fs::remove_all(destDir);
            }
            if (fs::exists(srcPath))
            {
                fs::rename(srcPath, destDir);
            }
        }
        catch (const std::exception& e)
        {
            status("Rename warning: " + std::string(e.what()));
        }
    }
    // Flatten: if extractedName is empty and destDir holds exactly one
    // subfolder (common with zips that have a root folder), move its contents
    // up. Not when detection uses a wildcard, which expects the nesting.
    else if (extracted && comp.extractedName.empty())
    {
        bool detectionUsesGlob = false;
        for (const auto& [platform, pathPattern] : comp.detection)
        {
            if (pathPattern.find('*') != std::string::npos)
            {
                detectionUsesGlob = true;
                break;
            }
        }

        if (!detectionUsesGlob)
        {
            try
            {
                int dirCount = 0;
                int fileCount = 0;
                std::string singleSubDir;
                for (const auto& entry : fs::directory_iterator(destDir))
                {
                    if (entry.is_directory())
                    {
                        dirCount++;
                        singleSubDir = entry.path().string();
                    }
                    else
                    {
                        fileCount++;
                    }
                }
                if (dirCount == 1 && fileCount == 0)
                {
                    status("Flattening single root folder: " + fs::path(singleSubDir).filename().string());
                    std::string tempDir = destDir + "_flatten_tmp";
                    fs::rename(singleSubDir, tempDir);
                    for (const auto& entry : fs::directory_iterator(tempDir))
                    {
                        fs::rename(entry.path(), fs::path(destDir) / entry.path().filename());
                    }
                    fs::remove(tempDir);
                }
            }
            catch (const std::exception& e)
            {
                status("Flatten warning: " + std::string(e.what()));
            }
        }
    }

    if (extracted)
    {
        if (const int fixedScripts = NormalizeBatchLineEndings(destDir); fixedScripts > 0)
        {
            status("Gave " + std::to_string(fixedScripts) + " batch file(s) Windows line endings");
        }

        // Record the installed version so "Check for Updates" can compare it with
        // the latest release and flag outdated components.
        WriteInstalledVersion(m_ToolchainsDir, comp.id, installVersion);
        // These files are new; any earlier setup was for the ones they replaced.
        // The post-install step runs right after this.
        EditorSettings::SetBool("setup." + comp.id, false);
        {
            std::lock_guard<std::mutex> lock(m_VersionMutex);
            auto& info = m_VersionCache[comp.id];
            info.name = comp.displayName;
            info.installedVersion = installVersion;
            if (info.latestVersion.empty())
            {
                info.latestVersion = installVersion;
            }
            info.updateAvailable = false;
        }
        status("Installed " + comp.displayName + " " + installVersion + " to: " + destDir);
        BuildActivityComplete(actCategory);
    }
    else
    {
        BuildActivityFail(actCategory, "Failed to extract " + comp.displayName + ": " + extractError);
    }

    if (cb)
    {
        BuildProgress p;
        p.state = extracted ? BuildState::Completed : BuildState::Failed;
        p.statusText = extracted ? (comp.displayName + " installed") : "Extraction failed";
        p.progress = 1.0f;
        if (!extracted)
        {
            p.error = "Failed to extract " + comp.displayName + ": " + extractError;
        }
        cb(p);
    }
}

void ToolchainComponentManager::DoInstallSubComponents(const ComponentDef& comp, BuildProgressCallback cb)
{
    std::string actCategory = comp.displayName + " Install";
    auto status = [&cb, &actCategory](const std::string& msg)
    {
        if (cb)
        {
            BuildProgress p;
            p.state = BuildState::Building;
            p.statusText = msg;
            cb(p);
        }
        BuildActivityLog(actCategory, msg);
    };

    std::string baseDir = m_ToolchainsDir + "/" + comp.installPath;

    try
    {
        fs::create_directories(baseDir);
    }
    catch (...)
    { /* best effort: the operation that needs this path fails visibly on its own if it did not happen */
    }

    int total = (int)comp.subComponents.size();
    int current = 0;

    for (const auto& sub : comp.subComponents)
    {
        status("(" + std::to_string(current + 1) + "/" + std::to_string(total) + ") Downloading " + sub.name + "...");
        status("URL: " + sub.url);

        std::string zipPath = ArchivePath(m_ToolchainsDir, comp.id + "-" + sub.name);

        bool downloaded = FetchArchive(sub.url, zipPath, sub.sha256, status);
        if (!downloaded)
        {
            std::string err = "Download failed for " + sub.name;
            BuildActivityFail(actCategory, err);
            if (cb)
            {
                BuildProgress p;
                p.state = BuildState::Failed;
                p.error = err;
                cb(p);
            }
            return;
        }

        status("Extracting " + sub.name + "...");
        std::string extractError;
        bool extracted = EditorHttpUtils::ExtractZip(zipPath, baseDir, status, &extractError);
        // Kept when the extract failed, so a retry uses it (FetchArchive).
        if (extracted)
        {
            std::error_code removeError;
            fs::remove(zipPath, removeError);
        }

        if (!extracted)
        {
            std::string err = "Extraction failed for " + sub.name + ": " + extractError;
            BuildActivityFail(actCategory, err);
            if (cb)
            {
                BuildProgress p;
                p.state = BuildState::Failed;
                p.error = err;
                cb(p);
            }
            return;
        }

        // Rename extracted folder to target name
        if (!sub.extractedName.empty() && !sub.targetName.empty() && sub.extractedName != sub.targetName)
        {
            std::string srcPath = baseDir + "/" + sub.extractedName;
            std::string dstPath = baseDir + "/" + sub.targetName;
            status("Renaming " + sub.extractedName + " -> " + sub.targetName);
            try
            {
                if (fs::exists(dstPath))
                {
                    fs::remove_all(dstPath);
                }
                if (fs::exists(srcPath))
                {
                    fs::rename(srcPath, dstPath);
                }
            }
            catch (const std::exception&)
            {
            }
        }

        status(sub.name + " installed");
        current++;
    }

    BuildActivityComplete(actCategory);

    if (cb)
    {
        BuildProgress p;
        p.state = BuildState::Completed;
        p.statusText = comp.displayName + " installed";
        p.progress = 1.0f;
        cb(p);
    }
}

// ============================================================================
// Setup (run postInstall script)
// ============================================================================

void ToolchainComponentManager::SetupComponent(const std::string& componentId, BuildProgressCallback cb)
{
    if (m_IsBusy)
    {
        return;
    }

    const ComponentDef* compDef = nullptr;
    for (const auto& c : m_Definition.components)
    {
        if (c.id == componentId)
        {
            compDef = &c;
            break;
        }
    }

    if (!compDef)
    {
        if (cb)
        {
            BuildProgress p;
            p.state = BuildState::Failed;
            p.error = "Unknown component: " + componentId;
            cb(p);
        }
        return;
    }

    if (m_WorkerThread.joinable())
    {
        m_WorkerThread.join();
    }

    m_IsBusy = true;

    ComponentDef def = *compDef;

    m_WorkerThread = std::thread(
        [this, def, cb]()
        {
            DoSetupComponent(def, cb);
            m_IsBusy = false;
        });
}

void ToolchainComponentManager::DoSetupComponent(const ComponentDef& comp, BuildProgressCallback cb)
{
    std::string actCategory = comp.displayName + " Setup";
    auto status = [&cb, &actCategory](const std::string& msg)
    {
        if (cb)
        {
            BuildProgress p;
            p.state = BuildState::Building;
            p.statusText = msg;
            cb(p);
        }
        BuildActivityLog(actCategory, msg);
    };

    // Pick platform-specific postInstall command
#ifdef _WIN32
    std::string postInstallTemplate = comp.postInstall.windows;
#else
    std::string postInstallTemplate = comp.postInstall.unixLike;
#endif

    if (postInstallTemplate.empty())
    {
        std::string err = "No postInstall script defined for " + comp.displayName;
        BuildActivityFail(actCategory, err);
        if (cb)
        {
            BuildProgress p;
            p.state = BuildState::Failed;
            p.error = err;
            cb(p);
        }
        return;
    }

    std::string resolved = ResolvePath(postInstallTemplate, comp);

    // Split into command path and arguments at the first space after the file extension
    // e.g. "espressif/esp-idf/*/install.bat esp32s3" → command="espressif/esp-idf/*/install.bat", args="esp32s3"
    std::string commandPart = resolved;
    std::string argsPart;

    // Find the script file extension boundary, then the space after it
    size_t extPos = resolved.find(".bat");
    if (extPos == std::string::npos)
    {
        extPos = resolved.find(".sh");
    }
    if (extPos != std::string::npos)
    {
        size_t endExt = resolved.find(' ', extPos);
        if (endExt != std::string::npos)
        {
            commandPart = resolved.substr(0, endExt);
            argsPart = resolved.substr(endExt + 1);
        }
    }

    // Resolve wildcard in command path
    std::string fullCommand;
    size_t starPos = commandPart.find('*');
    if (starPos != std::string::npos)
    {
        std::string prefix = commandPart.substr(0, starPos);
        std::string suffix = commandPart.substr(starPos + 1);

        std::string parentDir = m_ToolchainsDir + "/" + prefix;
        if (!parentDir.empty() && (parentDir.back() == '/' || parentDir.back() == '\\'))
        {
            parentDir.pop_back();
        }

        bool found = false;
        try
        {
            if (fs::exists(parentDir) && fs::is_directory(parentDir))
            {
                for (const auto& entry : fs::directory_iterator(parentDir))
                {
                    std::string candidate = entry.path().string() + suffix;
                    std::replace(candidate.begin(), candidate.end(), '\\', '/');
                    if (fs::exists(candidate))
                    {
                        fullCommand = candidate;
                        found = true;
                        break;
                    }
                }
            }
        }
        catch (const std::exception&)
        {
        }

        if (!found)
        {
            std::string err = "Could not find setup script matching: " + commandPart;
            BuildActivityFail(actCategory, err);
            if (cb)
            {
                BuildProgress p;
                p.state = BuildState::Failed;
                p.error = err;
                cb(p);
            }
            return;
        }
    }
    else
    {
        fullCommand = m_ToolchainsDir + "/" + commandPart;
    }

    std::string workDir = fs::path(fullCommand).parent_path().string();

    // Here as well as after the extract, for a copy installed before it was
    // done there (see NormalizeBatchLineEndings).
    if (const int fixedScripts = NormalizeBatchLineEndings(m_ToolchainsDir + "/" + comp.installPath);
        fixedScripts > 0)
    {
        status("Gave " + std::to_string(fixedScripts) + " batch file(s) Windows line endings");
    }

    // The installed components' PATH entries go first, so the script runs
    // their tools (ESP-IDF's install.bat runs `python`) rather than whatever
    // the machine has: on a fresh Windows, only the Microsoft Store's
    // python.exe stub, which exits with 9009.
    std::string pathPrefix;
    for (const std::string& entry : InstalledPathEntries())
    {
#ifdef _WIN32
        pathPrefix += entry + ";";
#else
        pathPrefix += entry + ":";
#endif
    }
#ifdef _WIN32
    std::replace(fullCommand.begin(), fullCommand.end(), '/', '\\');
    std::replace(workDir.begin(), workDir.end(), '/', '\\');

    // set "MSYSTEM=" && call "script.bat" args
    //
    // MSYSTEM must be cleared for the child. It is inherited from any MSYS2 or
    // Git Bash session (the shell this editor's own compiler lives in), and a
    // batch script may take it to mean it is not running under cmd.exe. The
    // ESP-IDF installer does: `if defined MSYSTEM goto :__end` exits 0 having
    // installed nothing, leaving a new SDK with no compilers. The ESP32 backend
    // clears it the same way for export.bat.
    std::string cmdLine = "set \"MSYSTEM=\" && call \"" + fullCommand + "\"";
    if (!pathPrefix.empty())
    {
        std::replace(pathPrefix.begin(), pathPrefix.end(), '/', '\\');
        cmdLine = "set \"PATH=" + pathPrefix + "%PATH%\" && " + cmdLine;
    }
#else
    std::string cmdLine = "\"" + fullCommand + "\"";
    if (!pathPrefix.empty())
    {
        cmdLine = "PATH=\"" + pathPrefix + "$PATH\" " + cmdLine;
    }
#endif
    if (!argsPart.empty())
    {
        cmdLine += " " + argsPart;
    }

    status("Running: " + cmdLine);
    status("Working directory: " + workDir);

    const int exitCode = RunShellCommand(cmdLine, workDir, [&status](const std::string& line) { status(line); });

    if (exitCode == 0)
    {
        EditorSettings::SetBool("setup." + comp.id, true);
        status("Setup completed successfully");
        BuildActivityComplete(actCategory);
        if (cb)
        {
            BuildProgress p;
            p.state = BuildState::Completed;
            p.statusText = comp.displayName + " setup complete";
            p.progress = 1.0f;
            cb(p);
        }
    }
    else
    {
        std::string err = exitCode < 0 ? "Failed to launch setup script"
                                       : "Setup script exited with code " + std::to_string(exitCode);
        BuildActivityFail(actCategory, err);
        if (cb)
        {
            BuildProgress p;
            p.state = BuildState::Failed;
            p.error = err;
            cb(p);
        }
    }
}

// ============================================================================
// Version Check
// ============================================================================

void ToolchainComponentManager::CheckForUpdates(std::function<void()> onComplete)
{
    if (m_IsBusy)
    {
        if (onComplete)
        {
            onComplete();
        }
        return;
    }

    if (m_WorkerThread.joinable())
    {
        m_WorkerThread.join();
    }

    m_IsBusy = true;

    m_WorkerThread = std::thread(
        [this, onComplete]()
        {
            for (const auto& comp : m_Definition.components)
            {
                // 1. Determine the latest available version (+ download URL).
                std::string latest;
                std::string downloadUrl;
                if (comp.versionCheck.type == "github_release" && !comp.versionCheck.repo.empty())
                {
                    std::string apiUrl = "https://api.github.com/repos/" + comp.versionCheck.repo + "/releases/latest";
                    std::string response = EditorHttpUtils::FetchUrl(apiUrl);
                    if (!response.empty())
                    {
                        std::string version, dl;
                        if (EditorHttpUtils::ParseGitHubRelease(response, comp.versionCheck.assetPattern,
                                                                comp.versionCheck.repo, version, dl))
                        {
                            latest = version;
                            downloadUrl = dl;
                        }
                    }
                }
                else if (comp.versionCheck.type == "static")
                {
                    latest = comp.versionCheck.version;
                }

                // 2. Compare with the version recorded when this component was
                //    installed. An update is available only if it is installed,
                //    both versions are known, and they differ.
                const bool installed = EvaluateDetection(comp);
                const std::string installedVer =
                    installed ? ReadInstalledVersion(m_ToolchainsDir, comp.id) : std::string();
                // Only a version actually fetched can flag an update, so a failed
                // fetch does not show a false "update available".
                const bool update = installed && !installedVer.empty() && !latest.empty() &&
                                    NormalizeVersion(installedVer) != NormalizeVersion(latest);

                // For display, the definition's version when no latest could be
                // fetched (no version check, or the fetch failed).
                const std::string displayLatest = !latest.empty() ? latest : comp.fallback.version;

                // 3. Update the cache.
                std::lock_guard<std::mutex> lock(m_VersionMutex);
                auto& info = m_VersionCache[comp.id];
                info.name = comp.displayName;
                if (!displayLatest.empty())
                {
                    info.latestVersion = displayLatest;
                }
                if (!downloadUrl.empty())
                {
                    info.downloadUrl = downloadUrl;
                }
                info.installedVersion = installedVer;
                info.updateAvailable = update;
                info.checked = true;
            }

            m_IsBusy = false;
            if (onComplete)
            {
                onComplete();
            }
        });
}

}  // namespace DekiEditor
