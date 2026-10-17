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
#include <deki-editor/Localization.h>
#include "../../core/commands/CommandText.h"
#include "../../core/commands/PropertyCommands.h"
#include "../../core/commands/ModifyFieldCommand.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace DekiEditor
{

// The GUID of a font size variant, derived from the font's (shared FNV-1a hash).
static std::string GenerateVariantGuid(const std::string& fontGuid, int fontSize)
{
    std::string seed = fontGuid + ":" + std::to_string(fontSize);
    // Deep copy: the returned string lives on deki-editor.dll's heap.
    return std::string(DekiEditor::GenerateDeterministicGuid(seed).c_str());
}

// The font sizes listed in the font's .data file.
static std::vector<int> GetFontSizesFromData(const std::string& fontPath)
{
    std::vector<int> sizes;
    std::string dataPath = fontPath + ".data";

    if (!fs::exists(dataPath))
    {
        return sizes;
    }

    std::ifstream file(dataPath);
    if (!file.is_open())
    {
        return sizes;
    }

    try
    {
        json j = json::parse(file);
        if (j.contains("fontSettings") && j["fontSettings"].contains("sizes"))
        {
            for (const auto& size : j["fontSettings"]["sizes"])
            {
                if (size.is_number_integer())
                {
                    sizes.push_back(size.get<int>());
                }
            }
        }
    }
    catch (const json::exception&)
    {
    }

    return sizes;
}

AssetPropertyEditor::AssetPropertyEditor(EditorApp* app, AssetPicker* picker)
    : m_App(app),
      m_AssetPicker(picker)
{
}

namespace
{

// A sprite frame's name, or "Frame N" when it has none.
std::string SubAssetName(const SubAssetInfo& subAsset)
{
    if (!subAsset.name.empty())
    {
        return subAsset.name;
    }
    char name[64];
    snprintf(name, sizeof(name), Tr("Frame %d"), static_cast<int>(subAsset.subAssetIndex));
    return name;
}

}  // namespace

std::string AssetPropertyEditor::ResolveGuidToDisplayName(const std::string& guid)
{
    if (guid.empty())
    {
        return Tr("(None)");
    }

    // A GUID has dashes and is at least 32 characters.
    bool isGuid = (guid.find('-') != std::string::npos && guid.length() >= 32);

    if (isGuid)
    {
        auto* pipeline = m_App->GetAssetPipeline();
        if (pipeline)
        {
            // A sub-asset (sprite frame) shows its parent's name.
            const SubAssetInfo* subAsset = pipeline->GetSubAsset(guid);
            if (subAsset)
            {
                const AssetInfo* parentInfo = pipeline->GetAssetInfoByGuid(subAsset->parentGuid);
                if (parentInfo)
                {
                    size_t lastSlash = parentInfo->path.find_last_of("/\\");
                    std::string parentName =
                        (lastSlash != std::string::npos) ? parentInfo->path.substr(lastSlash + 1) : parentInfo->path;
                    return parentName + " > " + SubAssetName(*subAsset);
                }
                else
                {
                    return SubAssetName(*subAsset);
                }
            }

            const AssetInfo* info = pipeline->GetAssetInfoByGuid(guid);
            if (info)
            {
                namespace fs = std::filesystem;
                std::string filename = fs::path(info->path).filename().string();
                std::string ext = fs::path(info->path).extension().string();

                // The icon follows the AssetCategory the owning package
                // declares for the file type.
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
                // The asset is gone: show "Missing", like Unity.
                return Tr("Missing");
            }
        }
        else
        {
            return guid.substr(0, 8) + "...";
        }
    }
    else
    {
        // Not a GUID but a path: show its file name.
        size_t lastSlash = guid.find_last_of("/\\");
        if (lastSlash != std::string::npos)
        {
            return guid.substr(lastSlash + 1);
        }
        else
        {
            return guid;
        }
    }
}

AssetFilterType AssetPropertyEditor::GetAssetFilterType(const std::string& assetType)
{
    std::string typeLower = assetType;
    std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(), ::tolower);

    if (typeLower == "texture" || typeLower == "sprite" || typeLower == "image")
    {
        return AssetFilterType::Textures;
    }
    if (typeLower == "audio" || typeLower == "sound" || typeLower == "music")
    {
        return AssetFilterType::Audio;
    }
    if (typeLower == "script")
    {
        return AssetFilterType::Scripts;
    }
    if (typeLower == "scene" || typeLower == "scene")
    {
        return AssetFilterType::Scenes;
    }
    if (typeLower == "font" || typeLower == "bitmapfont")
    {
        return AssetFilterType::Fonts;
    }
    if (typeLower == "animation" || typeLower == "anim")
    {
        return AssetFilterType::Animations;
    }

    return AssetFilterType::All;
}

