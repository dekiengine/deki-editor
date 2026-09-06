#include "CMakeGenUtils.h"
#include <deki/LogSystem.h>
#include <deki-editor/Paths.h>
#include "core/PlatformConfig.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace DekiEditor
{
namespace CMakeGen
{

// ============================================================================
// Package manifest scanning
// ============================================================================

// Scan a single packages directory for package.json manifests
static void ScanPackagesDir(const fs::path& packagesDir, std::vector<PackageEntry>& allPackages)
{
    if (!fs::exists(packagesDir))
        return;

    for (const auto& entry : fs::directory_iterator(packagesDir))
    {
        if (!entry.is_directory()) continue;
        fs::path manifestPath = entry.path() / "package.json";
        if (!fs::exists(manifestPath)) continue;

        try
        {
            std::ifstream mf(manifestPath);
            nlohmann::json j;
            mf >> j;

            PackageEntry info;
            info.dirName = entry.path().filename().string();
            info.buildDefine = j.value("buildDefine", "");
            info.id = j.value("id", "");

            // Collect dependencies from all platform dependency lists
            if (j.contains("dependencies"))
            {
                for (auto& [platform, deps] : j["dependencies"].items())
                {
                    if (!deps.is_array()) continue;
                    for (auto& dep : deps)
                    {
                        std::string name;
                        if (dep.is_string())
                            name = dep.get<std::string>();
                        else if (dep.is_object())
                            name = dep.value("name", "");

                        if (name.empty()) continue;

                        if (name.rfind("deki-", 0) == 0)
                            info.dekiDeps.push_back(name);
                        else if (platform == "espidf")
                            info.espidfDeps.push_back(name);
                    }
                }
            }

            // Parse package.cmake for PACKAGE_FILE_PREFIX and PACKAGE_SOURCES
            fs::path cmakePath = entry.path() / "package.cmake";
            if (fs::exists(cmakePath))
            {
                std::ifstream cmf(cmakePath);
                std::string cmakeContent((std::istreambuf_iterator<char>(cmf)),
                                         std::istreambuf_iterator<char>());

                // Extract PACKAGE_FILE_PREFIX
                {
                    std::string marker = "set(PACKAGE_FILE_PREFIX";
                    auto pos = cmakeContent.find(marker);
                    if (pos != std::string::npos)
                    {
                        pos += marker.length();
                        auto end = cmakeContent.find(")", pos);
                        if (end != std::string::npos)
                        {
                            std::string val = cmakeContent.substr(pos, end - pos);
                            val.erase(0, val.find_first_not_of(" \t\n\r\""));
                            val.erase(val.find_last_not_of(" \t\n\r\"") + 1);
                            info.filePrefix = val;
                        }
                    }
                }

                // Extract PACKAGE_PREFIX (e.g., "Deki2D")
                {
                    std::string marker = "set(PACKAGE_PREFIX";
                    auto pos = cmakeContent.find(marker);
                    if (pos != std::string::npos)
                    {
                        pos += marker.length();
                        auto end = cmakeContent.find(")", pos);
                        if (end != std::string::npos)
                        {
                            std::string val = cmakeContent.substr(pos, end - pos);
                            val.erase(0, val.find_first_not_of(" \t\n\r\""));
                            val.erase(val.find_last_not_of(" \t\n\r\"") + 1);
                            info.packagePrefix = val;
                        }
                    }
                }

                // Extract PACKAGE_HAS_SYSTEM_INIT
                {
                    if (cmakeContent.find("set(PACKAGE_HAS_SYSTEM_INIT ON)") != std::string::npos)
                        info.hasSystemInit = true;
                }

                // Extract PACKAGE_ENTRY filename for exclusion
                std::string entryFile;
                {
                    std::string marker = "set(PACKAGE_ENTRY";
                    auto pos = cmakeContent.find(marker);
                    if (pos != std::string::npos)
                    {
                        pos += marker.length();
                        auto end = cmakeContent.find(")", pos);
                        if (end != std::string::npos)
                        {
                            entryFile = cmakeContent.substr(pos, end - pos);
                            entryFile.erase(0, entryFile.find_first_not_of(" \t\n\r\""));
                            entryFile.erase(entryFile.find_last_not_of(" \t\n\r\"") + 1);
                        }
                    }
                }

                // Auto-discover source stems from *.cpp files in package directory
                {
                    for (auto& cppEntry : fs::recursive_directory_iterator(entry.path()))
                    {
                        if (!cppEntry.is_regular_file()) continue;
                        if (cppEntry.path().extension() != ".cpp") continue;
                        // Skip editor/, generated/ subdirectories and entry file
                        std::string rel = fs::relative(cppEntry.path(), entry.path()).generic_string();
                        if (rel.rfind("editor/", 0) == 0) continue;
                        if (rel.rfind("generated/", 0) == 0) continue;
                        if (rel.rfind("tests/", 0) == 0) continue;
                        if (!entryFile.empty() && cppEntry.path().filename().string() == entryFile) continue;
                        info.sourceStems.push_back(cppEntry.path().stem().string());
                    }
                }
            }

            allPackages.push_back(info);
        }
        catch (const std::exception& e)
        {
            DEKI_LOG_WARNING("CMakeGenUtils: package descriptor unreadable (%s); this package is left out of the build files", e.what());
        }
    }
}

std::vector<PackageEntry> ScanPackageManifests(const std::string& projectPath)
{
    std::vector<PackageEntry> allPackages;
    if (!projectPath.empty())
        ScanPackagesDir(fs::path(GetPackagesDirectory(projectPath)), allPackages);
    return allPackages;
}

// ============================================================================
// Active package resolution
// ============================================================================

std::set<std::string> ResolveActivePackages(const std::vector<PackageEntry>& allPackages,
                                            const std::vector<std::string>& packageDefines)
{
    // Seed with packages whose buildDefine appears in packageDefines
    std::set<std::string> activeIds;
    for (const auto& pkg : allPackages)
    {
        if (std::find(packageDefines.begin(), packageDefines.end(), pkg.buildDefine) != packageDefines.end())
            activeIds.insert(pkg.id);
    }

    // Resolve transitive deki:* dependencies (fixed-point loop)
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const auto& pkg : allPackages)
        {
            if (activeIds.count(pkg.id) == 0) continue;
            for (const auto& dep : pkg.dekiDeps)
            {
                if (activeIds.insert(dep).second)
                    changed = true;
            }
        }
    }

    return activeIds;
}

