#include "Project.h"
#include <deki-editor/build/ProjectPaths.h>
#include <deki-editor/VersionCompare.h>
#include <deki-editor/Paths.h>
#include <deki-editor/FileIO.h>
#include "SettingsOwnership.h"
#include <deki/LogSystem.h>
#include <deki/Engine.h>
#include <deki/reflection/SettingsRegistry.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/Property.h>
#include <deki/settings/DisplayProjectSettings.h>
#include <deki/settings/UnitProjectSettings.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <map>

namespace DekiEditor
{

Deki::DisplayProjectSettings* GetDisplaySettings()
{
    return Deki::SettingsRegistry::Instance().Get<Deki::DisplayProjectSettings>();
}

Deki::UnitProjectSettings* GetUnitSettings()
{
    return Deki::SettingsRegistry::Instance().Get<Deki::UnitProjectSettings>();
}

Deki::CoreProjectSettings* GetCoreSettings()
{
    return Deki::SettingsRegistry::Instance().Get<Deki::CoreProjectSettings>();
}

namespace
{
// Per-property JSON round-trip for settings types. Settings declare a
// constrained property set (bool / int / float / enum / string), so this
// is intentionally narrower than Scene::SavePropertyToJson — no arrays,
// no asset/object refs, no colors. Adding more types here is mechanical.

void SaveSettingProperty(const Deki::Component* comp, const Deki::PropertyInfo& prop, nlohmann::json& out)
{
    switch (prop.type)
    {
        case Deki::PropertyType::Bool:
            out[prop.name] = Deki::GetPropertyValue<bool>(comp, prop);
            break;
        case Deki::PropertyType::Int8:
            out[prop.name] = (int32_t)Deki::GetPropertyValue<int8_t>(comp, prop);
            break;
        case Deki::PropertyType::Int16:
            out[prop.name] = (int32_t)Deki::GetPropertyValue<int16_t>(comp, prop);
            break;
        case Deki::PropertyType::Int32:
            out[prop.name] = Deki::GetPropertyValue<int32_t>(comp, prop);
            break;
        case Deki::PropertyType::UInt8:
            out[prop.name] = (int32_t)Deki::GetPropertyValue<uint8_t>(comp, prop);
            break;
        case Deki::PropertyType::UInt16:
            out[prop.name] = (int32_t)Deki::GetPropertyValue<uint16_t>(comp, prop);
            break;
        case Deki::PropertyType::UInt32:
            out[prop.name] = Deki::GetPropertyValue<uint32_t>(comp, prop);
            break;
        case Deki::PropertyType::Float:
            out[prop.name] = Deki::GetPropertyValue<float>(comp, prop);
            break;
        case Deki::PropertyType::String:
            out[prop.name] = Deki::GetPropertyValue<std::string>(comp, prop);
            break;
        case Deki::PropertyType::Enum:
            switch (prop.enumSize)
            {
                case 1: out[prop.name] = (int32_t)Deki::GetPropertyValue<uint8_t>(comp, prop); break;
                case 2: out[prop.name] = (int32_t)Deki::GetPropertyValue<uint16_t>(comp, prop); break;
                default: out[prop.name] = Deki::GetPropertyValue<int32_t>(comp, prop); break;
            }
            break;
        default:
            // Settings shouldn't use other types yet. Silently skip; the field
            // keeps its compiled default on next load.
            break;
    }
}

void LoadSettingProperty(Deki::Component* comp, const Deki::PropertyInfo& prop, const nlohmann::json& in)
{
    if (!in.contains(prop.name)) return;
    const auto& v = in[prop.name];
    switch (prop.type)
    {
        case Deki::PropertyType::Bool:
            if (v.is_boolean()) Deki::SetPropertyValue<bool>(comp, prop, v.get<bool>());
            break;
        case Deki::PropertyType::Int8:
            if (v.is_number_integer()) Deki::SetPropertyValue<int8_t>(comp, prop, (int8_t)v.get<int32_t>());
            break;
        case Deki::PropertyType::Int16:
            if (v.is_number_integer()) Deki::SetPropertyValue<int16_t>(comp, prop, (int16_t)v.get<int32_t>());
            break;
        case Deki::PropertyType::Int32:
            if (v.is_number_integer()) Deki::SetPropertyValue<int32_t>(comp, prop, v.get<int32_t>());
            break;
        case Deki::PropertyType::UInt8:
            if (v.is_number_integer()) Deki::SetPropertyValue<uint8_t>(comp, prop, (uint8_t)v.get<int32_t>());
            break;
        case Deki::PropertyType::UInt16:
            if (v.is_number_integer()) Deki::SetPropertyValue<uint16_t>(comp, prop, (uint16_t)v.get<int32_t>());
            break;
        case Deki::PropertyType::UInt32:
            if (v.is_number_unsigned())
                Deki::SetPropertyValue<uint32_t>(comp, prop, v.get<uint32_t>());
            else if (v.is_number_integer())
                Deki::SetPropertyValue<uint32_t>(comp, prop, (uint32_t)v.get<int32_t>());
            break;
        case Deki::PropertyType::Float:
            if (v.is_number()) Deki::SetPropertyValue<float>(comp, prop, v.get<float>());
            break;
        case Deki::PropertyType::String:
            if (v.is_string()) Deki::SetPropertyValue<std::string>(comp, prop, v.get<std::string>());
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
        default:
            break;
    }
}
}  // namespace

namespace
{
// Where a settings section is stored.
//
// Package-owned sections get a folder per package so uninstalling one takes its
// settings with it. Engine-core sections share core.json — they are few, small
// and always present, so a file each would be noise. The framebuffer is the
// exception: it is the device contract also written to dproject.bin and the
// one most likely to be edited per-platform, so it stays addressable on its own.
std::filesystem::path SectionFilePath(const std::string& root, const std::string& section)
{
    std::string lower = section;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });

    const std::string owner = DekiEditor::SettingsOwnership::OwnerOf(section);
    if (!owner.empty())
        return DekiEditor::ProjectPaths::Settings(root) / "packages" / owner / (lower + ".json");

    if (lower == "framebuffer" || lower == "display")
        return DekiEditor::ProjectPaths::Settings(root) / "framebuffer.json";

    return DekiEditor::ProjectPaths::Settings(root) / "core.json";
}

nlohmann::json ReadJsonFile(const std::filesystem::path& p)
{
    std::ifstream in(p);
    if (!in.is_open()) return nlohmann::json::object();
    try
    {
        nlohmann::json j;
        in >> j;
        return j.is_object() ? j : nlohmann::json::object();
    }
    catch (...)
    {
        return nlohmann::json::object();
    }
}
}  // namespace

std::vector<std::string> Project::SaveSettingsFiles(const std::string& root) const
{
    std::vector<std::string> written;
    namespace fs = std::filesystem;

    // Group by destination file, then merge into whatever is already there. The
    // read-then-merge keeps sections belonging to packages that are not currently
    // loaded — the same guarantee the single packageSettings block used to give,
    // and stronger here, since an absent package's file is never even opened.
    std::map<fs::path, nlohmann::json> byFile;

    for (const auto& info : Deki::SettingsRegistry::Instance().GetAll())
    {
        if (info.meta == nullptr || info.sectionName == nullptr || info.instance == nullptr)
            continue;

        const fs::path dest = SectionFilePath(root, info.sectionName);
        auto it = byFile.find(dest);
        if (it == byFile.end())
            it = byFile.emplace(dest, ReadJsonFile(dest)).first;

        nlohmann::json sectionJson = nlohmann::json::object();
        for (int i = 0; i < info.meta->propertyCount; ++i)
            SaveSettingProperty(info.instance, info.meta->properties[i], sectionJson);

        it->second[info.sectionName] = sectionJson;
        written.push_back(info.sectionName);
    }

    for (const auto& [dest, contents] : byFile)
    {
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        std::ofstream out(dest);
        if (out.is_open())
            out << contents.dump(2) << "\n";
    }

    return written;
}

