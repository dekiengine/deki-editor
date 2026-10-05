#include "Project.h"
#include <deki-editor/build/ProjectPaths.h>
#include <deki-editor/VersionCompare.h>
#include <deki-editor/Paths.h>
#include <deki-editor/FileIO.h>
#include <deki-editor/build/PlatformConfig.h>
#include "SettingsOwnership.h"
#include <deki/LogSystem.h>
#include <deki/Engine.h>
#include <deki/reflection/SettingsRegistry.h>
#include <set>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/Property.h>
#include <deki/settings/UnitProjectSettings.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <map>

namespace DekiEditor
{

Deki::UnitProjectSettings* GetUnitSettings()
{
    return Deki::SettingsRegistry::Instance().Get<Deki::UnitProjectSettings>();
}

Deki::CoreProjectSettings* GetCoreSettings()
{
    return Deki::SettingsRegistry::Instance().Get<Deki::CoreProjectSettings>();
}

void ApplySettingsToGlobalCache()
{
    if (auto* units = GetUnitSettings())
    {
        units->ApplyToGlobalCache();
    }
}

namespace
{
// Reads and writes one settings property as JSON. Settings use only bool,
// integer, float, enum and string properties, so this handles fewer types than
// Scene::SavePropertyToJson (no arrays, asset or object refs, or colours).

void SaveSettingProperty(const Deki::Component* comp, const Deki::PropertyInfo& prop, nlohmann::json& out)
{
    switch (prop.type)
    {
        case Deki::PropertyType::Bool: out[prop.name] = Deki::GetPropertyValue<bool>(comp, prop); break;
        case Deki::PropertyType::Int8: out[prop.name] = (int32_t)Deki::GetPropertyValue<int8_t>(comp, prop); break;
        case Deki::PropertyType::Int16: out[prop.name] = (int32_t)Deki::GetPropertyValue<int16_t>(comp, prop); break;
        case Deki::PropertyType::Int32: out[prop.name] = Deki::GetPropertyValue<int32_t>(comp, prop); break;
        case Deki::PropertyType::UInt8: out[prop.name] = (int32_t)Deki::GetPropertyValue<uint8_t>(comp, prop); break;
        case Deki::PropertyType::UInt16: out[prop.name] = (int32_t)Deki::GetPropertyValue<uint16_t>(comp, prop); break;
        case Deki::PropertyType::UInt32: out[prop.name] = Deki::GetPropertyValue<uint32_t>(comp, prop); break;
        case Deki::PropertyType::Float: out[prop.name] = Deki::GetPropertyValue<float>(comp, prop); break;
        case Deki::PropertyType::String: out[prop.name] = Deki::GetPropertyValue<std::string>(comp, prop); break;
        case Deki::PropertyType::Enum:
            switch (prop.enumSize)
            {
                case 1: out[prop.name] = (int32_t)Deki::GetPropertyValue<uint8_t>(comp, prop); break;
                case 2: out[prop.name] = (int32_t)Deki::GetPropertyValue<uint16_t>(comp, prop); break;
                default: out[prop.name] = Deki::GetPropertyValue<int32_t>(comp, prop); break;
            }
            break;
        default:
            // Other types are skipped; the field keeps its compiled default on
            // the next load.
            break;
    }
}

void LoadSettingProperty(Deki::Component* comp, const Deki::PropertyInfo& prop, const nlohmann::json& in)
{
    if (!in.contains(prop.name))
    {
        return;
    }
    const auto& v = in[prop.name];
    switch (prop.type)
    {
        case Deki::PropertyType::Bool:
            if (v.is_boolean())
            {
                Deki::SetPropertyValue<bool>(comp, prop, v.get<bool>());
            }
            break;
        case Deki::PropertyType::Int8:
            if (v.is_number_integer())
            {
                Deki::SetPropertyValue<int8_t>(comp, prop, (int8_t)v.get<int32_t>());
            }
            break;
        case Deki::PropertyType::Int16:
            if (v.is_number_integer())
            {
                Deki::SetPropertyValue<int16_t>(comp, prop, (int16_t)v.get<int32_t>());
            }
            break;
        case Deki::PropertyType::Int32:
            if (v.is_number_integer())
            {
                Deki::SetPropertyValue<int32_t>(comp, prop, v.get<int32_t>());
            }
            break;
        case Deki::PropertyType::UInt8:
            if (v.is_number_integer())
            {
                Deki::SetPropertyValue<uint8_t>(comp, prop, (uint8_t)v.get<int32_t>());
            }
            break;
        case Deki::PropertyType::UInt16:
            if (v.is_number_integer())
            {
                Deki::SetPropertyValue<uint16_t>(comp, prop, (uint16_t)v.get<int32_t>());
            }
            break;
        case Deki::PropertyType::UInt32:
            if (v.is_number_unsigned())
            {
                Deki::SetPropertyValue<uint32_t>(comp, prop, v.get<uint32_t>());
            }
            else if (v.is_number_integer())
            {
                Deki::SetPropertyValue<uint32_t>(comp, prop, (uint32_t)v.get<int32_t>());
            }
            break;
        case Deki::PropertyType::Float:
            if (v.is_number())
            {
                Deki::SetPropertyValue<float>(comp, prop, v.get<float>());
            }
            break;
        case Deki::PropertyType::String:
            if (v.is_string())
            {
                Deki::SetPropertyValue<std::string>(comp, prop, v.get<std::string>());
            }
            break;
        case Deki::PropertyType::Enum:
            if (v.is_number_integer())
            {
                int32_t intVal = v.get<int32_t>();
                switch (prop.enumSize)
                {
                    case 1: Deki::SetPropertyValue<uint8_t>(comp, prop, (uint8_t)intVal); break;
                    case 2: Deki::SetPropertyValue<uint16_t>(comp, prop, (uint16_t)intVal); break;
                    default: Deki::SetPropertyValue<int32_t>(comp, prop, intVal); break;
                }
            }
            break;
        default: break;
    }
}
}  // namespace

namespace
{
// Where a settings section is stored.
//
// Package-owned sections get a folder per package, so uninstalling one takes
// its settings with it. Engine-core sections share core.json: they are few,
// small and always present.
std::filesystem::path SectionFilePath(const std::string& root, const std::string& section)
{
    std::string lower = section;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const std::string owner = DekiEditor::SettingsOwnership::OwnerOf(section);
    if (!owner.empty())
    {
        return DekiEditor::ProjectPaths::Settings(root) / "packages" / owner / (lower + ".json");
    }

    return DekiEditor::ProjectPaths::Settings(root) / "core.json";
}

// A settings file, or an empty object when there is none. `unreadable` is set
// when the file is there but is not a JSON object (a merge conflict): its
// sections then load as defaults, and saving must not write over it.
nlohmann::json ReadJsonFile(const std::filesystem::path& p, bool* unreadable = nullptr)
{
    if (unreadable)
    {
        *unreadable = false;
    }
    std::ifstream in(p);
    if (!in.is_open())
    {
        return nlohmann::json::object();
    }
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_object())
    {
        return j;
    }
    if (unreadable)
    {
        *unreadable = true;
    }
    DEKI_LOG_ERROR("Project settings: %s could not be read (a merge conflict?). Its settings are at their "
                   "defaults until it is fixed, and saving leaves it as it is.",
                   p.string().c_str());
    return nlohmann::json::object();
}
}  // namespace

