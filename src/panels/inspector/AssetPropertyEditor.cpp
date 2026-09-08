#include <deki-editor/EditorTheme.h>  // DekiEditor::Tooltip (delayed)
#include "AssetPropertyEditor.h"
#include "InspectorUtils.h"
#include "../../EditorApp.h"
#include <deki-editor/AssetPicker.h>
#include "../AssetBrowserPanel.h"
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <deki-editor/SubAsset.h>
#include <deki-editor/GuidCache.h>
#include "../../core/Project.h"
#include <deki-editor/TextureImporter.h>
#include <deki-editor/AssetData.h>
#include <deki-editor/Paths.h>
#include <deki/reflection/Property.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/Component.h>
#include <deki/assets/AssetRef.h>
#include "imgui.h"
#include <deki-editor/IconsTabler.h>
#include <deki-editor/Command.h>
#include <deki-editor/CommandHistory.h>
#include "../../core/commands/PropertyCommands.h"
#include "../../core/commands/ModifyFieldCommand.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace DekiEditor
{

// Helper to generate deterministic variant GUID (uses shared FNV-1a implementation)
static std::string GenerateVariantGuid(const std::string& fontGuid, int fontSize)
{
    std::string seed = fontGuid + ":" + std::to_string(fontSize);
    // Force deep copy to avoid cross-DLL heap issues
    // DekiEditor::GenerateDeterministicGuid returns std::string from deki-editor.dll's heap
    return std::string(DekiEditor::GenerateDeterministicGuid(seed).c_str());
}

// Helper to read available font sizes from .data file
static std::vector<int> GetFontSizesFromData(const std::string& fontPath)
{
    std::vector<int> sizes;
    std::string dataPath = fontPath + ".data";

    if (!fs::exists(dataPath))
        return sizes;

    std::ifstream file(dataPath);
    if (!file.is_open())
        return sizes;

    try
    {
        json j = json::parse(file);
        if (j.contains("fontSettings") && j["fontSettings"].contains("sizes"))
        {
            for (const auto& size : j["fontSettings"]["sizes"])
            {
                if (size.is_number_integer())
                    sizes.push_back(size.get<int>());
            }
        }
    }
    catch (const json::exception&)
    {
    }

    return sizes;
}

AssetPropertyEditor::AssetPropertyEditor(EditorApp* app, AssetPicker* picker)
    : m_App(app), m_AssetPicker(picker)
{
}

std::string AssetPropertyEditor::ResolveGuidToDisplayName(const std::string& guid)
{
    if (guid.empty())
        return "(None)";

    // Check if this looks like a GUID (contains dashes and is long enough)
    bool isGuid = (guid.find('-') != std::string::npos && guid.length() >= 32);

    if (isGuid)
    {
        // Look up asset info from pipeline
        auto* pipeline = m_App->GetAssetPipeline();
        if (pipeline)
        {
            // Check if this is a SubAsset (sprite frame)
            const SubAssetInfo* subAsset = pipeline->GetSubAsset(guid);
            if (subAsset)
            {
                // Get parent asset info for display
                const AssetInfo* parentInfo = pipeline->GetAssetInfoByGuid(subAsset->parentGuid);
                if (parentInfo)
                {
                    // Extract filename from path
                    size_t lastSlash = parentInfo->path.find_last_of("/\\");
                    std::string parentName = (lastSlash != std::string::npos)
                                                 ? parentInfo->path.substr(lastSlash + 1)
                                                 : parentInfo->path;
                    std::string frameName = subAsset->name.empty()
                                                ? "Frame " + std::to_string(subAsset->subAssetIndex)
                                                : subAsset->name;
                    return parentName + " > " + frameName;
                }
                else
                {
                    return subAsset->name.empty()
                               ? "Frame " + std::to_string(subAsset->subAssetIndex)
                               : subAsset->name;
                }
            }

            const AssetInfo* info = pipeline->GetAssetInfoByGuid(guid);
            if (info)
            {
                // Extract filename from path
                namespace fs = std::filesystem;
                std::string filename = fs::path(info->path).filename().string();
                std::string ext = fs::path(info->path).extension().string();

                // Icon chosen from the package-declared AssetCategory so the
                // owning package is the single source of truth for what icon
                // represents its file type.
                const char* icon = ICON_TI_FILE " ";
                switch (AssetTypeRegistry::Instance().GetCategoryForExtension(ext))
                {
                    case AssetCategory::Texture: icon = ICON_TI_PHOTO " "; break;
                    case AssetCategory::Font: icon = ICON_TI_TYPOGRAPHY " "; break;
                    case AssetCategory::Scene: icon = ICON_TI_BOX " "; break;
                    case AssetCategory::Animation: icon = ICON_TI_MOVIE " "; break;
                    default: break;  // generic ICON_TI_FILE
                }

                return std::string(icon) + filename;
            }
            else
            {
                // GUID no longer resolves (asset deleted) - show a plain
                // "Missing" like Unity, no GUID.
                return "Missing";
            }
        }
        else
        {
            return guid.substr(0, 8) + "...";
        }
    }
    else
    {
        // Legacy path - show filename
        size_t lastSlash = guid.find_last_of("/\\");
        if (lastSlash != std::string::npos)
            return guid.substr(lastSlash + 1);
        else
            return guid;
    }
}

AssetFilterType AssetPropertyEditor::GetAssetFilterType(const std::string& assetType)
{
    std::string typeLower = assetType;
    std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(), ::tolower);

    if (typeLower == "texture" || typeLower == "sprite" || typeLower == "image")
        return AssetFilterType::Textures;
    if (typeLower == "audio" || typeLower == "sound" || typeLower == "music")
        return AssetFilterType::Audio;
    if (typeLower == "script")
        return AssetFilterType::Scripts;
    if (typeLower == "scene" || typeLower == "scene")
        return AssetFilterType::Scenes;
    if (typeLower == "font" || typeLower == "bitmapfont")
        return AssetFilterType::Fonts;
    if (typeLower == "animation" || typeLower == "anim")
        return AssetFilterType::Animations;

    return AssetFilterType::All;
}