// ============================================================================
// CMake snippet emission
// ============================================================================

void EmitEngineCoreSourceCollection(std::ostream& out,
                                    const std::vector<std::string>& extraExclusions)
{
    out << "# Engine core sources (excluding packages, generated, desktop, editor)\n";
    out << "file(GLOB_RECURSE DEKI_ENGINE_SOURCES \"${DEKI_ENGINE_PATH}/src/*.cpp\")\n";
    out << "list(FILTER DEKI_ENGINE_SOURCES EXCLUDE REGEX \".*/packages/.*\")\n";
    out << "list(FILTER DEKI_ENGINE_SOURCES EXCLUDE REGEX \".*/generated/.*\")\n";
    out << "list(FILTER DEKI_ENGINE_SOURCES EXCLUDE REGEX \".*/platforms/desktop/.*\")\n";
    out << "list(FILTER DEKI_ENGINE_SOURCES EXCLUDE REGEX \".*/editor/.*\")\n";

    for (const auto& exclusion : extraExclusions)
    {
        out << "list(FILTER DEKI_ENGINE_SOURCES EXCLUDE REGEX \"" << exclusion << "\")\n";
    }

    out << "\n";
}

std::string CMakeIdentifier(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name)
    {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        out.push_back(ok ? static_cast<char>(c) : '_');
    }
    if (out.empty() || (out[0] >= '0' && out[0] <= '9'))
        out.insert(out.begin(), '_');
    return out;
}

