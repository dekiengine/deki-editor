#include <algorithm>
#include <cstdlib>
#include "ESPIDFToolchain.h"
#include <deki-editor/SafeNames.h>
#include <deki-editor/EditorPaths.h>
#include <deki-editor/build/PlatformConfig.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace DekiEditor
{

std::string ESPIDFToolchain::GetDekiEditorDir() const
{
    return EditorPaths::GetEditorDataDir();
}

std::string ESPIDFToolchain::GetToolchainsDir() const
{
    return EditorPaths::GetToolchainsDir();
}

std::string ESPIDFToolchain::ReadEngineVersion(const std::string& projectPath) const
{
    fs::path dekiJsonPath = fs::path(projectPath) / "deki.json";
    if (!fs::exists(dekiJsonPath))
        return "";

    try
    {
        std::ifstream file(dekiJsonPath);
        if (!file.is_open())
            return "";

        nlohmann::json config = nlohmann::json::parse(file);
        if (config.contains("engineVersion"))
            return config["engineVersion"].get<std::string>();
    }
    catch (const std::exception&)
    {
    }

    return "";
}

std::string ESPIDFToolchain::GetIDFPath() const
{
    std::string idfDir = GetToolchainsDir() + "/espressif/esp-idf";

    // The SDK sitting directly in its install directory is the normal case now
    // that the installer lifts a wrapping folder out of the archive. The search
    // below stays for installs made before it did.
#ifdef _WIN32
    if (fs::exists(fs::path(idfDir) / "export.bat"))
        return idfDir;
#else
    if (fs::exists(fs::path(idfDir) / "export.sh"))
        return idfDir;
#endif

    try
    {
        if (fs::exists(idfDir))
        {
            for (const auto& entry : fs::directory_iterator(idfDir))
            {
                if (entry.is_directory())
                {
                    std::string name = entry.path().filename().string();
                    if (name.find("esp-idf") != std::string::npos)
                    {
                        return entry.path().string();
                    }
                }
            }
        }
    }
    catch (const std::exception&)
    {
    }

    return idfDir;
}

bool ESPIDFToolchain::IsInstalled() const
{
    std::string idfPath = GetIDFPath();

#ifdef _WIN32
    std::string exportScript = idfPath + "/export.bat";
#else
    std::string exportScript = idfPath + "/export.sh";
#endif

    return fs::exists(exportScript);
}

std::string ESPIDFToolchain::GetStatus() const
{
    if (IsInstalled())
        return "ESP-IDF installed at " + GetIDFPath();
    return "ESP-IDF not installed";
}

void ESPIDFToolchain::PrepareForBuild(BuildOutputCallback outputCallback,
                                      const std::string& buildDir)
{
    try
    {
        fs::path appDescObj = fs::path(buildDir) / "build" / "esp-idf" / "esp_app_format" /
                              "CMakeFiles" / "__idf_esp_app_format.dir" / "esp_app_desc.c.obj";

        if (fs::exists(appDescObj))
        {
            fs::remove(appDescObj);
            if (outputCallback) outputCallback("Deleted esp_app_desc.c.obj to refresh compile timestamp", false);
        }
    }
    catch (const std::exception&)
    {
    }
}

// ============================================================================
// ExecuteIDF
// ============================================================================

#ifdef _WIN32

int ESPIDFToolchain::ExecuteIDF(const std::string& command, const std::string& workDir,
                                const std::string& enginePath, BuildOutputCallback outputCallback,
                                const ESPIDFExecContext& ctx)
{
    std::string idfPath = GetIDFPath();

    std::string logEnabled = ctx.enableLogging ? "1" : "";
    std::string internalLogEnabled = ctx.enableInternalLogging ? "1" : "";

    std::string envSettings = "set \"DEKI_ENGINE_PATH=" + enginePath + "\" && ";

    // Anything that must happen before export.bat runs. cmd expands %PATH%
    // when it parses the whole line rather than when each piece runs, so
    // appending to PATH after the export would append to the PATH as it was
    // beforehand and discard everything ESP-IDF had just added, leaving idf.py
    // unfindable. Doing it first is correct either way: export.bat prepends
    // its own entries and leaves ours in place behind them.
    std::string preExportSettings;

    // The reflection generator's compiler, handed down explicitly.
    //
    // A stripped firmware build regenerates the reflection tables, and that
    // needs GCC 16. Inside the ESP-IDF environment PATH belongs to Espressif,
    // so a search for g++ there finds the cross compiler or nothing at all.
    // The editor was itself built with the compiler in question, so it is the
    // one thing in the chain that knows where it is.
    {
        std::string gxx16;
        if (const char* fromEnv = std::getenv("DEKI_GXX16"); fromEnv && *fromEnv)
        {
            gxx16 = fromEnv;
        }
        else
        {
            for (const char* candidate : { "C:/msys64/mingw64/bin/g++.exe",
                                           "C:/mingw64/bin/g++.exe",
                                           "C:/MinGW/bin/g++.exe" })
            {
                if (fs::exists(candidate))
                {
                    gxx16 = candidate;
                    break;
                }
            }
        }
        if (!gxx16.empty())
        {
            envSettings += "set \"DEKI_GXX16=" + gxx16 + "\" && ";

            // And its own directory on PATH, or the compiler cannot run at all.
            //
            // g++ is only a driver: the real compiler is cc1plus, which lives
            // in lib/gcc/... rather than beside g++, so Windows resolves its
            // DLLs through PATH. export.bat replaces PATH with Espressif's,
            // cc1plus then fails to start, and because it never starts it
            // prints nothing: the build reported "generator compile failed"
            // with no diagnostics whatsoever.
            //
            // Appended, not prepended: ESP-IDF's own cmake, ninja and python
            // must keep winning, and nothing in its directories supplies the
            // libraries cc1plus wants.
            std::string compilerDir = fs::path(gxx16).parent_path().string();
            std::replace(compilerDir.begin(), compilerDir.end(), '/', '\\');
            if (!compilerDir.empty())
                preExportSettings += "set \"PATH=%PATH%;" + compilerDir + "\" && ";

            if (outputCallback)
                outputCallback("[Codegen] DEKI_GXX16=" + gxx16 + " (PATH += " + compilerDir + ")",
                               false);
        }
    }

    envSettings += "set \"DEKI_LOG_ENABLED=" + logEnabled + "\" && ";
    envSettings += "set \"DEKI_LOG_INTERNAL_ENABLED=" + internalLogEnabled + "\" && ";

    if (ctx.hasPlatformConfig && ctx.platformConfig)
    {
        if (ctx.platformConfig->screenWidth > 0)
            envSettings += "set \"DEKI_SCREEN_WIDTH=" + std::to_string(ctx.platformConfig->screenWidth) + "\" && ";
        if (ctx.platformConfig->screenHeight > 0)
            envSettings += "set \"DEKI_SCREEN_HEIGHT=" + std::to_string(ctx.platformConfig->screenHeight) + "\" && ";
        bool usePsram = ctx.platformConfig->psramSize > 0;
        envSettings += "set \"DEKI_USE_PSRAM=" + std::string(usePsram ? "1" : "") + "\" && ";
        if (!ctx.platformConfig->displayBus.empty())
            envSettings += "set \"DEKI_DISPLAY_BUS=" + ctx.platformConfig->displayBus + "\" && ";

        if (outputCallback)
        {
            outputCallback("[Platform Config] DEKI_SCREEN_WIDTH=" + std::to_string(ctx.platformConfig->screenWidth), false);
            outputCallback("[Platform Config] DEKI_SCREEN_HEIGHT=" + std::to_string(ctx.platformConfig->screenHeight), false);
        }
    }
    else
    {
        if (outputCallback)
            outputCallback("[Platform Config] Not set - using defaults from target header", false);
    }

    if (ctx.packageDefines && !ctx.packageDefines->empty())
    {
        // These come from package.json files and land inside a cmd.exe line:
        // only identifier-shaped defines get through (SafeNames::IsSafeDefine).
        std::string definesList;
        for (const auto& def : *ctx.packageDefines)
        {
            std::string reason;
            if (!SafeNames::IsSafeDefine(def, reason))
            {
                if (outputCallback) outputCallback("[Package Config] skipping define '" + def + "': " + reason, true);
                continue;
            }
            if (!definesList.empty()) definesList += ";";
            definesList += def;
        }
        envSettings += "set \"DEKI_PACKAGE_DEFINES=" + definesList + "\" && ";

        if (outputCallback)
        {
            outputCallback("[Package Config] Auto-detected defines:", false);
            for (const auto& def : *ctx.packageDefines)
                outputCallback("  " + def, false);
        }
    }

    // export.bat by its full path, not by name after a cd. Windows can be
    // configured with NoDefaultCurrentDirectoryInExePath, and then cmd refuses
    // to run anything from the working directory: the build failed with
    // "export.bat is not recognised" while the file sat right there.
    // /S: with this many quoted segments cmd otherwise strips the wrong pair
    // and reports "the filename, directory name, or volume label syntax is
    // incorrect". /S makes it take everything between the outermost quotes
    // literally.
    // Backslashes for the script's own path. export.bat works out where it
    // lives from %~dp0 and decides from that whether it is running under
    // CMD; handed a forward-slash path it concludes it is not, and refuses
    // with "This .bat file is for Windows CMD.EXE shell only".
    std::string idfPathNative = idfPath;
    std::replace(idfPathNative.begin(), idfPathNative.end(), '/', '\\');

    // MSYSTEM unset first. export.bat refuses outright when it sees that
    // variable, on the grounds that it is being run from an MSYS shell rather
    // than CMD. It is set for every child of an MSYS2 or Git Bash session, so
    // launching the editor from the same shell its own compiler lives in was
    // enough to make every firmware build fail with "This .bat file is for
    // Windows CMD.EXE shell only". The child really is CMD; only the inherited
    // variable said otherwise.
    std::string fullCommand = "cmd /s /c \"set \"MSYSTEM=\" && " + preExportSettings +
                              "cd /d \"" + idfPathNative +
                              "\" && call \"" + idfPathNative + "\\export.bat\" && cd /d \"" +
                              workDir + "\" && " + envSettings + command + "\"";

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
    {
        return -1;
    }

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<char> cmdBuf(fullCommand.begin(), fullCommand.end());
    cmdBuf.push_back('\0');

    BOOL success = CreateProcessA(
        NULL,
        cmdBuf.data(),
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &si,
        &pi);

    CloseHandle(hWritePipe);

    if (!success)
    {
        CloseHandle(hReadPipe);
        return -1;
    }

    char buffer[4096];
    DWORD bytesRead;
    std::string lineBuffer;

    while (true)
    {
        if (ctx.cancelRequested)
        {
            TerminateProcess(pi.hProcess, 1);
            break;
        }

        BOOL readSuccess = ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL);
        if (!readSuccess || bytesRead == 0)
        {
            break;
        }

        buffer[bytesRead] = '\0';
        lineBuffer += buffer;

        size_t pos;
        while ((pos = lineBuffer.find('\n')) != std::string::npos)
        {
            std::string line = lineBuffer.substr(0, pos);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            lineBuffer = lineBuffer.substr(pos + 1);

            if (outputCallback && !line.empty())
            {
                bool isError = (line.find("error:") != std::string::npos) ||
                               (line.find("Error:") != std::string::npos) ||
                               (line.find("ERROR") != std::string::npos) ||
                               (line.find("fatal error") != std::string::npos) ||
                               (line.find("FAILED") != std::string::npos);
                outputCallback(line, isError);
            }
        }
    }

    if (outputCallback && !lineBuffer.empty())
    {
        bool isError = (lineBuffer.find("error:") != std::string::npos) ||
                       (lineBuffer.find("Error:") != std::string::npos);
        outputCallback(lineBuffer, isError);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

    return static_cast<int>(exitCode);
}

#else

int ESPIDFToolchain::ExecuteIDF(const std::string& command, const std::string& workDir,
                                const std::string& enginePath, BuildOutputCallback outputCallback,
                                const ESPIDFExecContext& ctx)
{
    std::string idfPath = GetIDFPath();

    std::string logEnabled = ctx.enableLogging ? "1" : "";
    std::string internalLogEnabled = ctx.enableInternalLogging ? "1" : "";

    std::string envExports = "export DEKI_ENGINE_PATH=\"" + enginePath + "\" && ";
    envExports += "export DEKI_LOG_ENABLED=\"" + logEnabled + "\" && ";
    envExports += "export DEKI_LOG_INTERNAL_ENABLED=\"" + internalLogEnabled + "\" && ";

    if (ctx.hasPlatformConfig && ctx.platformConfig)
    {
        if (ctx.platformConfig->screenWidth > 0)
            envExports += "export DEKI_SCREEN_WIDTH=\"" + std::to_string(ctx.platformConfig->screenWidth) + "\" && ";
        if (ctx.platformConfig->screenHeight > 0)
            envExports += "export DEKI_SCREEN_HEIGHT=\"" + std::to_string(ctx.platformConfig->screenHeight) + "\" && ";
        bool usePsram = ctx.platformConfig->psramSize > 0;
        envExports += "export DEKI_USE_PSRAM=\"" + std::string(usePsram ? "1" : "") + "\" && ";
        if (!ctx.platformConfig->displayBus.empty())
            envExports += "export DEKI_DISPLAY_BUS=\"" + ctx.platformConfig->displayBus + "\" && ";
    }

    if (ctx.packageDefines && !ctx.packageDefines->empty())
    {
        // Same allowlist as the Windows branch: these land inside a shell line.
        std::string definesList;
        for (const auto& def : *ctx.packageDefines)
        {
            std::string reason;
            if (!SafeNames::IsSafeDefine(def, reason))
            {
                if (outputCallback) outputCallback("[Package Config] skipping define '" + def + "': " + reason, true);
                continue;
            }
            if (!definesList.empty()) definesList += ";";
            definesList += def;
        }
        envExports += "export DEKI_PACKAGE_DEFINES=\"" + definesList + "\" && ";
    }

    std::string fullCommand = "cd \"" + idfPath + "\" && . ./export.sh && cd \"" +
                              workDir + "\" && " + envExports + command;

    FILE* pipe = popen(fullCommand.c_str(), "r");
    if (!pipe)
    {
        return -1;
    }

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        if (ctx.cancelRequested)
        {
            pclose(pipe);
            return 1;
        }

        std::string line(buffer);
        if (!line.empty() && line.back() == '\n')
        {
            line.pop_back();
        }

        if (outputCallback && !line.empty())
        {
            bool isError = (line.find("error:") != std::string::npos) ||
                           (line.find("Error:") != std::string::npos) ||
                           (line.find("ERROR") != std::string::npos) ||
                           (line.find("fatal error") != std::string::npos) ||
                           (line.find("FAILED") != std::string::npos);
            outputCallback(line, isError);
        }
    }

    return pclose(pipe);
}

#endif

}  // namespace DekiEditor
