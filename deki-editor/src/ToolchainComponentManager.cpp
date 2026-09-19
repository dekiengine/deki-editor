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
// Installed-version tracking. We record the version a component was installed at
// in a small marker file under <toolchains>/.versions/<id>, so update checks can
// compare it against the latest available release.
std::string Trim(std::string s)
{
    auto notSpace = [](unsigned char c)
    { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

// Normalize for comparison: trim + drop a leading 'v' (e.g. "v5.1" == "5.1").
std::string NormalizeVersion(std::string v)
{
    v = Trim(std::move(v));
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V'))
        v.erase(v.begin());
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
        return {};
    std::string v;
    std::getline(f, v);
    return Trim(v);
}

void WriteInstalledVersion(const std::string& toolchainsDir, const std::string& compId,
                           const std::string& version)
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
        f << version;
}
}  // namespace

ToolchainComponentManager::ToolchainComponentManager() = default;

ToolchainComponentManager::~ToolchainComponentManager()
{
    if (m_WorkerThread.joinable())
        m_WorkerThread.join();
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

    // Replace {installPath} with the component's install path
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
    // Check if path contains a * wildcard
    size_t starPos = resolvedPath.find('*');
    if (starPos == std::string::npos)
    {
        // No wildcard, direct check
        std::string fullPath = m_ToolchainsDir + "/" + resolvedPath;
        return fs::exists(fullPath);
    }

    // Split at the wildcard: everything before * is the parent dir, everything after is the suffix
    std::string prefix = resolvedPath.substr(0, starPos);
    std::string suffix = resolvedPath.substr(starPos + 1);

    std::string parentDir = m_ToolchainsDir + "/" + prefix;

    // Remove trailing slash from parent if present
    if (!parentDir.empty() && (parentDir.back() == '/' || parentDir.back() == '\\'))
        parentDir.pop_back();

    if (!fs::exists(parentDir) || !fs::is_directory(parentDir))
        return false;

    // Scan parent directory for any entry that matches when combined with suffix
    try
    {
        for (const auto& entry : fs::directory_iterator(parentDir))
        {
            std::string candidate = entry.path().string() + suffix;
            // Normalize slashes
            std::replace(candidate.begin(), candidate.end(), '\\', '/');
            if (fs::exists(candidate))
                return true;
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
        return false;

    for (const auto& [platform, pathPattern] : comp.detection)
    {
        // Filter by current platform
#ifdef _WIN32
        if (platform != "windows" && platform != "any")
            continue;
#else
        if (platform != "unix" && platform != "any")
            continue;
#endif

        std::string resolved = ResolvePath(pathPattern, comp);

        // Check for directory_exists (paths ending without an extension typically)
        // or file_exists (with extension)
        if (MatchesGlob(resolved))
            return true;
    }

    return false;
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

        // Check if installed
        bool installed = EvaluateDetection(compDef);
        comp.status = installed ? ToolchainComponentStatus::Installed
                                : ToolchainComponentStatus::NotInstalled;

        // The pinned version is what an install puts down, so it is what the
        // backend was written against. An install of another version - left
        // behind when a package moved its pin - is out of date, and says so
        // without a network check. Only "Check for Updates" could flag it
        // before, by comparing against the newest release, which is not the
        // same question.
        const std::string installedVer = installed ? ReadInstalledVersion(m_ToolchainsDir, compDef.id)
                                                   : std::string();
        if (!installedVer.empty())
            comp.installedVersion = installedVer;
        if (!installedVer.empty() && !compDef.fallback.version.empty() &&
            NormalizeVersion(installedVer) != NormalizeVersion(compDef.fallback.version))
            comp.status = ToolchainComponentStatus::UpdateAvailable;

        // Fill version info from cache
        {
            std::lock_guard<std::mutex> lock(m_VersionMutex);
            auto it = m_VersionCache.find(compDef.id);
            if (it != m_VersionCache.end())
            {
                comp.installedVersion = it->second.installedVersion;
                comp.latestVersion = it->second.latestVersion;

                if (installed && it->second.updateAvailable)
                    comp.status = ToolchainComponentStatus::UpdateAvailable;
                if (!installedVer.empty())
                    comp.installedVersion = installedVer;
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
        return;

    // Find component definition
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
        m_WorkerThread.join();

    m_IsBusy = true;

    // Copy the component def for the thread
    ComponentDef def = *compDef;

    m_WorkerThread = std::thread([this, def, cb]()
                                 {
        if (!def.subComponents.empty())
            DoInstallSubComponents(def, cb);
        else
            DoInstallComponent(def, cb);

        // Auto-run setup if the component has a postInstall script and installed successfully
        if (def.canSetup && EvaluateDetection(def))
        {
#ifdef _WIN32
            bool hasPostInstall = !def.postInstall.windows.empty();
#else
            bool hasPostInstall = !def.postInstall.unix_.empty();
#endif
            if (hasPostInstall)
                DoSetupComponent(def, nullptr);
        }

        m_IsBusy = false; });
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
    // pinned beside it. The release fetched by CheckForUpdates is shown as
    // "newer available" in the UI and nothing more: a URL chosen by a
    // response from the network has no hash to verify against, and what is
    // downloaded here is executed as a build toolchain. Newer toolchain
    // versions arrive with editor releases that move the pin.
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
    std::string zipPath = m_ToolchainsDir + "/" + comp.id + "_download.zip";

    status("Downloading " + comp.displayName + "...");
    status("URL: " + url);
    status("Saving to: " + zipPath);

    // Download and verify. The archive becomes a compiler the build runs, so a
    // download nobody checked trusts both the transport and the mirror. A
    // mismatch deletes the file and fails the install rather than warning.
    bool downloaded = EditorHttpUtils::DownloadFileVerified(url, zipPath, comp.sha256, nullptr, status);
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

    // Extract
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
    bool extracted = EditorHttpUtils::ExtractZip(zipPath, extractDir, status);

    // Clean up zip
    try
    {
        fs::remove(zipPath);
    }
    catch (...)
    { /* best effort: the operation that needs this path fails visibly on its own if it did not happen */
    }
    status("Cleaned up temporary archive");

    // Rename extracted folder to target path if needed
    // An archive that wraps everything in one directory leaves the real root a
    // level below where the component says it is, and the failure surfaces much
    // later as a missing tool. `extractedName` handles that when it is declared,
    // but it has to name the folder, which for ESP-IDF carries the version, so a
    // version bump silently breaks it. Hoisting a lone directory needs nothing
    // declared and cannot go stale.
    if (extracted && comp.extractedName.empty())
    {
        try
        {
            std::vector<fs::path> entries;
            for (const auto& entry : fs::directory_iterator(destDir))
                entries.push_back(entry.path());

            if (entries.size() == 1 && fs::is_directory(entries[0]))
            {
                const fs::path wrapper = entries[0];
                status("Archive wrapped everything in '" + wrapper.filename().string() +
                       "'; lifting its contents up");
                for (const auto& child : fs::directory_iterator(wrapper))
                    fs::rename(child.path(), fs::path(destDir) / child.path().filename());
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
                fs::remove_all(destDir);
            if (fs::exists(srcPath))
                fs::rename(srcPath, destDir);
        }
        catch (const std::exception& e)
        {
            status("Rename warning: " + std::string(e.what()));
        }
    }
    // Auto-flatten: if extractedName is empty and destDir contains exactly one subfolder
    // (common with zips that have a root folder), move its contents up.
    // Skip if detection uses a wildcard — that means it expects the nested structure.
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
        // Record the installed version so "Check for Updates" can compare it to the
        // latest release and flag outdated components.
        WriteInstalledVersion(m_ToolchainsDir, comp.id, installVersion);
        // These files are new; whatever setup ran was for the ones they
        // replaced. (The post-install step runs right after this anyway.)
        EditorSettings::SetBool("setup." + comp.id, false);
        {
            std::lock_guard<std::mutex> lock(m_VersionMutex);
            auto& info = m_VersionCache[comp.id];
            info.name = comp.displayName;
            info.installedVersion = installVersion;
            if (info.latestVersion.empty())
                info.latestVersion = installVersion;
            info.updateAvailable = false;
        }
        status("Installed " + comp.displayName + " " + installVersion + " to: " + destDir);
        BuildActivityComplete(actCategory);
    }
    else
    {
        BuildActivityFail(actCategory, "Failed to extract " + comp.displayName);
    }

    if (cb)
    {
        BuildProgress p;
        p.state = extracted ? BuildState::Completed : BuildState::Failed;
        p.statusText = extracted ? (comp.displayName + " installed") : "Extraction failed";
        p.progress = 1.0f;
        if (!extracted) p.error = "Failed to extract " + comp.displayName;
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

        std::string zipPath = baseDir + "/" + sub.name + ".zip";

        bool downloaded = EditorHttpUtils::DownloadFileVerified(sub.url, zipPath, sub.sha256, nullptr, status);
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
        bool extracted = EditorHttpUtils::ExtractZip(zipPath, baseDir, status);
        try
        {
            fs::remove(zipPath);
        }
        catch (...)
        { /* best effort: the operation that needs this path fails visibly on its own if it did not happen */
        }

        if (!extracted)
        {
            std::string err = "Extraction failed for " + sub.name;
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
        if (!sub.extractedName.empty() && !sub.targetName.empty() &&
            sub.extractedName != sub.targetName)
        {
            std::string srcPath = baseDir + "/" + sub.extractedName;
            std::string dstPath = baseDir + "/" + sub.targetName;
            status("Renaming " + sub.extractedName + " -> " + sub.targetName);
            try
            {
                if (fs::exists(dstPath))
                    fs::remove_all(dstPath);
                if (fs::exists(srcPath))
                    fs::rename(srcPath, dstPath);
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
        return;

    // Find component definition
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
        m_WorkerThread.join();

    m_IsBusy = true;

    ComponentDef def = *compDef;

    m_WorkerThread = std::thread([this, def, cb]()
                                 {
        DoSetupComponent(def, cb);
        m_IsBusy = false; });
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
    std::string postInstallTemplate = comp.postInstall.unix_;
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

    // Replace {installPath}
    std::string resolved = ResolvePath(postInstallTemplate, comp);

    // Split into command path and arguments at the first space after the file extension
    // e.g. "espressif/esp-idf/*/install.bat esp32s3" → command="espressif/esp-idf/*/install.bat", args="esp32s3"
    std::string commandPart = resolved;
    std::string argsPart;

    // Find the script file extension boundary, then the space after it
    size_t extPos = resolved.find(".bat");
    if (extPos == std::string::npos)
        extPos = resolved.find(".sh");
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
            parentDir.pop_back();

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
#ifdef _WIN32
    std::replace(fullCommand.begin(), fullCommand.end(), '/', '\\');
    std::replace(workDir.begin(), workDir.end(), '/', '\\');

    // set "MSYSTEM=" && call "script.bat" args
    //
    // MSYSTEM cleared for the child. It is inherited from every MSYS2 or Git
    // Bash session - the shell this editor's own compiler lives in - and a
    // batch script may take it to mean it is not running under cmd.exe. The
    // ESP-IDF installer does exactly that: `if defined MSYSTEM goto :__end`,
    // which EXITS 0 having installed nothing, so setup reported "complete" and
    // left a new SDK with no compilers. The child is cmd.exe; only the
    // inherited variable said otherwise. (The ESP32 backend clears it the same
    // way for export.bat.)
    std::string cmdLine = "set \"MSYSTEM=\" && call \"" + fullCommand + "\"";
#else
    std::string cmdLine = "\"" + fullCommand + "\"";
#endif
    if (!argsPart.empty())
        cmdLine += " " + argsPart;

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
        if (onComplete) onComplete();
        return;
    }

    if (m_WorkerThread.joinable())
        m_WorkerThread.join();

    m_IsBusy = true;

    m_WorkerThread = std::thread([this, onComplete]()
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

            // 2. Compare against the version recorded when this component was
            //    installed. An update is available only if it's installed, we know
            //    both versions, and they differ.
            const bool installed = EvaluateDetection(comp);
            const std::string installedVer = installed ? ReadInstalledVersion(m_ToolchainsDir, comp.id)
                                                       : std::string();
            // Only flag an update from a real fetched version (so a failed network
            // fetch doesn't produce a false "update available").
            const bool update = installed && !installedVer.empty() && !latest.empty() &&
                                NormalizeVersion(installedVer) != NormalizeVersion(latest);

            // For display, fall back to the definition's version when we couldn't
            // fetch a latest (no version check, or the fetch failed).
            const std::string displayLatest = !latest.empty() ? latest : comp.fallback.version;

            // 3. Update the cache.
            std::lock_guard<std::mutex> lock(m_VersionMutex);
            auto& info = m_VersionCache[comp.id];
            info.name = comp.displayName;
            if (!displayLatest.empty()) info.latestVersion = displayLatest;
            if (!downloadUrl.empty())   info.downloadUrl = downloadUrl;
            info.installedVersion = installedVer;
            info.updateAvailable = update;
            info.checked = true;
        }

        m_IsBusy = false;
        if (onComplete) onComplete(); });
}

}  // namespace DekiEditor