std::string EscapeCppString(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string EscapeCMakeString(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '$': out += "\\$"; break;
            case ';': out += "\\;"; break;
            case '\n': out += "\\n"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

void EmitActivePackageSources(std::ostream& out,
                              const std::vector<PackageEntry>& allPackages,
                              const std::set<std::string>& activePackageIds)
{
    out << "# Active package sources (auto-detected from project configuration)\n";
    for (const auto& pkg : allPackages)
    {
        if (activePackageIds.count(pkg.id) == 0) continue;
        // The directory name is a real on-disk name (SafeNames-validated at
        // install), but as a CMake variable suffix it still gets identifier
        // characters only, the way string(MAKE_C_IDENTIFIER) would spell it.
        const std::string var = "DEKI_PKG_SOURCES_" + CMakeIdentifier(pkg.dirName);
        out << "file(GLOB_RECURSE " << var
            << " \"${DEKI_PROJECT_PATH}/packages/" << pkg.dirName << "/*.cpp\""
            << " \"${DEKI_PROJECT_PATH}/packages/" << pkg.dirName << "/*.c\")\n";
        out << "list(FILTER " << var << " EXCLUDE REGEX \".*/editor/.*\")\n";
        out << "list(FILTER " << var << " EXCLUDE REGEX \".*/tests/.*\")\n";
        out << "list(APPEND DEKI_ENGINE_SOURCES ${" << var << "})\n";
    }

    // Per-package include dirs (so generated/*.gen.cpp can find package headers)
    out << "\n# Per-package include directories\n";
    out << "set(DEKI_PACKAGE_INCLUDE_DIRS\n";
    for (const auto& pkg : allPackages)
    {
        if (activePackageIds.count(pkg.id) == 0) continue;
        out << "    \"${DEKI_PROJECT_PATH}/packages/" << pkg.dirName << "\"\n";
    }
    out << ")\n";
}

void EmitGeneratedFileSources(std::ostream& out,
                              const std::string& enginePath,
                              const std::vector<PackageEntry>& allPackages,
                              const std::set<std::string>& activePackageIds)
{
    // Build gen file stem -> package id mapping
    std::map<std::string, std::string> genStemToPackageId;
    for (const auto& pkg : allPackages)
    {
        if (!pkg.filePrefix.empty())
            genStemToPackageId[pkg.filePrefix + "Registration"] = pkg.id;
        for (const auto& stem : pkg.sourceStems)
            genStemToPackageId[stem] = pkg.id;
    }

    // Scan generated/ directory and selectively include gen files
    out << "\n# Generated files (core + active packages only)\n";
    fs::path genDir = fs::path(enginePath) / "include" / "deki" / "generated";
    if (fs::exists(genDir))
    {
        for (const auto& genEntry : fs::directory_iterator(genDir))
        {
            std::string filename = genEntry.path().filename().string();
            auto genPos = filename.find(".gen.cpp");
            if (genPos == std::string::npos) continue;

            // Skip ComponentFactory.gen.cpp (static initializers, not needed on firmware)
            if (filename == "ComponentFactory.gen.cpp") continue;
            // Skip CoreRegistration.gen.cpp (redundant with AllComponentRegistration)
            if (filename == "CoreRegistration.gen.cpp") continue;
            // Skip AllPackageInit.gen.cpp (deki_package_init.gen.cpp provides init/shutdown)
            if (filename == "AllPackageInit.gen.cpp") continue;

            // Always include AllComponentRegistration (provides deki_register_all_components)
            if (filename == "AllComponentRegistration.gen.cpp")
            {
                out << "list(APPEND DEKI_ENGINE_SOURCES \"${DEKI_ENGINE_PATH}/include/deki/generated/"
                    << filename << "\")\n";
                continue;
            }

            std::string stem = filename.substr(0, genPos);

            auto it = genStemToPackageId.find(stem);
            if (it != genStemToPackageId.end())
            {
                // Belongs to a package - only include if package is active
                if (activePackageIds.count(it->second) > 0)
                {
                    // Skip per-package Registration files (not called by Deki::Engine)
                    if (stem.size() >= 12 &&
                        stem.compare(stem.size() - 12, 12, "Registration") == 0)
                        continue;
                    out << "list(APPEND DEKI_ENGINE_SOURCES \"${DEKI_ENGINE_PATH}/include/deki/generated/"
                        << filename << "\")\n";
                }
            }
            else
            {
                // Core gen file (not tied to any package) - always include
                out << "list(APPEND DEKI_ENGINE_SOURCES \"${DEKI_ENGINE_PATH}/include/deki/generated/"
                    << filename << "\")\n";
            }
        }
    }
}

void EmitProjectSourceCollection(std::ostream& out,
                                 const std::string& relPath,
                                 bool excludePluginExports)
{
    out << "\n";
    out << "# Project sources\n";
    out << "file(GLOB_RECURSE PROJECT_SOURCES\n";
    out << "    \"" << relPath << "/*.cpp\"\n";
    out << "    \"" << relPath << "/*.c\"\n";
    out << ")\n";

    if (excludePluginExports)
    {
        out << "list(FILTER PROJECT_SOURCES EXCLUDE REGEX \".*PluginExports\\\\.cpp$\")\n";
    }
}

void EmitPlatformDefines(std::ostream& out,
                         const std::string& targetName,
                         const PlatformConfig& config,
                         const std::vector<std::string>& packageDefines,
                         const std::vector<std::string>& skipDefines)
{
    out << "\n";
    out << "# Platform defines (" << config.displayName << ")\n";
    out << "target_compile_definitions(" << targetName << " PRIVATE\n";

    // Platform defines from PlatformConfig
    for (const auto& define : config.defines)
    {
        out << "    " << define << "\n";
    }

    // Screen dimensions
    if (config.screenWidth > 0)
        out << "    DEKI_SCREEN_WIDTH=" << config.screenWidth << "\n";
    if (config.screenHeight > 0)
        out << "    DEKI_SCREEN_HEIGHT=" << config.screenHeight << "\n";

    // Color format (unless already in defines)
    bool hasColorFmt = false;
    for (const auto& d : config.defines)
    {
        if (d.find("DEKI_DEFAULT_COLOR_FORMAT") != std::string::npos)
        {
            hasColorFmt = true;
            break;
        }
    }
    if (!hasColorFmt)
    {
        std::string fmt = config.colorFormat.empty() ? "RGB565" : config.colorFormat;
        out << "    DEKI_DEFAULT_COLOR_FORMAT=Deki::ColorFormat::" << fmt << "\n";
    }

    // Package feature defines (skip any in skipDefines)
    std::set<std::string> skipSet(skipDefines.begin(), skipDefines.end());
    out << "    DEKI_PACKAGE_FEATURES_CONFIGURED\n";
    for (const auto& define : packageDefines)
    {
        if (skipSet.count(define) > 0) continue;
        out << "    " << define << "\n";
    }

    out << ")\n";
}

void EmitIncludePaths(std::ostream& out,
                      const std::string& targetName,
                      const std::string& projectSrcRelPath,
                      const std::vector<std::string>& extraPaths)
{
    out << "\n";
    out << "target_include_directories(" << targetName << " PRIVATE\n";
    // include/ is the public API; src/ is needed here because this target compiles
    // the engine's own sources, which reach its internal headers.
    out << "    \"${DEKI_ENGINE_PATH}/include\"\n";
    out << "    \"${DEKI_ENGINE_PATH}/src\"\n";
    out << "    \"${DEKI_ENGINE_PATH}/third_party\"\n";
    out << "    \"${DEKI_PROJECT_PATH}/packages\"\n";
    out << "    ${DEKI_PACKAGE_INCLUDE_DIRS}\n";
    out << "    \"" << projectSrcRelPath << "\"\n";

    for (const auto& path : extraPaths)
    {
        out << "    " << path << "\n";
    }

    out << ")\n";
}

// ============================================================================
// File utility
// ============================================================================

bool WriteIfChanged(const fs::path& filePath, const std::string& content)
{
    if (fs::exists(filePath))
    {
        std::ifstream existing(filePath, std::ios::binary);
        std::string existingContent((std::istreambuf_iterator<char>(existing)),
                                    std::istreambuf_iterator<char>());
        if (existingContent == content)
            return true;
    }
    std::ofstream file(filePath, std::ios::binary);
    if (!file.is_open())
        return false;
    file << content;
    return true;
}

std::string GeneratePackageInitFile(const fs::path& outputDir,
                                    const std::vector<PackageEntry>& allPackages,
                                    const std::set<std::string>& activePackageIds,
                                    const fs::path& projectSrcPath,
                                    bool engineDefinesSystemInit)
{
    std::ostringstream cpp;
    cpp << "// AUTO-GENERATED by Deki Editor — calls package registration for embedded builds\n";
    cpp << "#include <deki/reflection/ComponentFactory.h>\n";

    // Scan project src/ for game components (DEKI_COMPONENT in headers)
    std::vector<std::string> projectComponents;
    if (!projectSrcPath.empty() && fs::exists(projectSrcPath))
    {
        try
        {
            for (const auto& entry : fs::recursive_directory_iterator(projectSrcPath))
            {
                if (!entry.is_regular_file()) continue;
                auto ext = entry.path().extension().string();
                if (ext != ".h" && ext != ".hpp") continue;
                if (entry.path().string().find("generated") != std::string::npos) continue;

                std::ifstream headerFile(entry.path());
                if (!headerFile.is_open()) continue;

                std::string line;
                while (std::getline(headerFile, line))
                {
                    size_t firstNonSpace = line.find_first_not_of(" \t");
                    if (firstNonSpace != std::string::npos && line.substr(firstNonSpace, 2) == "//") continue;
                    if (firstNonSpace != std::string::npos && line.substr(firstNonSpace, 7) == "#define") continue;

                    size_t pos = line.find("DEKI_COMPONENT(");
                    if (pos != std::string::npos)
                    {
                        pos += 15;  // strlen("DEKI_COMPONENT(")
                        size_t endPos = line.find(',', pos);
                        if (endPos != std::string::npos)
                        {
                            std::string className = line.substr(pos, endPos - pos);
                            while (!className.empty() && (className.front() == ' ' || className.front() == '\t'))
                                className.erase(0, 1);
                            while (!className.empty() && (className.back() == ' ' || className.back() == '\t'))
                                className.pop_back();
                            projectComponents.push_back(className);
                            break;
                        }
                    }
                }
            }
        }
        catch (const fs::filesystem_error&)
        {
        }

        std::sort(projectComponents.begin(), projectComponents.end());
    }

    // Include project component headers
    if (!projectComponents.empty())
    {
        for (const auto& cls : projectComponents)
            cpp << "#include \"" << cls << ".h\"\n";
    }
    cpp << "\n";

    // Declare extern registration functions (using PACKAGE_PREFIX from package.cmake)
    for (const auto& pkg : allPackages)
    {
        if (activePackageIds.count(pkg.id) == 0) continue;
        if (pkg.packagePrefix.empty()) continue;
        cpp << "extern void " << pkg.packagePrefix << "_RegisterComponents();\n";
    }

    // Collect packages with system init. Declared up front so deki_register_project_packages()
    // can call them inline when the engine core already provides deki_init_package_systems().
    std::vector<const PackageEntry*> initPackages;
    for (const auto& pkg : allPackages)
    {
        if (activePackageIds.count(pkg.id) == 0) continue;
        if (!pkg.hasSystemInit || pkg.packagePrefix.empty()) continue;
        initPackages.push_back(&pkg);
    }

    // Declare init/shutdown externs
    for (const auto* pkg : initPackages)
    {
        cpp << "extern void " << pkg->packagePrefix << "_InitSystem();\n";
        cpp << "extern void " << pkg->packagePrefix << "_ShutdownSystem();\n";
    }

    cpp << "\nextern \"C\" void deki_register_project_packages(void)\n";
    cpp << "{\n";
    for (const auto& pkg : allPackages)
    {
        if (activePackageIds.count(pkg.id) == 0) continue;
        if (pkg.packagePrefix.empty()) continue;
        cpp << "    " << pkg.packagePrefix << "_RegisterComponents();\n";
    }

    // Static desktop simulator: the engine core defines (an empty) deki_init_package_systems(),
    // so this file must not redefine it (link collision). Bootstrap package systems here
    // instead, after component registration. Same order as deki_init_package_systems() below.
    if (engineDefinesSystemInit && !initPackages.empty())
    {
        cpp << "\n    // Package systems (engine core owns the deki_init_package_systems() symbol)\n";
        for (const auto* pkg : initPackages)
            cpp << "    " << pkg->packagePrefix << "_InitSystem();\n";
    }

    // Register project components with ComponentFactory
    if (!projectComponents.empty())
    {
        cpp << "\n    // Project game components\n";
        cpp << "    auto& factory = ::Deki::ComponentFactory::Instance();\n";
        for (const auto& cls : projectComponents)
            cpp << "    factory.Register(" << cls << "::StaticType, []() -> ::Deki::Component* { return new " << cls << "(); });\n";
    }

    cpp << "}\n";

    // Firmware builds: the engine excludes the empty stub, so define the system
    // init/shutdown entry points here (called by Deki::Engine::Initialize/Shutdown).
    if (!engineDefinesSystemInit)
    {
        // deki_init_package_systems
        cpp << "\nextern \"C\" void deki_init_package_systems(void)\n";
        cpp << "{\n";
        for (const auto* pkg : initPackages)
            cpp << "    " << pkg->packagePrefix << "_InitSystem();\n";
        cpp << "}\n";

        // deki_shutdown_package_systems (reverse order)
        cpp << "\nextern \"C\" void deki_shutdown_package_systems(void)\n";
        cpp << "{\n";
        for (auto it = initPackages.rbegin(); it != initPackages.rend(); ++it)
            cpp << "    " << (*it)->packagePrefix << "_ShutdownSystem();\n";
        cpp << "}\n";
    }

    fs::path outPath = outputDir / "deki_package_init.gen.cpp";
    WriteIfChanged(outPath, cpp.str());
    return outPath.string();
}

}  // namespace CMakeGen
}  // namespace DekiEditor
