#include "ProjectSettingsPanel.h"
#include "../PanelMaximize.h"
#include <deki-editor/EditorTheme.h>
#include "../EditorApp.h"
#include "../core/Project.h"
#include "inspector/InspectorUtils.h"
#include "../core/Commands.h"
#include <deki-editor/CommandHistory.h>
#include <functional>
#include <memory>
#include <deki/Engine.h>
#include <deki/reflection/SettingsRegistry.h>
#include <deki/reflection/Property.h>
#include <deki-editor/AssetPipeline.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>

namespace DekiEditor
{

using InspectorUtils::BeginPropertyRow;

namespace
{
// Show tooltip from DEKI_TOOLTIP("..."). Mirrors PropertyEditorFactory's
// ShowTooltip but inline so this panel doesn't depend on the inspector.
void ShowTooltip(const Deki::PropertyInfo& prop)
{
    if (prop.tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        DekiEditor::Tooltip("%s", prop.tooltip);
}

// Visibility check for DEKI_VISIBLE_WHEN(...). Returns true if the
// property should be rendered. Mirrors InspectorUtils::IsPropertyVisible
// for the bool-toggle and threshold cases — sufficient for settings.
bool IsVisible(const Deki::Component* comp, const Deki::PropertyInfo* props,
               int propCount, const Deki::PropertyInfo& prop)
{
    if (!prop.visibilityProperty) return true;
    for (int i = 0; i < propCount; ++i)
    {
        const Deki::PropertyInfo& other = props[i];
        if (!other.name || std::strcmp(other.name, prop.visibilityProperty) != 0) continue;
        // Read controlling-property value as int32.
        int32_t controllingValue = 0;
        switch (other.type)
        {
            case Deki::PropertyType::Bool:
                controllingValue = Deki::GetPropertyValue<bool>(comp, other) ? 1 : 0;
                break;
            case Deki::PropertyType::Int8: controllingValue = Deki::GetPropertyValue<int8_t>(comp, other); break;
            case Deki::PropertyType::Int16: controllingValue = Deki::GetPropertyValue<int16_t>(comp, other); break;
            case Deki::PropertyType::Int32: controllingValue = Deki::GetPropertyValue<int32_t>(comp, other); break;
            case Deki::PropertyType::UInt8: controllingValue = Deki::GetPropertyValue<uint8_t>(comp, other); break;
            case Deki::PropertyType::UInt16: controllingValue = Deki::GetPropertyValue<uint16_t>(comp, other); break;
            case Deki::PropertyType::UInt32: controllingValue = (int32_t)Deki::GetPropertyValue<uint32_t>(comp, other); break;
            case Deki::PropertyType::Enum:
                switch (other.enumSize)
                {
                    case 1: controllingValue = Deki::GetPropertyValue<uint8_t>(comp, other); break;
                    case 2: controllingValue = Deki::GetPropertyValue<uint16_t>(comp, other); break;
                    default: controllingValue = Deki::GetPropertyValue<int32_t>(comp, other); break;
                }
                break;
            default: return true;
        }
        if (prop.visibilityMask != 0)
            return (prop.visibilityMask & (1u << controllingValue)) != 0;
        return controllingValue >= prop.visibilityMinValue;
    }
    return true;
}

// Render one settings property using the same widget style as the
// inspector (left-aligned label via BeginPropertyRow, drag widgets for
// numerics). Returns true when the user has finished an edit and the
// change should be persisted — atomic widgets (Checkbox/Combo) commit
// on click; drag and text widgets commit on IsItemDeactivatedAfterEdit
// so we don't write deki.json every frame during a drag.
// One project-settings row. Every edit goes through CommandHistory, so
// settings are undoable like the Inspector rows they are modelled on (they
// used to write straight into the instance). `onChanged` runs after undo/redo
// so the file on disk follows the in-memory value.
bool DrawSettingProperty(Deki::Component* comp, const Deki::PropertyInfo& prop,
                         const std::function<void()>& onChanged)
{
    std::string displayName = InspectorUtils::GetDisplayName(prop);
    const char* label = displayName.c_str();
    bool committed = false;

    // Commit a trivially-copyable value: restore the old bytes, then let the
    // command apply the new ones so undo has the real "before".
    auto commitBytes = [&](void* target, size_t size, const void* oldBytes)
    {
        uint8_t newBytes[16];
        std::memcpy(newBytes, target, size);
        if (std::memcmp(newBytes, oldBytes, size) == 0)
            return;
        std::memcpy(target, oldBytes, size);
        auto onApply = [onChanged](const void*) { if (onChanged) onChanged(); };
        CommandHistory::Instance().ExecuteNoMerge(std::make_unique<ModifyValueCommand>(
            target, size, oldBytes, newBytes, displayName, onApply, onApply));
        committed = true;
    };

    switch (prop.type)
    {
        case Deki::PropertyType::Bool:
        {
            bool& v = ::Deki::GetPropertyRef<bool>(comp, prop);
            const bool old = v;
            BeginPropertyRow(label);
            if (DekiEditor::SchematicCheckbox("##value", &v))
                commitBytes(&v, sizeof(v), &old);
            ShowTooltip(prop);
            break;
        }
        case Deki::PropertyType::Int32:
        {
            int32_t& v = ::Deki::GetPropertyRef<int32_t>(comp, prop);
            const int32_t captured = v;
            static int32_t s_Old = 0;
            BeginPropertyRow(label);
            if (prop.useSlider && prop.hasRange)
                ImGui::SliderInt("##value", &v, (int)prop.minValue, (int)prop.maxValue);
            else if (prop.hasRange)
                ImGui::DragInt("##value", &v, 1.0f, (int)prop.minValue, (int)prop.maxValue);
            else
                ImGui::DragInt("##value", &v);
            if (prop.hasRange)
                v = std::clamp(v, (int)prop.minValue, (int)prop.maxValue);
            if (ImGui::IsItemActivated())
                s_Old = captured;
            if (ImGui::IsItemDeactivatedAfterEdit())
                commitBytes(&v, sizeof(v), &s_Old);
            ShowTooltip(prop);
            break;
        }
        case Deki::PropertyType::Float:
        {
            float& v = ::Deki::GetPropertyRef<float>(comp, prop);
            const float captured = v;
            static float s_Old = 0.0f;
            BeginPropertyRow(label);
            if (prop.useSlider && prop.hasRange)
                ImGui::SliderFloat("##value", &v, prop.minValue, prop.maxValue);
            else if (prop.hasRange)
                ImGui::DragFloat("##value", &v, 0.1f, prop.minValue, prop.maxValue);
            else
                ImGui::DragFloat("##value", &v, 0.1f);
            if (prop.hasRange)
                v = std::clamp(v, prop.minValue, prop.maxValue);
            if (ImGui::IsItemActivated())
                s_Old = captured;
            if (ImGui::IsItemDeactivatedAfterEdit())
                commitBytes(&v, sizeof(v), &s_Old);
            ShowTooltip(prop);
            break;
        }
        case Deki::PropertyType::String:
        {
            std::string& v = ::Deki::GetPropertyRef<std::string>(comp, prop);
            const std::string captured = v;
            static std::string s_Old;
            char buf[256];
            std::strncpy(buf, v.c_str(), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            BeginPropertyRow(label);
            if (ImGui::InputText("##value", buf, sizeof(buf)))
                v = buf;  // live preview
            if (ImGui::IsItemActivated())
                s_Old = captured;
            if (ImGui::IsItemDeactivatedAfterEdit() && v != s_Old)
            {
                const std::string newValue = v;
                v = s_Old;
                CommandHistory::Instance().ExecuteNoMerge(std::make_unique<ModifyFieldCommand<std::string>>(
                    &v, newValue, displayName, onChanged));
                committed = true;
            }
            ShowTooltip(prop);
            break;
        }
        case Deki::PropertyType::Enum:
        {
            const size_t size = prop.enumSize == 1 ? 1 : prop.enumSize == 2 ? 2 : 4;
            void* target = reinterpret_cast<char*>(comp) + prop.offset;
            uint8_t old[4];
            std::memcpy(old, target, size);
            int currentValue = 0;
            switch (prop.enumSize)
            {
                case 1: currentValue = Deki::GetPropertyValue<uint8_t>(comp, prop); break;
                case 2: currentValue = Deki::GetPropertyValue<uint16_t>(comp, prop); break;
                default: currentValue = Deki::GetPropertyValue<int32_t>(comp, prop); break;
            }
            auto write = [&](int value)
            {
                switch (prop.enumSize)
                {
                    case 1: Deki::SetPropertyValue<uint8_t>(comp, prop, (uint8_t)value); break;
                    case 2: Deki::SetPropertyValue<uint16_t>(comp, prop, (uint16_t)value); break;
                    default: Deki::SetPropertyValue<int32_t>(comp, prop, value); break;
                }
            };
            if (prop.enumValues && prop.enumCount > 0)
            {
                int idx = currentValue;
                if (idx < 0 || idx >= prop.enumCount) idx = 0;
                BeginPropertyRow(label);
                if (ImGui::Combo("##value", &idx, prop.enumValues, prop.enumCount))
                {
                    write(idx);
                    commitBytes(target, size, old);
                }
            }
            else
            {
                int v = currentValue;
                BeginPropertyRow(label);
                ImGui::DragInt("##value", &v);
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    write(v);
                    commitBytes(target, size, old);
                }
            }
            ShowTooltip(prop);
            break;
        }
        default:
            BeginPropertyRow(label);
            ImGui::TextDisabled("<unsupported type>");
            break;
    }
    return committed;
}
}  // namespace

ProjectSettingsPanel::ProjectSettingsPanel(EditorApp* app)
    : m_App(app)
{
}

void ProjectSettingsPanel::RefreshScans(Project* project)
{
    namespace fs = std::filesystem;
    auto* pipeline = DekiEditor::AssetPipeline::Instance();
    const size_t assetCount = pipeline ? pipeline->GetAllAssets().size() : 0;
    m_ScanAge += ImGui::GetIO().DeltaTime;
    if (assetCount == m_ScannedAssetCount && m_ScanAge < 2.0f)
        return;
    m_ScannedAssetCount = assetCount;
    m_ScanAge = 0.0f;

    m_SceneFiles.clear();
    m_SceneFiles.push_back("(None)");
    m_PipelineFiles.clear();
    m_PipelineGuids.clear();
    m_PipelineFiles.push_back("(None)");
    m_PipelineGuids.push_back("");

    const std::string assetsPath = project->GetAssetsPath();
    if (assetsPath.empty() || !fs::exists(assetsPath))
        return;
    const std::string projectPath = project->GetPath();

    for (const auto& entry : fs::recursive_directory_iterator(assetsPath))
    {
        if (!entry.is_regular_file())
            continue;
        const fs::path ext = entry.path().extension();
        if (ext == ".scene")
        {
            m_SceneFiles.push_back(fs::relative(entry.path(), assetsPath).string());
        }
        else if (ext == ".asset" && pipeline)
        {
            try
            {
                std::ifstream f(entry.path());
                nlohmann::json j;
                f >> j;
                if (j.value("type", "") != "RenderPipeline") continue;
            }
            catch (...)
            {
                continue;
            }
            m_PipelineFiles.push_back(fs::relative(entry.path(), assetsPath).string());
            std::string lookupPath = fs::relative(entry.path(), projectPath).string();
            for (char& c : lookupPath)
                if (c == '\\') c = '/';
            const auto* info = pipeline->GetAssetInfo(lookupPath);
            m_PipelineGuids.push_back(info ? info->guid : "");
        }
    }
}

void ProjectSettingsPanel::DrawStartupSceneSection(Project* project)
{
    // Same band and same property-list metrics as the Inspector: a settings
    // section is a list of properties, and there is no reason for it to be laid
    // out differently from every other list of properties in the editor.
    if (!DekiEditor::SchematicSectionBegin("Startup"))
        return;

    DekiEditor::BeginPropertyContext();

    RefreshScans(project);
    std::vector<std::string>& sceneFiles = m_SceneFiles;

    int currentIndex = 0;
    std::string currentScene = project->GetStartupScene();
    for (size_t i = 1; i < sceneFiles.size(); i++)
    {
        if (sceneFiles[i] == currentScene)
        {
            currentIndex = (int)i;
            break;
        }
    }

    auto getter = [](void* data, int idx) -> const char*
    {
        auto* files = static_cast<std::vector<std::string>*>(data);
        if (idx < 0 || idx >= (int)files->size()) return nullptr;
        return (*files)[idx].c_str();
    };

    BeginPropertyRow("Startup Scene");
    if (ImGui::Combo("##startup_scene", &currentIndex, getter, &sceneFiles, (int)sceneFiles.size()))
    {
        if (currentIndex == 0)
            project->SetStartupScene("");
        else
            project->SetStartupScene(sceneFiles[currentIndex]);
        project->Save();
    }
    if (currentIndex == 0)
        ImGui::TextDisabled("No startup scene selected");
    else
        ImGui::TextDisabled("Loaded when game starts");

    DekiEditor::EndPropertyContext();
    DekiEditor::SchematicSectionEnd();
}

void ProjectSettingsPanel::DrawRenderPipelineSection(Project* project)
{
    if (!DekiEditor::SchematicSectionBegin("Render Pipeline"))
        return;

    DekiEditor::BeginPropertyContext();

    RefreshScans(project);
    std::vector<std::string>& pipelineFiles = m_PipelineFiles;
    std::vector<std::string>& pipelineGuids = m_PipelineGuids;

    int currentPipelineIndex = 0;
    std::string currentGuid = project->GetRenderPipelineGuid();
    if (!currentGuid.empty())
    {
        bool found = false;
        for (size_t i = 1; i < pipelineGuids.size(); i++)
        {
            if (pipelineGuids[i] == currentGuid)
            {
                currentPipelineIndex = (int)i;
                found = true;
                break;
            }
        }
        if (!found)
        {
            pipelineFiles.push_back("(Unknown pipeline)");
            pipelineGuids.push_back(currentGuid);
            currentPipelineIndex = (int)pipelineFiles.size() - 1;
        }
    }

    const char* previewLabel = pipelineFiles[currentPipelineIndex].c_str();
    BeginPropertyRow("Pipeline");
    if (ImGui::BeginCombo("##pipeline", previewLabel))
    {
        for (int i = 0; i < (int)pipelineFiles.size(); i++)
        {
            bool isSelected = (currentPipelineIndex == i);
            if (ImGui::Selectable(pipelineFiles[i].c_str(), isSelected))
            {
                project->SetRenderPipelineGuid(pipelineGuids[i]);
                project->Save();
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (currentPipelineIndex == 0)
        ImGui::TextDisabled("No render pipeline selected - rendering disabled on device");
    else
        ImGui::TextDisabled("Defines renderer and passes for device builds");

    DekiEditor::EndPropertyContext();
    DekiEditor::SchematicSectionEnd();
}

void ProjectSettingsPanel::Draw()
{
    if (!m_Open) return;

    ImGui::SetNextWindowSize(ImVec2(420, 400), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("PROJECT SETTINGS###Project Settings", &m_Open, DekiEditor::MaximizedWindowFlags("PROJECT SETTINGS###Project Settings")))
    {
        Project* project = m_App->GetProject();
        if (!project)
        {
            ImGui::TextDisabled("No project loaded");
            ImGui::End();
            return;
        }

        // Asset-picker sections — engine-ignorant of feature packages but tied
        // to the editor-only AssetPipeline, so they stay hardcoded.
        DrawStartupSceneSection(project);
        ImGui::Spacing();
        DrawRenderPipelineSection(project);
        ImGui::Spacing();

        // Package-extensible settings — render every registered settings type
        // generically by walking the registry and the type's reflection
        // metadata. Tooltips, ranges, and visibility conditions all come from
        // the same Deki::PropertyInfo machinery that drives the inspector.
        for (const auto& info : Deki::SettingsRegistry::Instance().GetAll())
        {
            if (DekiEditor::SchematicSectionBegin(info.sectionName))
            {
                DekiEditor::BeginPropertyContext();
                ImGui::PushID(info.sectionName);
                bool sectionCommitted = false;
                // Also run after undo/redo, so the file follows the value.
                const std::function<void()> onChanged = [project]()
                {
                    project->Save();
                    if (auto* units = DekiEditor::GetUnitSettings())
                        units->ApplyToGlobalCache();
                };
                for (int i = 0; i < info.meta->propertyCount; ++i)
                {
                    const Deki::PropertyInfo& prop = info.meta->properties[i];
                    if (!IsVisible(info.instance, info.meta->properties, info.meta->propertyCount, prop))
                        continue;
                    ImGui::PushID(prop.name);
                    if (DrawSettingProperty(info.instance, prop, onChanged))
                        sectionCommitted = true;
                    ImGui::PopID();
                }
                ImGui::PopID();
                DekiEditor::EndPropertyContext();
                if (sectionCommitted)
                {
                    project->Save();
                    if (auto* units = DekiEditor::GetUnitSettings())
                        units->ApplyToGlobalCache();
                }
                DekiEditor::SchematicSectionEnd();
            }
            ImGui::Spacing();
        }
    }
    ImGui::End();
}

}  // namespace DekiEditor