std::vector<std::string> Project::SaveSettingsFiles(const std::string& root) const
{
    std::vector<std::string> written;
    namespace fs = std::filesystem;

    // Group by destination file, then merge into what is already there. This
    // keeps sections of packages that are not loaded; an absent package's file
    // is never even opened.
    std::map<fs::path, nlohmann::json> byFile;
    std::set<fs::path> unreadable;  // there but not JSON: never written over

    for (const auto& info : Deki::SettingsRegistry::Instance().GetAll())
    {
        if (info.meta == nullptr || info.sectionName == nullptr || info.instance == nullptr)
        {
            continue;
        }

        const fs::path dest = SectionFilePath(root, info.sectionName);
        auto it = byFile.find(dest);
        if (it == byFile.end())
        {
            bool bad = false;
            it = byFile.emplace(dest, ReadJsonFile(dest, &bad)).first;
            if (bad)
            {
                unreadable.insert(dest);
            }
        }
        if (unreadable.count(dest))
        {
            continue;
        }

        nlohmann::json sectionJson = nlohmann::json::object();
        for (int i = 0; i < info.meta->propertyCount; ++i)
        {
            SaveSettingProperty(info.instance, info.meta->properties[i], sectionJson);
        }

        it->second[info.sectionName] = sectionJson;
        written.push_back(info.sectionName);
    }

    for (const auto& [dest, contents] : byFile)
    {
        if (unreadable.count(dest))
        {
            continue;  // rewriting it from defaults would erase what the conflict holds
        }
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        std::string err;
        if (!DekiEditor::AtomicWriteFile(dest.string(), contents.dump(2) + "\n", err))
        {
            DEKI_LOG_ERROR("Project settings: could not save %s: %s", dest.string().c_str(), err.c_str());
        }
    }

    return written;
}