void Project::LoadSettingsFiles(const std::string& root)
{
    for (const auto& info : Deki::SettingsRegistry::Instance().GetAll())
    {
        if (info.meta == nullptr || info.sectionName == nullptr || info.instance == nullptr)
            continue;

        const nlohmann::json fileJson = ReadJsonFile(SectionFilePath(root, info.sectionName));
        if (!fileJson.contains(info.sectionName))
            continue;

        const nlohmann::json& sectionJson = fileJson[info.sectionName];
        if (!sectionJson.is_object())
            continue;

        for (int i = 0; i < info.meta->propertyCount; ++i)
            LoadSettingProperty(info.instance, info.meta->properties[i], sectionJson);
    }
}

bool Project::Load(const std::string& path)
{
    namespace fs = std::filesystem;

    // Path can be either a directory or deki.json file
    fs::path projectPath = path;

    if (fs::is_regular_file(projectPath))
    {
        // It's a file, get parent directory
        projectPath = projectPath.parent_path();
    }

    // Look for deki.json
    fs::path dekiJsonPath = projectPath / "deki.json";
    if (!fs::exists(dekiJsonPath))
    {
        return false;
    }

    // Read deki.json
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
        // Store absolute: a project opened by a relative path otherwise propagates
        // that relative root into every derived path, and package loading rejects
        // relative paths outright.
        m_Path = NormalizeProjectRoot(projectPath.string());
        projectPath = m_Path;

        // Migrate before validating. The check below asks whether the project has
        // an assets/ directory, which is a question about the *current* layout —
        // ask it of an unmigrated project and it says no, Load bails, and the
        // migration that would have fixed it never runs. The project simply stops
        // opening.
        //
        // Gated on the structure version so it runs once, and so a project can
        // always say which layout it is on rather than each migration inferring it
        // from the shape of the directory.
        // Which editor last wrote this project. Refuse a project from a NEWER
        // one: its scenes may use a format this build does not understand, and
        // opening it would quietly rewrite them back into the older shape on
        // the next save, losing whatever the newer editor stored.
        //
        // Older is always fine, which is the whole point of the migrations
        // above and of the name and enum tables.
        //
        // The check reads "lastSavedBy" rather than the older "engineVersion",
        // which cannot be trusted: before 0.16.0 that one was stamped once at
        // scaffold time and never updated, so it recorded whatever created the
        // project rather than what last wrote it, and the scaffold had a
        // fallback that wrote the literal "1.0.0". Since 1.0.0 compares above
        // 0.16.0, honouring it would lock every project made before this one
        // out of the editor, and no threshold can tell a stale 1.0.0 from a
        // real one. "lastSavedBy" is written only by editors that maintain it,
        // so its absence means "older", which is always allowed.
        const std::string wroteWith = json.value("lastSavedBy", "");
        if (ProjectNeedsNewerEditor(wroteWith, DEKI_ENGINE_VERSION))
        {
            m_LoadError = "This project was last saved by Deki " + wroteWith +
                          "; this editor is " + DEKI_ENGINE_VERSION +
                          ". Update the editor to open it. Opening it here would "
                          "write its scenes back in the older format.";
            DEKI_LOG_ERROR("Project: %s", m_LoadError.c_str());
            return false;
        }

        m_StructureVersion = json.value("structure", "v1");
        const bool migrated = (m_StructureVersion != ProjectPaths::kStructureVersion);
        if (migrated)
        {
            std::string summary;
            if (ProjectPaths::MigrateToGeneratedLayout(m_Path, summary))
                DEKI_LOG_EDITOR("Project: layout migration — %s", summary.c_str());

            // After the generated/ move so the two never contend for a name.
            std::string flattenSummary;
            const bool flattened = ProjectPaths::MigrateFlattenProjectFolder(m_Path, flattenSummary);
            if (flattened)
                DEKI_LOG_EDITOR("Project: layout migration — %s", flattenSummary.c_str());
            else if (!flattenSummary.empty())
                DEKI_LOG_ERROR("Project: %s", flattenSummary.c_str());

            // Only record the new version once the project actually reached it.
            // Advancing regardless would mark a project migrated that is not, and
            // the gate would then stop it ever being retried.
            std::string pkgSummary;
            if (ProjectPaths::MigrateModulesToPackages(m_Path, pkgSummary))
                DEKI_LOG_EDITOR("Project: layout migration — %s", pkgSummary.c_str());

            const bool stillOnOldLayout = std::filesystem::exists(fs::path(m_Path) / "project");
            if (!stillOnOldLayout)
                m_StructureVersion = ProjectPaths::kStructureVersion;
        }

        fs::path assetsPath = GetAssetsDirectory(m_Path);
        fs::path srcPath = GetSourceDirectory(m_Path);

        if (!fs::exists(assetsPath))
        {
            DEKI_LOG_ERROR("Project: no assets directory at '%s' — not a Deki project, "
                           "or a migration did not complete",
                           assetsPath.string().c_str());
            return false;
        }

        m_AssetsPath = assetsPath.string();
        m_SrcPath = srcPath.string();

        m_BuildPath = ProjectPaths::Build(m_Path).string();
        m_CachePath = ProjectPaths::Cache(m_Path).string();

        // Legacy source: settings used to live in a single "packageSettings" object
        // here. Read it first so an unmigrated project still loads, then let the
        // settings/ files overlay it below. The next Save writes the files and drops
        // this block, so the migration completes without a separate pass.
        if (json.contains("packageSettings") && json["packageSettings"].is_object())
        {
            const auto& packageSettingsJson = json["packageSettings"];
            for (auto it = packageSettingsJson.begin(); it != packageSettingsJson.end(); ++it)
            {
                std::string section = it.key();
                const nlohmann::json& sectionJson = it.value();

                // "Display" was split: the framebuffer contract (width/height/format)
                // kept its fields under the new "Framebuffer" name, while frame pacing
                // moved to "Core". Without the alias, an existing project's resolution
                // and colour format would silently fall back to defaults on first open
                // after the rename.
                if (section == "Display")
                {
                    if (Deki::Component* core = Deki::SettingsRegistry::Instance().GetByName("Core"))
                    {
                        if (const Deki::ComponentMeta* coreMeta =
                                Deki::ComponentRegistry::Instance().GetMeta(core->GetType()))
                        {
                            for (int i = 0; i < coreMeta->propertyCount; ++i)
                                LoadSettingProperty(core, coreMeta->properties[i], sectionJson);
                        }
                    }
                    section = "Framebuffer";
                }

                Deki::Component* inst = Deki::SettingsRegistry::Instance().GetByName(section.c_str());
                if (inst == nullptr) continue;  // unknown package; leave alone
                const Deki::ComponentMeta* compMeta = Deki::ComponentRegistry::Instance().GetMeta(inst->GetType());
                if (compMeta == nullptr) continue;
                for (int i = 0; i < compMeta->propertyCount; ++i)
                {
                    LoadSettingProperty(inst, compMeta->properties[i], sectionJson);
                }
            }
        }

        // Current source of truth. Applied after the legacy block so a project that
        // has both takes the files.
        LoadSettingsFiles(m_Path);

        // Load startup scene. Older files stored it with backslashes, which the
        // runtime's asset lookup (keyed by the forward-slash asset path) never
        // matched: the device built fine and then failed to load its scene.
        SetStartupScene(json.value("startupScene", ""));

        // Load render pipeline
        m_RenderPipelineGuid = json.value("renderPipelineGuid", "");

        // Load active platform
        m_ActivePlatformId = json.value("activePlatformId", "");

        // Load force-include GUIDs for asset deployment
        m_ForceIncludeGuids.clear();
        if (json.contains("forceIncludeGuids") && json["forceIncludeGuids"].is_array())
        {
            for (const auto& guid : json["forceIncludeGuids"])
            {
                m_ForceIncludeGuids.insert(guid.get<std::string>());
            }
        }

        // Load flash-targeted GUIDs
        m_FlashGuids.clear();
        if (json.contains("flashGuids") && json["flashGuids"].is_array())
        {
            for (const auto& guid : json["flashGuids"])
            {
                m_FlashGuids.insert(guid.get<std::string>());
            }
        }

        // A freshly loaded project is clean — except when this load migrated it.
        // Marking dirty earlier would not survive: this assignment runs at the end
        // of the same function and silently cleared it, so the migration never got
        // written and re-ran on every open.
        m_IsDirty = migrated;
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
        return false;

    fs::path dekiJsonPath = fs::path(m_Path) / "deki.json";

    // Read existing file to preserve other settings
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
                // A malformed deki.json leaves every field at its default, which
                // reads downstream as "an empty project" rather than "a broken file".
                DEKI_LOG_WARNING("Project: could not parse %s (%s); using defaults",
                                 dekiJsonPath.string().c_str(), e.what());
            }
        }
    }

    // Settings live under settings/, not in deki.json. packageSettings was keyed by
    // section name, so it grew with every package a project installed and any two
    // people editing unrelated settings collided in one file. A package's sections
    // now live in its own folder and go away when it is uninstalled.
    //
    // Only the sections actually written are dropped from the legacy block below.
    // A section whose package is not currently loaded is not in the registry, so it
    // cannot be written — erasing the whole block regardless would delete it. That
    // is not hypothetical: it is how this project's "Rendering" settings were lost
    // on a run where the packages had failed to load.
    const std::vector<std::string> writtenSections = SaveSettingsFiles(m_Path);

    // Record the layout version so the migration above runs exactly once.
    // Which editor wrote this, rewritten every save. Load refuses a project
    // from a newer one. "engineVersion" used to carry this, but it was stamped
    // once when the project was created and never touched again, so every
    // project claimed the version that scaffolded it, forever. It is kept
    // up to date here too because the ESP-IDF toolchain and the doc generator
    // still read it, but the gate on load uses "lastSavedBy" alone.
    json["lastSavedBy"] = DEKI_ENGINE_VERSION;
    json["engineVersion"] = DEKI_ENGINE_VERSION;

    json["structure"] = m_StructureVersion.empty()
                            ? std::string(ProjectPaths::kStructureVersion)
                            : m_StructureVersion;

    // Update startup scene
    json["startupScene"] = m_StartupScene;

    // Update render pipeline
    json["renderPipelineGuid"] = m_RenderPipelineGuid;

    // Update active platform
    json["activePlatformId"] = m_ActivePlatformId;

    // Update force-include GUIDs
    json["forceIncludeGuids"] = nlohmann::json::array();
    for (const auto& guid : m_ForceIncludeGuids)
    {
        json["forceIncludeGuids"].push_back(guid);
    }

    // Update flash-targeted GUIDs
    json["flashGuids"] = nlohmann::json::array();
    for (const auto& guid : m_FlashGuids)
    {
        json["flashGuids"].push_back(guid);
    }

    // Settings now live in settings/; drop the copies that used to sit here so the
    // two cannot disagree. Written out by SaveSettingsFiles above first, so this
    // never loses anything.
    json.erase("displaySettings");
    json.erase("profilingSettings");
    json.erase("unitSettings");

    if (json.contains("packageSettings") && json["packageSettings"].is_object())
    {
        nlohmann::json& legacy = json["packageSettings"];
        for (const std::string& section : writtenSections)
            legacy.erase(section);
        // "Display" was split into Framebuffer + Core; both are written, so the
        // legacy key has no remaining owner once either lands.
        if (std::find(writtenSections.begin(), writtenSections.end(), "Framebuffer") != writtenSections.end())
            legacy.erase("Display");

        if (legacy.empty())
            json.erase("packageSettings");
    }
    // Only ever written, never read by anything — dropped rather than migrated.
    json.erase("assetSettings");

    // Write back atomically: a crash mid-write used to leave a truncated
    // deki.json, the one file the editor needs to open the project at all.
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