void AssetPropertyEditor::FindDimensionPointers(Deki::Component* comp, int32_t** outWidth, int32_t** outHeight)
{
    *outWidth = nullptr;
    *outHeight = nullptr;

    auto& registry = Deki::ComponentRegistry::Instance();
    const Deki::ComponentMeta* compMeta = registry.GetMeta(comp->GetType());
    if (!compMeta || !compMeta->name)
        return;

    auto findProperty = [&](const Deki::ComponentMeta* m, const char* name) -> int32_t*
    {
        if (!m || !m->properties) return nullptr;
        for (int i = 0; i < m->propertyCount; ++i)
        {
            const Deki::PropertyInfo& p = m->properties[i];
            if (p.name && p.type == Deki::PropertyType::Int32 && strcmp(p.name, name) == 0)
            {
                return reinterpret_cast<int32_t*>(reinterpret_cast<char*>(comp) + p.offset);
            }
        }
        return nullptr;
    };

    const Deki::ComponentMeta* baseMeta = (compMeta->baseTypeId != 0)
                                            ? registry.GetMeta(compMeta->baseTypeId)
                                            : nullptr;

    int32_t* widthPtr = findProperty(baseMeta, "width");
    if (!widthPtr) widthPtr = findProperty(compMeta, "width");
    int32_t* heightPtr = findProperty(baseMeta, "height");
    if (!heightPtr) heightPtr = findProperty(compMeta, "height");

    *outWidth = widthPtr;
    *outHeight = heightPtr;
}

