#ifdef DEKI_EDITOR

#include <deki-editor/EditorExtension.h>
#include <deki-editor/EditorTheme.h>
#include <deki-editor/EditorRegistry.h>
#include <deki-editor/AssetDatabase.h>
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/Localization.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <string>
#include <sstream>
#include <vector>

using namespace DekiEditor;

// Editor for .dpack (Asset Pack) files: lists of assets bundled into one
// .pack binary, for fast loading from an SD card.
class AssetPackEditor : public AssetTypeEditor
{
public:
    const char* GetTypeName() const override { return "AssetPack"; }
    const char* GetDisplayName() const override { return "Asset Pack"; }
    const char* GetExtension() const override { return ".dpack"; }

    const char* GetDefaultContent() const override
    {
        return R"({
  "type": "AssetPack",
  "assets": []
})";
    }

    int GetCompileTarget() const override { return 3; }  // None: AssetPacker does the packing

    bool Compile(const std::string& jsonData, std::vector<uint8_t>& rgba, int& width, int& height) override
    {
        return false;  // not a texture target
    }

    bool OnInspectorGUI(std::string& jsonData, const std::string& assetPath, const std::string& assetGuid) override
    {
        bool modified = false;

        nlohmann::json data;
        try
        {
            data = nlohmann::json::parse(jsonData);
        }
        catch (...)
        {
            ImGui::TextColored(DekiEditor::Palette::Red, "%s", DekiEditor::Tr("Invalid JSON"));
            return false;
        }

        if (!data.contains("assets") || !data["assets"].is_array())
        {
            data["assets"] = nlohmann::json::array();
            modified = true;
        }

        auto& assets = data["assets"];

        ImGui::Text(DekiEditor::Tr("Assets in pack: %d"), (int)assets.size());

        // Estimated total size.
        ImGui::SameLine();
        ImGui::TextDisabled("%s", DekiEditor::Tr("(exported as single .pack file)"));

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        int removeIndex = -1;
        for (size_t i = 0; i < assets.size(); i++)
        {
            auto& entry = assets[i];
            std::string guid = entry.value("guid", "???");
            std::string type = entry.value("type", "Unknown");

            ImGui::PushID(static_cast<int>(i));

            // The display name, from the GUID.
            std::string displayName;
            std::string assetPath2 = AssetDatabase::GUIDToAssetPath(guid);
            if (!assetPath2.empty())
            {
                // The file name without extension.
                size_t lastSlash = assetPath2.find_last_of("/\\");
                size_t dotPos = assetPath2.find_last_of('.');
                if (lastSlash != std::string::npos && dotPos != std::string::npos && dotPos > lastSlash)
                {
                    displayName = assetPath2.substr(lastSlash + 1, dotPos - lastSlash - 1);
                }
                else if (lastSlash != std::string::npos)
                {
                    displayName = assetPath2.substr(lastSlash + 1);
                }
                else
                {
                    displayName = assetPath2;
                }
            }
            else
            {
                displayName = guid.length() > 12 ? guid.substr(0, 12) + "..." : guid;
            }

            ImGui::Text("  %s  [%s]", displayName.c_str(), type.c_str());

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
            if (ImGui::SmallButton("X"))
            {
                removeIndex = static_cast<int>(i);
            }

            ImGui::PopID();
        }

        if (removeIndex >= 0)
        {
            assets.erase(assets.begin() + removeIndex);
            modified = true;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Drop zone for adding assets.
        ImGui::TextUnformatted(DekiEditor::Tr("Drag assets here to add to pack:"));

        ImVec2 dropSize(ImGui::GetContentRegionAvail().x, 60);
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##DropZone", dropSize);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImU32 borderColor = ImGui::IsItemHovered() ? IM_COL32(100, 180, 255, 200) : IM_COL32(128, 128, 128, 128);
        drawList->AddRect(cursorPos, ImVec2(cursorPos.x + dropSize.x, cursorPos.y + dropSize.y), borderColor, 4.0f, 0,
                          2.0f);

        const char* dropText = DekiEditor::Tr("Drop Asset Here");
        ImVec2 textSize = ImGui::CalcTextSize(dropText);
        drawList->AddText(
            ImVec2(cursorPos.x + (dropSize.x - textSize.x) * 0.5f, cursorPos.y + (dropSize.y - textSize.y) * 0.5f),
            IM_COL32(128, 128, 128, 200), dropText);

        // Adds an asset (GUID and path) to the pack.
        auto tryAddAsset = [&](const std::string& guid, const std::string& path)
        {
            if (guid.empty())
            {
                return;
            }

            // Skip duplicates.
            for (const auto& existing : assets)
            {
                if (existing.value("guid", "") == guid)
                {
                    return;
                }
            }

            // The type, from the path's extension.
            std::string assetType = "Unknown";
            size_t dotPos = path.find_last_of('.');
            if (dotPos != std::string::npos)
            {
                std::string ext = path.substr(dotPos);
                assetType = AssetPipeline::GetAssetTypeFromExtension(ext);
                if (assetType.empty())
                {
                    assetType = "Unknown";
                }
            }

            nlohmann::json newAsset;
            newAsset["guid"] = guid;
            newAsset["type"] = assetType;
            assets.push_back(newAsset);
            modified = true;
        };

        if (ImGui::BeginDragDropTarget())
        {
            // Several assets (a drag of a multi-selection).
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MULTI"))
            {
                std::string multiData(static_cast<const char*>(payload->Data));
                std::istringstream stream(multiData);
                std::string line;
                while (std::getline(stream, line))
                {
                    if (line.empty())
                    {
                        continue;
                    }
                    size_t tabPos = line.find('\t');
                    if (tabPos == std::string::npos)
                    {
                        continue;
                    }

                    std::string guid = line.substr(0, tabPos);
                    std::string path = line.substr(tabPos + 1);

                    // No GUID: look it up from the path.
                    if (guid.empty())
                    {
                        guid = AssetDatabase::AssetPathToGUID(path);
                    }

                    tryAddAsset(guid, path);
                }
            }
            // One asset (ASSET_PATH).
            else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PATH"))
            {
                std::string droppedPath(static_cast<const char*>(payload->Data));
                std::string guid = AssetDatabase::AssetPathToGUID(droppedPath);
                tryAddAsset(guid, droppedPath);
            }
            // A sub-asset (ASSET_GUID).
            else if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_GUID"))
            {
                std::string guid(static_cast<const char*>(payload->Data));
                std::string path = AssetDatabase::GUIDToAssetPath(guid);
                tryAddAsset(guid, path);
            }
            ImGui::EndDragDropTarget();
        }

        if (modified)
        {
            jsonData = data.dump(2);
        }

        return modified;
    }
};

REGISTER_EDITOR(AssetPackEditor)

#endif  // DEKI_EDITOR