namespace
{
// The old Framebuffer section, settings/framebuffer.json, which held how much
// of the world a project showed before cameras framed it themselves. Nothing
// writes it. It stays on
// disk because scenes migrate their cameras when they load, which can be long
// after the project was first opened.
//
//   0.16  targetWidth/targetHeight: the render size, pixels
//   0.18  designWidth/designHeight: the design area, meters; pixelPerfect
nlohmann::json ReadLegacyFramebuffer(const std::string& root)
{
    const nlohmann::json file = ReadJsonFile(DekiEditor::ProjectPaths::Settings(root) / "framebuffer.json");
    if (file.contains("Framebuffer") && file["Framebuffer"].is_object())
    {
        return file["Framebuffer"];
    }
    return nlohmann::json();
}

float LegacyNumber(const nlohmann::json& j, const char* key)
{
    return (j.is_object() && j.contains(key) && j[key].is_number()) ? j[key].get<float>() : 0.0f;
}
}  // namespace

void Project::LoadSettingsFiles(const std::string& root)
{
    for (const auto& info : Deki::SettingsRegistry::Instance().GetAll())
    {
        if (info.meta == nullptr || info.sectionName == nullptr || info.instance == nullptr)
        {
            continue;
        }

        const nlohmann::json fileJson = ReadJsonFile(SectionFilePath(root, info.sectionName));
        if (!fileJson.contains(info.sectionName))
        {
            continue;
        }

        const nlohmann::json& sectionJson = fileJson[info.sectionName];
        if (!sectionJson.is_object())
        {
            continue;
        }

        for (int i = 0; i < info.meta->propertyCount; ++i)
        {
            LoadSettingProperty(info.instance, info.meta->properties[i], sectionJson);
        }
    }
}

void ActivePlatformScreen(const Project* project, int& width, int& height)
{
    width = 320;
    height = 240;
    if (project == nullptr)
    {
        return;
    }
    const PlatformConfig* platform = PlatformRegistry::Instance().GetPlatform(project->GetActivePlatformId());
    if (platform != nullptr && platform->screenWidth > 0 && platform->screenHeight > 0)
    {
        width = platform->screenWidth;
        height = platform->screenHeight;
    }
}

void Project::LoadLegacyFraming(const nlohmann::json& legacyFramebuffer)
{
    // From the old Framebuffer section, for scenes whose cameras migrate from
    // it: its height in meters and its Pixel Perfect flag.
    const float ppm = Deki::EngineSettings::Global().pixelsPerMeter;
    SceneView::LegacyFraming legacy;
    if (const float h = LegacyNumber(legacyFramebuffer, "designHeight"); h > 0.0f)
    {
        legacy.designHeight = h;
    }
    else if (const float px = LegacyNumber(legacyFramebuffer, "targetHeight"); px > 0.0f && ppm > 0.0f)
    {
        legacy.designHeight = px / ppm;
    }
    if (legacyFramebuffer.is_object() && legacyFramebuffer.contains("pixelPerfect") &&
        legacyFramebuffer["pixelPerfect"].is_boolean())
    {
        legacy.pixelPerfect = legacyFramebuffer["pixelPerfect"].get<bool>();
    }
    SceneView::Get().SetLegacyFraming(legacy);
}