void AssetPropertyEditor::AutoPopulateDimensions(Deki::Component* comp, const std::string& selectedGuid)
{
    auto* pipeline = m_App->GetAssetPipeline();
    if (!pipeline)
        return;

    int32_t* widthPtr = nullptr;
    int32_t* heightPtr = nullptr;
    FindDimensionPointers(comp, &widthPtr, &heightPtr);

    // Always update width/height from the new asset dimensions
    if (widthPtr && heightPtr)
    {
        // Check if this is a SubAsset (sprite frame)
        const SubAssetInfo* subAsset = pipeline->GetSubAsset(selectedGuid);
        if (subAsset)
        {
            // Get frame dimensions from frame data registry
            const SpriteFrameData* frameData = TextureImporter::GetFrameData(selectedGuid);
            if (frameData)
            {
                *widthPtr = frameData->width;
                *heightPtr = frameData->height;
                return;
            }
        }

        Project* project = m_App->GetProject();
        const AssetInfo* assetInfo = pipeline->GetAssetInfoByGuid(selectedGuid);
        if (project && assetInfo)
        {
            // First check for spritesheet frame dimensions in .data file
            std::string dataPath = DekiEditor::GetAssetDataPath(assetInfo->path);
            DekiEditor::AssetData assetData;
            bool usedFrameSize = false;

            if (DekiEditor::LoadAssetData(dataPath, assetData) &&
                assetData.settings.contains("sprite"))
            {
                auto& sprite = assetData.settings["sprite"];
                if (sprite.contains("frameWidth") && sprite.contains("frameHeight"))
                {
                    int fw = sprite["frameWidth"].get<int>();
                    int fh = sprite["frameHeight"].get<int>();
                    if (fw > 0 && fh > 0)
                    {
                        *widthPtr = fw;
                        *heightPtr = fh;
                        usedFrameSize = true;
                    }
                }
            }

            // If not a spritesheet, use full texture dimensions
            if (!usedFrameSize)
            {
                std::string cachePath = DekiEditor::GetCachePath(project->GetPath(), selectedGuid);
                DekiEditor::TexData texData;
                if (DekiEditor::TextureImporter::ReadTexFile(cachePath, texData) && texData.isValid())
                {
                    *widthPtr = static_cast<int32_t>(texData.header.width);
                    *heightPtr = static_cast<int32_t>(texData.header.height);
                }
            }
        }
    }
}

void AssetPropertyEditor::RevealAssetInBrowser(const std::string& guid)
{
    if (guid.empty()) return;

    auto* pipeline = m_App->GetAssetPipeline();
    auto* browser = m_App->GetAssetBrowserPanel();
    if (!pipeline || !browser) return;

    const AssetInfo* info = pipeline->GetAssetInfoByGuid(guid);
    if (info)
    {
        std::string absPath = pipeline->GetAbsolutePath(info->path);
        browser->RevealAsset(absPath);
    }
}

bool AssetPropertyEditor::DrawAssetProperty(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                            const char* displayName)
{
    // Asset properties - show asset reference with picker
    // Value is stored as GUID (like Unity)
    std::string& assetRef = *reinterpret_cast<std::string*>(
        reinterpret_cast<char*>(comp) + prop.offset);

    std::string displayText = ResolveGuidToDisplayName(assetRef);

    // Label in the standard property-row column (so the field lines up with
    // the other rows), then the asset name + picker icon as one combined field.
    InspectorUtils::BeginPropertyRow(displayName);
    // Inherit the right gutter set by BeginPropertyRow (SetNextItemWidth) so the
    // combined name+picker field ends in line with every other value control.
    float fieldWidth = ImGui::CalcItemWidth();

    float pickerBtnWidth = ImGui::GetFrameHeight();
    float nameFieldWidth = fieldWidth - pickerBtnWidth;

    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_FrameBgActive]);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));

    std::string popupId = std::string("AssetPicker##") + prop.name;

    // Left: asset name button (double-click to reveal in Asset Browser)
    std::string nameBtnId = displayText + "##name_" + prop.name;
    ImGui::Button(nameBtnId.c_str(), ImVec2(nameFieldWidth, 0));

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && !assetRef.empty())
    {
        RevealAssetInBrowser(assetRef);
    }

    if (ImGui::IsItemHovered() && !assetRef.empty())
    {
        DekiEditor::Tooltip("GUID: %s\nDouble-click to reveal in Asset Browser", assetRef.c_str());
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    // Right: picker button, flush against the name field (one combined control)
    ImGui::SameLine(0, 0);
    std::string pickerBtnId = std::string(ICON_TI_CHEVRON_DOWN "##picker_") + prop.name;
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    const bool pickerClicked = ImGui::Button(pickerBtnId.c_str(), ImVec2(pickerBtnWidth, 0));
    ImGui::PopStyleVar();
    if (pickerClicked)
    {
        m_ActivePropertyName = prop.name;
        m_ActiveComponent = comp;

        AssetFilterType filter = GetAssetFilterType(prop.assetType ? prop.assetType : "");
        std::string selectedGuid;
        m_AssetPicker->Open(popupId, assetRef, filter, selectedGuid);
    }

    std::string selectedGuid;
    if (m_AssetPicker->Draw(popupId, selectedGuid))
    {
        if (m_ActiveComponent == comp && m_ActivePropertyName == prop.name)
        {
            assetRef = selectedGuid;
        }
    }

    return false;
}