void AssetPropertyEditor::FindDimensionPointers(Deki::Component* comp, int32_t** outWidth, int32_t** outHeight)
{
    *outWidth = nullptr;
    *outHeight = nullptr;

    auto& registry = Deki::ComponentRegistry::Instance();
    const Deki::ComponentMeta* compMeta = registry.GetMeta(comp->GetType());
    if (!compMeta || !compMeta->name)
    {
        return;
    }

    auto findProperty = [&](const Deki::ComponentMeta* m, const char* name) -> int32_t*
    {
        if (!m || !m->properties)
        {
            return nullptr;
        }
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

    const Deki::ComponentMeta* baseMeta =
        (compMeta->baseTypeId != 0) ? registry.GetMeta(compMeta->baseTypeId) : nullptr;

    int32_t* widthPtr = findProperty(baseMeta, "width");
    if (!widthPtr)
    {
        widthPtr = findProperty(compMeta, "width");
    }
    int32_t* heightPtr = findProperty(baseMeta, "height");
    if (!heightPtr)
    {
        heightPtr = findProperty(compMeta, "height");
    }

    *outWidth = widthPtr;
    *outHeight = heightPtr;
}

void AssetPropertyEditor::AutoPopulateDimensions(Deki::Component* comp, const std::string& selectedGuid)
{
    auto* pipeline = m_App->GetAssetPipeline();
    if (!pipeline)
    {
        return;
    }

    int32_t* widthPtr = nullptr;
    int32_t* heightPtr = nullptr;
    FindDimensionPointers(comp, &widthPtr, &heightPtr);

    if (widthPtr && heightPtr)
    {
        // A sub-asset (sprite frame) has its frame's size.
        const SubAssetInfo* subAsset = pipeline->GetSubAsset(selectedGuid);
        if (subAsset)
        {
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
            // A spritesheet's frame size, from its .data file.
            std::string dataPath = DekiEditor::GetAssetDataPath(assetInfo->path);
            DekiEditor::AssetData assetData;
            bool usedFrameSize = false;

            // The sidecar keeps sprite settings under "settings": { "sprite": ... }.
            if (DekiEditor::LoadAssetData(dataPath, assetData) && assetData.settings.contains("settings") &&
                assetData.settings["settings"].contains("sprite"))
            {
                auto& sprite = assetData.settings["settings"]["sprite"];
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

            // Otherwise the whole texture.
            if (!usedFrameSize)
            {
                // The image's size, which a texture shrunk by Max Size records.
                std::string cachePath = DekiEditor::GetCachePath(project->GetPath(), selectedGuid);
                int imageW = 0, imageH = 0;
                if (DekiEditor::TextureImporter::ReadSourceSize(cachePath, imageW, imageH))
                {
                    *widthPtr = static_cast<int32_t>(imageW);
                    *heightPtr = static_cast<int32_t>(imageH);
                }
            }
        }
    }
}

void AssetPropertyEditor::RevealAssetInBrowser(const std::string& guid)
{
    if (guid.empty())
    {
        return;
    }

    auto* pipeline = m_App->GetAssetPipeline();
    auto* browser = m_App->GetAssetBrowserPanel();
    if (!pipeline || !browser)
    {
        return;
    }

    const AssetInfo* info = pipeline->GetAssetInfoByGuid(guid);
    if (info)
    {
        std::string absPath = pipeline->GetAbsolutePath(info->path);
        browser->RevealAsset(absPath);
    }
}

bool AssetPropertyEditor::DrawAssetRefProperty(Deki::Component* comp, const Deki::PropertyInfo& prop,
                                               const char* displayName)
{
    // For derived assets (fonts), `source` holds the original asset's GUID and
    // `guid` the baked asset's. Otherwise only `guid` is set.
    Deki::AssetRefBase& assetRefBase =
        *reinterpret_cast<Deki::AssetRefBase*>(reinterpret_cast<char*>(comp) + prop.offset);

    // Show the source (a TTF font, say) when there is one.
    const std::string& displayGuid = assetRefBase.source.empty() ? assetRefBase.guid : assetRefBase.source;

    std::string displayText = ResolveGuidToDisplayName(displayGuid);

    // The label in the usual column, then the asset name and picker as one field.
    InspectorUtils::BeginPropertyRow(displayName);
    // The width BeginPropertyRow set, so the field ends in line with the others.
    float fieldWidth = ImGui::CalcItemWidth();

    float pickerBtnWidth = ImGui::GetFrameHeight();
    float nameFieldWidth = fieldWidth - pickerBtnWidth;

    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_FrameBgHovered]);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_FrameBgActive]);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));

    std::string popupId = std::string("AssetPicker##") + prop.name;

    // Left: the asset name; double-click reveals it in the Asset Browser.
    std::string nameBtnId = displayText + "##refname_" + prop.name;
    ImGui::Button(nameBtnId.c_str(), ImVec2(nameFieldWidth, 0));

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && !displayGuid.empty())
    {
        RevealAssetInBrowser(displayGuid);
    }

    // Drop target for assets from the Asset Browser; must follow the button.
    if (ImGui::BeginDragDropTarget())
    {
        // Runs the drop as commands that one Undo reverts. The width and height
        // it would set are found by applying them, recording the result and
        // restoring the old values, so the commands make the real change.
        auto applyDrop =
            [&](const std::string& newGuid, const std::string& newSource, const std::string& dimGuid, bool autoPopulate)
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
                    {
                        commands.push_back(std::make_unique<ModifyFieldCommand<int32_t>>(widthPtr, newWidth, "width"));
                    }
                    if (newHeight != oldHeight)
                    {
                        commands.push_back(
                            std::make_unique<ModifyFieldCommand<int32_t>>(heightPtr, newHeight, "height"));
                    }
                }
            }

            CommandHistory::Instance().ExecuteNoMerge(std::make_unique<CompoundCommand>(
                CommandText::Format(Tr("Set %s"), Tr(EditorNaming::GetDisplayName(prop).c_str())),
                std::move(commands)));
        };

        bool handled = false;

        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
        {
            std::string droppedPath(static_cast<const char*>(payload->Data));
            auto* pipeline = m_App->GetAssetPipeline();
            if (pipeline)
            {
                // droppedPath is project-relative ("assets/textures/player.png").
                const AssetInfo* info = pipeline->GetAssetInfo(droppedPath);
                if (info && !info->guid.empty())
                {
                    // A TTF/OTF font is a source asset that gets baked.
                    std::string ext = std::filesystem::path(droppedPath).extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    bool isSourceAsset = (ext == ".ttf" || ext == ".otf");

                    if (isSourceAsset)
                    {
                        applyDrop(/*guid*/ "", /*source*/ info->guid, /*dimGuid*/ "", false);
                    }
                    else
                    {
                        applyDrop(info->guid, "", info->guid, true);
                    }

                    handled = true;
                }
            }
        }

        // ASSET_GUID carries sub-assets (sprite frames) and plain GUIDs. Skipped
        // when ASSET_PATH already handled the drop.
        if (!handled)
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_GUID"))
            {
                std::string droppedGuid(static_cast<const char*>(payload->Data));
                auto* pipeline = m_App->GetAssetPipeline();
                if (pipeline)
                {
                    const AssetInfo* info = pipeline->GetAssetInfoByGuid(droppedGuid);
                    const SubAssetInfo* subAsset = pipeline->GetSubAsset(droppedGuid);

                    if (subAsset)
                    {
                        // Sprite frame: the parent GUID loads the texture, the frame GUID finds the frame.
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
        // Both GUIDs, for debugging.
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

    // Right: the picker button, flush against the name.
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
            // A TTF/OTF font is a source asset that gets baked.
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
                // A source asset goes in `source`; the component editor works
                // out the baked guid.
                assetRefBase.source = selectedGuid;
                assetRefBase.guid.clear();
            }
            else
            {
                const SubAssetInfo* subAsset = pipeline ? pipeline->GetSubAsset(selectedGuid) : nullptr;
                if (subAsset)
                {
                    // Sprite frame: the parent GUID loads the texture, the frame GUID finds the frame.
                    assetRefBase.guid = subAsset->parentGuid;
                    assetRefBase.source = selectedGuid;
                }
                else
                {
                    assetRefBase.guid = selectedGuid;
                    assetRefBase.source.clear();
                }

                // Width and height from the asset's size.
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