bool Project::Load(const std::string& path)
{
    namespace fs = std::filesystem;

    // Path can be either the project directory or its deki.json.
    fs::path projectPath = path;

    if (fs::is_regular_file(projectPath))
    {
        projectPath = projectPath.parent_path();
    }

    fs::path dekiJsonPath = projectPath / "deki.json";
    if (!fs::exists(dekiJsonPath))
    {
        return false;
    }

    std::ifstream file(dekiJsonPath);
    if (!file.is_open())
    {
        return false;
    }

    try
    {
        nlohmann::json json;
        file >> json;

        m_Name = json.value("name", "Unnamed Project");
        // Stored absolute: every derived path is built from it, and package
        // loading rejects relative paths.
        m_Path = NormalizeProjectRoot(projectPath.string());
        projectPath = m_Path;

        // Refuse a project last saved by a newer editor: its scenes may use a
        // format this build does not understand, and the next save would
        // rewrite them in the older format, losing what the newer editor
        // stored. Older projects are always fine; the migrations below handle
        // them.
        //
        // Reads "lastSavedBy", never "engineVersion": that one was set once
        // when the project was created and can hold a literal "1.0.0" that
        // outranks every real version. A missing "lastSavedBy" means an older
        // editor, which is allowed.
        const std::string wroteWith = json.value("lastSavedBy", "");
        if (ProjectNeedsNewerEditor(wroteWith, DEKI_ENGINE_VERSION))
        {
            m_LoadError = "This project was last saved by Deki " + wroteWith + "; this editor is " +
                          DEKI_ENGINE_VERSION +
                          ". Update the editor to open it. Opening it here would "
                          "write its scenes back in the older format.";
            DEKI_LOG_ERROR("Project: %s", m_LoadError.c_str());
            return false;
        }

        fs::path assetsPath = GetAssetsDirectory(m_Path);
        fs::path srcPath = GetSourceDirectory(m_Path);

        if (!fs::exists(assetsPath))
        {
            DEKI_LOG_ERROR("Project: no assets directory at '%s' - not a Deki project", assetsPath.string().c_str());
            return false;
        }

        m_AssetsPath = assetsPath.string();
        m_SrcPath = srcPath.string();

        m_BuildPath = ProjectPaths::Build(m_Path).string();
        m_CachePath = ProjectPaths::Cache(m_Path).string();

        LoadSettingsFiles(m_Path);
        ApplySettingsToGlobalCache();  // the old framing converts at the art density

        LoadLegacyFraming(ReadLegacyFramebuffer(m_Path));

        // Through SetStartupScene, which turns backslashes into forward
        // slashes: the runtime's asset lookup only matches forward slashes, and
        // older files may hold backslashes.
        SetStartupScene(json.value("startupScene", ""));

        m_RenderPipelineGuid = json.value("renderPipelineGuid", "");

        m_ActivePlatformId = json.value("activePlatformId", "");

        m_ForceIncludeGuids.clear();
        if (json.contains("forceIncludeGuids") && json["forceIncludeGuids"].is_array())
        {
            for (const auto& guid : json["forceIncludeGuids"])
            {
                m_ForceIncludeGuids.insert(guid.get<std::string>());
            }
        }

        // A freshly loaded project is clean. Must stay the last assignment:
        // setters above call MarkDirty.
        m_IsDirty = false;
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool Project::Save()
{
    namespace fs = std::filesystem;

    if (m_Path.empty())
    {
        return false;
    }

    fs::path dekiJsonPath = fs::path(m_Path) / "deki.json";

    // Start from the existing file so keys this class does not own are kept.
    nlohmann::json json;
    {
        std::ifstream file(dekiJsonPath);
        if (file.is_open())
        {
            try
            {
                file >> json;
            }
            catch (const std::exception& e)
            {
                // Warned because a malformed deki.json leaves every field at
                // its default, which otherwise looks like an empty project.
                DEKI_LOG_WARNING("Project: could not parse %s (%s); using defaults", dekiJsonPath.string().c_str(),
                                 e.what());
            }
        }
    }

    // Settings live under settings/, not in deki.json.
    SaveSettingsFiles(m_Path);

    // Which editor wrote this, updated on every save; Load refuses a project
    // from a newer one. "engineVersion" is kept up to date too because the
    // ESP-IDF toolchain and the doc generator read it, but Load checks
    // "lastSavedBy" alone.
    json["lastSavedBy"] = DEKI_ENGINE_VERSION;
    json["engineVersion"] = DEKI_ENGINE_VERSION;

    // The folder layout this project uses.
    json["structure"] = ProjectPaths::kStructureVersion;

    json["startupScene"] = m_StartupScene;

    json["renderPipelineGuid"] = m_RenderPipelineGuid;

    json["activePlatformId"] = m_ActivePlatformId;

    json["forceIncludeGuids"] = nlohmann::json::array();
    for (const auto& guid : m_ForceIncludeGuids)
    {
        json["forceIncludeGuids"].push_back(guid);
    }

    // Obsolete: where assets are kept is set per target
    // (PlatformConfig::assetStorage), and no build read this list.
    json.erase("flashGuids");

    // Old copies of settings that now live in settings/, dropped so the two
    // cannot disagree. SaveSettingsFiles above has already written them.
    json.erase("displaySettings");
    json.erase("profilingSettings");
    json.erase("unitSettings");

    // Nothing reads it, so it is dropped rather than migrated.
    json.erase("assetSettings");

    // Atomic, so a crash mid-write cannot leave a truncated deki.json, the one
    // file the editor needs to open the project.
    const std::string outPath = std::filesystem::path(dekiJsonPath).string();
    std::string err;
    if (!AtomicWriteFile(outPath, json.dump(2), err))
    {
        DEKI_LOG_ERROR("Project: failed to write %s: %s", outPath.c_str(), err.c_str());
        return false;
    }
    m_IsDirty = false;
    return true;
}

}  // namespace DekiEditor