bool AssetPropertyEditor::DrawAssetRefProperty(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                               const char* displayName)
{
    // AssetRef properties - uses AssetRefBase struct with guid and source fields
    // For derived assets (like fonts), source holds the original asset GUID
    // and guid holds the baked/compiled asset GUID
    Deki::AssetRefBase& assetRefBase = *reinterpret_cast<Deki::AssetRefBase*>(
        reinterpret_cast<char*>(comp) + prop.offset);

    // Use source for display if available (e.g., TTF font), otherwise guid
    const std::string& displayGuid = assetRefBase.source.empty() ? assetRefBase.guid : assetRefBase.source;

    std::string displayText = ResolveGuidToDisplayName(displayGuid);

    // Label in the standard property-row column (so the field lines up with
    // the other rows), then the asset name + picker icon as one combined field.
    InspectorUtils::BeginPropertyRow(displayName);
    // Inherit the right gutter set by BeginPropertyRow (SetNextItemWidth) so the
    // combined name+picker field ends in line with every other value control.
    float fieldWidth = ImGui::CalcItemWidth();

    float pickerBtnWidth = ImGui::GetFrameHeight();
    float nameFieldWidth = fieldWidth - pickerBtnWidth;

    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_FrameBgActive]);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));

    std::string popupId = std::string("AssetPicker##") + prop.name;

    // Left: asset name button (double-click to reveal in Asset Browser)
    std::string nameBtnId = displayText + "##refname_" + prop.name;
    ImGui::Button(nameBtnId.c_str(), ImVec2(nameFieldWidth, 0));

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && !displayGuid.empty())
    {
        RevealAssetInBrowser(displayGuid);
    }

    // Drag-drop target for assets from Asset Browser (must be right after the button)
    if (ImGui::BeginDragDropTarget())
    {
        // Helper: build and execute the undoable drop command. Pre-computes any
        // auto-populated width/height by snapshotting the component's current values,
        // applying the auto-populate, capturing the new values, then restoring —
        // so all changes are applied through commands and a single Undo reverts them.
        auto applyDrop = [&](const std::string& newGuid, const std::string& newSource,
                             const std::string& dimGuid, bool autoPopulate)
        {
            std::vector<std::unique_ptr<Command>> commands;
            commands.push_back(std::make_unique<ModifyAssetRefCommand>(comp, prop, newGuid, newSource));

            if (autoPopulate)
            {
                int32_t* widthPtr = nullptr;
                int32_t* heightPtr = nullptr;
                FindDimensionPointers(comp, &widthPtr, &heightPtr);
                if (widthPtr && heightPtr)
                {
                    int32_t oldWidth = *widthPtr;
                    int32_t oldHeight = *heightPtr;

                    AutoPopulateDimensions(comp, dimGuid);
                    int32_t newWidth = *widthPtr;
                    int32_t newHeight = *heightPtr;

                    *widthPtr = oldWidth;
                    *heightPtr = oldHeight;

                    if (newWidth != oldWidth)
                        commands.push_back(std::make_unique<ModifyFieldCommand<int32_t>>(widthPtr, newWidth, "width"));
                    if (newHeight != oldHeight)
                        commands.push_back(std::make_unique<ModifyFieldCommand<int32_t>>(heightPtr, newHeight, "height"));
                }
            }

            CommandHistory::Instance().ExecuteNoMerge(
                std::make_unique<CompoundCommand>("Set " + std::string(prop.name), std::move(commands)));
        };

        bool handled = false;

        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
        {
            std::string droppedPath(static_cast<const char*>(payload->Data));
            auto* pipeline = m_App->GetAssetPipeline();
            if (pipeline)
            {
                // droppedPath is project-relative (e.g., "assets/textures/player.png")
                const AssetInfo* info = pipeline->GetAssetInfo(droppedPath);
                if (info && !info->guid.empty())
                {
                    // Check if this is a source asset (TTF/OTF font) that needs baking
                    std::string ext = std::filesystem::path(droppedPath).extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    bool isSourceAsset = (ext == ".ttf" || ext == ".otf");

                    if (isSourceAsset)
                        applyDrop(/*guid*/ "", /*source*/ info->guid, /*dimGuid*/ "", false);
                    else
                        applyDrop(info->guid, "", info->guid, true);

                    handled = true;
                }
            }
        }

        // Accept ASSET_GUID for sub-assets (sprite frames) and direct GUID drops
        // Skip if already handled via ASSET_PATH to prevent overwriting
        if (!handled)
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_GUID"))
            {
                std::string droppedGuid(static_cast<const char*>(payload->Data));
                auto* pipeline = m_App->GetAssetPipeline();
                if (pipeline)
                {
                    // Check if it's a valid asset or sub-asset (sprite frame)
                    const AssetInfo* info = pipeline->GetAssetInfoByGuid(droppedGuid);
                    const SubAssetInfo* subAsset = pipeline->GetSubAsset(droppedGuid);

                    if (subAsset)
                    {
                        // SubAsset (sprite frame): parent GUID loads the texture, frame GUID locates the frame
                        applyDrop(subAsset->parentGuid, droppedGuid, droppedGuid, true);
                    }
                    else if (info)
                    {
                        applyDrop(droppedGuid, "", droppedGuid, true);
                    }
                }
            }
        }

        ImGui::EndDragDropTarget();
    }

    if (ImGui::IsItemHovered() && !displayGuid.empty())
    {
        // Show both GUIDs in tooltip for debugging
        if (assetRefBase.source.empty())
        {
            DekiEditor::Tooltip("GUID: %s\nDouble-click to reveal in Asset Browser", assetRefBase.guid.c_str());
        }
        else
        {
            DekiEditor::Tooltip("Source: %s\nBaked: %s\nDouble-click to reveal in Asset Browser",
                                assetRefBase.source.c_str(), assetRefBase.guid.c_str());
        }
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    // Right: picker button, flush against the name field (one combined control)
    ImGui::SameLine(0, 0);
    std::string pickerBtnId = std::string(ICON_TI_CHEVRON_DOWN "##refpicker_") + prop.name;
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    const bool pickerClicked = ImGui::Button(pickerBtnId.c_str(), ImVec2(pickerBtnWidth, 0));
    ImGui::PopStyleVar();
    if (pickerClicked)
    {
        m_ActivePropertyName = prop.name;
        m_ActiveComponent = comp;

        AssetFilterType filter = GetAssetFilterType(prop.assetType ? prop.assetType : "");
        std::string selectedGuid;
        m_AssetPicker->Open(popupId, displayGuid, filter, selectedGuid);
    }

    std::string selectedGuid;
    if (m_AssetPicker->Draw(popupId, selectedGuid))
    {
        if (m_ActiveComponent == comp && m_ActivePropertyName == prop.name)
        {
            // Check if this is a source asset (TTF/OTF font) that needs baking
            bool isSourceAsset = false;
            auto* pipeline = m_App->GetAssetPipeline();
            if (pipeline)
            {
                const AssetInfo* info = pipeline->GetAssetInfoByGuid(selectedGuid);
                if (info)
                {
                    std::string ext = std::filesystem::path(info->path).extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    isSourceAsset = (ext == ".ttf" || ext == ".otf");
                }
            }

            if (isSourceAsset)
            {
                // For source assets, store in source field
                // The component editor will compute the baked guid
                assetRefBase.source = selectedGuid;
                assetRefBase.guid.clear();  // Will be computed by component editor
            }
            else
            {
                // Check if this is a subasset (sprite frame)
                const SubAssetInfo* subAsset = pipeline ? pipeline->GetSubAsset(selectedGuid) : nullptr;
                if (subAsset)
                {
                    // SubAsset (sprite frame): store parent GUID for loading, frame GUID for coordinate lookup
                    assetRefBase.guid = subAsset->parentGuid;
                    assetRefBase.source = selectedGuid;
                }
                else
                {
                    // For direct assets (sprites, etc.), store in guid field
                    assetRefBase.guid = selectedGuid;
                    assetRefBase.source.clear();
                }

                // Auto-set width/height from asset dimensions if they are 0
                AutoPopulateDimensions(comp, selectedGuid);
            }
            assetRefBase.ptr = nullptr;
            assetRefBase.loadAttempted = false;
            comp->ResetRefsResolved();
        }
    }

    return false;
}

}  // namespace DekiEditor
