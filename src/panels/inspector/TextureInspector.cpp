#include "TextureInspector.h"
#include <deki-editor/EditorTheme.h>
#include "InspectorUtils.h"
#include "../../EditorApp.h"
#include <deki-editor/TextureImporter.h>
#include <deki-editor/AssetData.h>
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetDatabase.h>
#include <deki-editor/TextureFormatResolve.h>
#include <deki-editor/Paths.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/build/TargetBuilder.h>
#include <deki-editor/build/BuilderRegistry.h>
#include "../../core/Project.h"
#include "../../services/ToolHost.h"
#include "imgui.h"
#include <deki-editor/IconsTabler.h>
#include <SDL3/SDL_opengl.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <vector>

namespace DekiEditor
{

// Index 0 is Automatic: follow the target's screen. The rest are stored as named.
static const char* s_FormatNames[] = { "Automatic", "RGB888", "RGBA8888", "RGB565", "RGB565A8", "ALPHA8" };
static const int s_FormatCount = 6;

static int FormatNameToIndex(const std::string& name)
{
    for (int i = 1; i < s_FormatCount; i++)
    {
        if (name == s_FormatNames[i])
            return i;
    }
    return 0;  // absent, "Automatic" or unknown: Automatic
}

// The platforms this project has its own copy of (platforms/<id>/), in the
// registry's order. Those are its targets.
static std::vector<const PlatformConfig*> ProjectPlatforms(const std::string& projectPath)
{
    namespace fs = std::filesystem;
    std::vector<const PlatformConfig*> out;
    const std::string dir = GetPlatformsDirectory(projectPath);
    for (const auto& [id, name] : PlatformRegistry::Instance().GetPlatformDisplayList())
    {
        std::error_code ec;
        if (!fs::is_directory(fs::path(dir) / id, ec))
            continue;
        if (const PlatformConfig* p = PlatformRegistry::Instance().GetPlatform(id))
            out.push_back(p);
    }
    return out;
}

TextureInspector::TextureInspector(EditorApp* app)
    : m_App(app)
{
}

TextureInspector::~TextureInspector()
{
    CleanupPreviewTexture();
}

void TextureInspector::DrawPreview()
{
    const std::string& cachePath = m_App->GetSelectedAssetCachePath();
    bool hasCached = m_App->GetSelectedAssetHasCached();
    const std::string& guid = m_App->GetSelectedAssetGuid();

    if (!hasCached || cachePath.empty())
    {
        if (m_PreviewTextureId != 0)
            CleanupPreviewTexture();
        return;
    }

    UpdatePreviewTexture(cachePath);

    if (m_PreviewTextureId != 0)
    {
        float availWidth = ImGui::GetContentRegionAvail().x;

        float fWidth, fHeight;
        float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;

        const SpriteFrameData* frameData = TextureImporter::GetFrameData(guid);
        if (frameData)
        {
            fWidth = static_cast<float>(frameData->width);
            fHeight = static_cast<float>(frameData->height);
            u0 = static_cast<float>(frameData->x) / m_PreviewWidth;
            v0 = static_cast<float>(frameData->y) / m_PreviewHeight;
            u1 = static_cast<float>(frameData->x + frameData->width) / m_PreviewWidth;
            v1 = static_cast<float>(frameData->y + frameData->height) / m_PreviewHeight;
        }
        else
        {
            fWidth = static_cast<float>(m_PreviewWidth);
            fHeight = static_cast<float>(m_PreviewHeight);
        }

        int srcWidth = static_cast<int>(fWidth);
        int srcHeight = static_cast<int>(fHeight);
        int availWidthInt = static_cast<int>(availWidth);

        // Bound the preview by the available width AND a maximum height, so
        // small sources (e.g. a 64x64 procedural sprite) don't integer-upscale
        // to fill the whole panel
        float maxHeight = m_App->Dpi(Metrics::PreviewMaxHeight);
        int maxHeightInt = static_cast<int>(maxHeight);

        // Scale up using integer multiples within both bounds (pixel-perfect)
        // Scale down proportionally if larger than either bound
        int displayWidth, displayHeight;
        if (srcWidth <= availWidthInt && srcHeight <= maxHeightInt)
        {
            int scale = 1;
            while ((srcWidth * (scale + 1)) <= availWidthInt &&
                   (srcHeight * (scale + 1)) <= maxHeightInt)
                scale++;
            displayWidth = srcWidth * scale;
            displayHeight = srcHeight * scale;
        }
        else
        {
            float scale = availWidth / fWidth;
            float scaleH = maxHeight / fHeight;
            if (scaleH < scale) scale = scaleH;
            displayWidth = static_cast<int>(fWidth * scale);
            displayHeight = static_cast<int>(fHeight * scale);
        }
        if (displayWidth < 1) displayWidth = 1;
        if (displayHeight < 1) displayHeight = 1;

        int cursorX = (availWidthInt - displayWidth) / 2;
        if (cursorX > 0)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<float>(cursorX));

        ImVec2 imagePos = ImGui::GetCursorScreenPos();
        ImGui::Image((ImTextureID)(intptr_t)m_PreviewTextureId,
                     ImVec2(static_cast<float>(displayWidth), static_cast<float>(displayHeight)),
                     ImVec2(u0, v0), ImVec2(u1, v1));

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRect(imagePos,
                          ImVec2(imagePos.x + displayWidth, imagePos.y + displayHeight),
                          IM_COL32(128, 128, 128, 255), 0.0f, 0, 1.0f);

        ImGui::TextDisabled("%d x %d", srcWidth, srcHeight);
    }
}

void TextureInspector::BeginScrollRegion()
{
    // Negative height reserves space at the bottom for the cache status bar.
    // Start the scroll child at the panel's left edge (cancel the inspector
    // body indent) so the section headers full-bleed like the component
    // windows; the indent is re-applied inside for the content.
    float reservedHeight = GetCacheStatusBarHeight();
    ImGui::SetCursorPosX(0.0f);
    ImGui::BeginChild("TextureInspectorScroll", ImVec2(0, -reservedHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    ImGui::Indent(m_App->Dpi(Metrics::InspectorIndent));
}

void TextureInspector::EndScrollRegion()
{
    ImGui::Unindent(m_App->Dpi(Metrics::InspectorIndent));
    ImGui::EndChild();
}

void TextureInspector::DrawSettings()
{
    const std::string& assetPath = m_App->GetSelectedAssetPath();
    const std::string& cachePath = m_App->GetSelectedAssetCachePath();
    bool hasCached = m_App->GetSelectedAssetHasCached();

    namespace fs = std::filesystem;

    // Every section in the asset inspector opens the way a component does in the
    // object inspector: a full-bleed hairline, then the band hanging off it.
    DekiEditor::FullBleedSeparator();
    if (DekiEditor::SchematicSectionBegin("Settings"))
    {
        // Load format from .data sidecar when asset selection changes.
        // When the sidecar lacks an explicit format, fall back to the format
        // that was actually baked into the .tex cache (the auto-default in
        // ImageCacheHandler picks RGB565 vs RGB565A8 based on alpha content).
        if (assetPath != m_LastFormatAssetPath)
        {
            m_LastFormatAssetPath = assetPath;
            m_SelectedFormatIndex = 0;  // Automatic
            m_TargetFormatIndex.clear();
            m_SourceHasAlpha = false;
            m_ChromaKeyEnabled = false;
            m_ChromaKeyColor[0] = 1.0f;
            m_ChromaKeyColor[1] = 0.0f;
            m_ChromaKeyColor[2] = 1.0f;

            std::string dataPath = assetPath + ".data";
            if (fs::exists(dataPath))
            {
                std::ifstream dataFile(dataPath);
                if (dataFile.is_open())
                {
                    try
                    {
                        nlohmann::json dataJson = nlohmann::json::parse(dataFile);
                        if (dataJson.contains("settings") &&
                            dataJson["settings"].contains("texture"))
                        {
                            auto& tex = dataJson["settings"]["texture"];
                            if (tex.contains("format") && tex["format"].is_string())
                                m_SelectedFormatIndex = FormatNameToIndex(tex["format"].get<std::string>());
                            if (tex.contains("targets") && tex["targets"].is_object())
                            {
                                for (auto it = tex["targets"].begin(); it != tex["targets"].end(); ++it)
                                    if (it.value().is_string())
                                        m_TargetFormatIndex[it.key()] = FormatNameToIndex(it.value().get<std::string>());
                            }
                            if (tex.contains("chroma_key"))
                            {
                                auto& ck = tex["chroma_key"];
                                m_ChromaKeyEnabled = ck.value("enabled", false);
                                m_ChromaKeyColor[0] = ck.value("r", 255) / 255.0f;
                                m_ChromaKeyColor[1] = ck.value("g", 0) / 255.0f;
                                m_ChromaKeyColor[2] = ck.value("b", 255) / 255.0f;
                            }
                        }
                    }
                    catch (const nlohmann::json::exception&)
                    {
                    }
                }
            }

            // Whether the source has alpha, which is what Automatic turns on.
            if (hasCached && !cachePath.empty())
            {
                TexData texData;
                if (TextureImporter::ReadTexFile(cachePath, texData))
                    m_SourceHasAlpha = texData.hasAlpha();
            }
        }

        // Helper: read existing .data, apply mutate, write back, reimport.
        auto writeTextureSettings = [&](auto mutate)
        {
            std::string dataPath = assetPath + ".data";
            nlohmann::json dataJson;
            if (fs::exists(dataPath))
            {
                std::ifstream dataFile(dataPath);
                if (dataFile.is_open())
                {
                    try
                    {
                        dataJson = nlohmann::json::parse(dataFile);
                    }
                    catch (const nlohmann::json::exception&)
                    {
                    }
                }
            }
            mutate(dataJson["settings"]["texture"]);
            std::ofstream outFile(dataPath);
            if (outFile.is_open())
                outFile << dataJson.dump(2);
            AssetDatabase::ImportAsset(assetPath);
            InvalidatePreview();
        };

        // Format: Automatic (each target's screen decides) or one for every target.
        if (hasCached)
        {
            InspectorUtils::BeginPropertyRow("Format");
            if (DekiEditor::SchematicCombo("##Format", &m_SelectedFormatIndex, s_FormatNames, s_FormatCount))
            {
                writeTextureSettings([&](nlohmann::json& tex)
                                     { tex["format"] = s_FormatNames[m_SelectedFormatIndex]; });
            }
        }

        // Targets: what each stores, and the overrides that exist. A project
        // can have many targets (boards, Tactility, a PC, a phone), so only
        // the overrides are listed, with a summary of the rest.
        if (hasCached && m_App->GetProject())
        {
            const std::string assetFormat = m_SelectedFormatIndex > 0 ? s_FormatNames[m_SelectedFormatIndex] : "";
            const std::vector<const PlatformConfig*> targets = ProjectPlatforms(m_App->GetProject()->GetPath());

            // Stored as: each format and how many targets store it.
            std::map<std::string, int> storedCounts;
            for (const PlatformConfig* platform : targets)
            {
                Deki::ColorFormat screen;
                if (!ParseColorFormat(platform->colorFormat, screen))
                    continue;
                auto it = m_TargetFormatIndex.find(platform->id);
                const std::string targetFormat =
                    (it != m_TargetFormatIndex.end() && it->second > 0) ? s_FormatNames[it->second] : "";
                ++storedCounts[TextureFormatName(ResolveTextureFormat(assetFormat, targetFormat, screen, m_SourceHasAlpha))];
            }
            if (!storedCounts.empty())
            {
                std::string summary;
                for (const auto& [format, count] : storedCounts)
                {
                    if (!summary.empty())
                        summary += "  ·  ";
                    summary += format + " on " + std::to_string(count) + (count == 1 ? " target" : " targets");
                }
                InspectorUtils::BeginPropertyRow("Stored as");
                ImGui::TextWrapped("%s", summary.c_str());
            }

            auto findTarget = [&](const std::string& id) -> const PlatformConfig* {
                for (const PlatformConfig* platform : targets)
                    if (platform->id == id)
                        return platform;
                return nullptr;
            };
            auto writeOverride = [&](const std::string& id, int chosen) {
                writeTextureSettings([&](nlohmann::json& tex) {
                    if (chosen <= 0)
                    {
                        if (tex.contains("targets"))
                            tex["targets"].erase(id);
                    }
                    else
                        tex["targets"][id] = s_FormatNames[chosen];
                });
            };

            // The overrides that exist, under one line that opens to a row
            // each; open by default while there are few.
            int overrideCount = 0;
            for (const auto& [id, index] : m_TargetFormatIndex)
                if (index > 0)
                    ++overrideCount;
            std::string removeId;
            bool showOverrides = false;
            if (overrideCount > 0)
            {
                const std::string header = "Overrides (" + std::to_string(overrideCount) + ")###overrides";
                ImGui::SetNextItemOpen(overrideCount <= 3, ImGuiCond_Appearing);
                showOverrides = ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
            }
            for (auto& [id, index] : m_TargetFormatIndex)
            {
                if (!showOverrides || index <= 0)
                    continue;
                ImGui::PushID(id.c_str());
                const PlatformConfig* platform = findTarget(id);
                const std::string label = platform ? (platform->displayName.empty() ? id : platform->displayName)
                                                   : id + " (not in this project)";
                InspectorUtils::BeginPropertyRow(label.c_str());
                // Explicit formats only here; removing the row is going back to Automatic.
                int choice = index - 1;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() -
                                        ImGui::GetStyle().ItemSpacing.x);
                if (DekiEditor::SchematicCombo("##TargetFormat", &choice, s_FormatNames + 1, s_FormatCount - 1))
                {
                    index = choice + 1;
                    writeOverride(id, index);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_TI_X))
                    removeId = id;
                if (ImGui::IsItemHovered())
                    DekiEditor::Tooltip("Remove the override: this target goes back to the Format above");
                ImGui::PopID();
            }
            if (showOverrides)
                ImGui::TreePop();
            if (!removeId.empty())
            {
                m_TargetFormatIndex.erase(removeId);
                writeOverride(removeId, 0);
            }

            // Add one: the targets without an override.
            if (DekiEditor::SchematicAddButton(ICON_TI_PLUS "  Add override"))
                ImGui::OpenPopup("##addoverride");
            if (ImGui::BeginPopup("##addoverride"))
            {
                if (ImGui::IsWindowAppearing())
                {
                    m_OverrideSearch[0] = '\0';
                    ImGui::SetKeyboardFocusHere();
                }
                ImGui::SetNextItemWidth(260.0f);
                ImGui::InputTextWithHint("##search", "Search targets", m_OverrideSearch, sizeof(m_OverrideSearch));
                std::string needle = m_OverrideSearch;
                std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return std::tolower(c); });

                // Targets without an override, by builder, filtered.
                std::map<std::string, std::vector<const PlatformConfig*>> byBuilder;
                for (const PlatformConfig* platform : targets)
                {
                    auto it = m_TargetFormatIndex.find(platform->id);
                    if (it != m_TargetFormatIndex.end() && it->second > 0)
                        continue;
                    std::string hay = platform->id + " " + platform->displayName;
                    std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return std::tolower(c); });
                    if (!needle.empty() && hay.find(needle) == std::string::npos)
                        continue;
                    std::string builder = platform->framework;
                    if (ITargetBuilder* b = BuilderRegistry::Instance().GetBuilder(platform->framework))
                        builder = b->GetName();
                    if (builder.empty())
                        builder = "Other";
                    byBuilder[builder].push_back(platform);
                }

                ImGui::BeginChild("##targets", ImVec2(260.0f, std::min(320.0f, 40.0f + 24.0f * targets.size())));
                if (byBuilder.empty())
                    ImGui::TextDisabled(needle.empty() ? "Every target has an override" : "No target matches");
                for (const auto& [builder, list] : byBuilder)
                {
                    // Open while searching, or when there is only one builder.
                    ImGui::SetNextItemOpen(!needle.empty() || byBuilder.size() == 1);
                    const std::string header = builder + " (" + std::to_string(list.size()) + ")";
                    if (!ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
                        continue;
                    for (const PlatformConfig* platform : list)
                    {
                        Deki::ColorFormat screen;
                        std::string item = platform->displayName.empty() ? platform->id : platform->displayName;
                        TextureFormat now = TextureFormat::RGB565A8;
                        if (ParseColorFormat(platform->colorFormat, screen))
                        {
                            now = ResolveTextureFormat(assetFormat, "", screen, m_SourceHasAlpha);
                            item += std::string("  (") + TextureFormatName(now) + ")";
                        }
                        if (ImGui::Selectable((item + "##" + platform->id).c_str()))
                        {
                            // Starts at what it stores now, so adding changes
                            // nothing until a format is picked.
                            int index = 0;
                            for (int i = 1; i < s_FormatCount; ++i)
                                if (TextureFormatName(now) == std::string(s_FormatNames[i]))
                                    index = i;
                            m_TargetFormatIndex[platform->id] = index;
                            writeOverride(platform->id, index);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::TreePop();
                }
                ImGui::EndChild();
                ImGui::EndPopup();
            }
        }

        // Chroma key (1-bit transparency). Most useful with RGB565 to avoid
        // paying for an alpha plane when only on/off transparency is needed.
        if (hasCached)
        {
            InspectorUtils::BeginPropertyRow("Chroma Key");
            if (DekiEditor::SchematicCheckbox("##ChromaKeyEnabled", &m_ChromaKeyEnabled))
            {
                writeTextureSettings([&](nlohmann::json& tex)
                                     {
                    tex["chroma_key"]["enabled"] = m_ChromaKeyEnabled;
                    tex["chroma_key"]["r"] = static_cast<int>(m_ChromaKeyColor[0] * 255.0f + 0.5f);
                    tex["chroma_key"]["g"] = static_cast<int>(m_ChromaKeyColor[1] * 255.0f + 0.5f);
                    tex["chroma_key"]["b"] = static_cast<int>(m_ChromaKeyColor[2] * 255.0f + 0.5f); });
            }

            if (m_ChromaKeyEnabled)
            {
                InspectorUtils::BeginPropertyRow("Chroma Color");
                DekiEditor::SchematicColorEdit3("##ChromaKeyColor", m_ChromaKeyColor);
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    writeTextureSettings([&](nlohmann::json& tex)
                                         {
                        tex["chroma_key"]["enabled"] = m_ChromaKeyEnabled;
                        tex["chroma_key"]["r"] = static_cast<int>(m_ChromaKeyColor[0] * 255.0f + 0.5f);
                        tex["chroma_key"]["g"] = static_cast<int>(m_ChromaKeyColor[1] * 255.0f + 0.5f);
                        tex["chroma_key"]["b"] = static_cast<int>(m_ChromaKeyColor[2] * 255.0f + 0.5f); });
                }
            }
        }

        // Open in Spritesheet Editor button
        if (hasCached && !cachePath.empty() && m_PreviewTextureId != 0)
        {
            if (DekiEditor::SchematicAddButton(ICON_TI_LAYOUT_GRID "  Open in Spritesheet Editor"))
            {
                if (auto* toolHost = m_App->GetToolHost())
                    toolHost->OpenToolForFile(assetPath.c_str(), cachePath.c_str());
            }
        }
        DekiEditor::SchematicSectionEnd();
    }
}

void TextureInspector::DrawData()
{
    const std::string& assetPath = m_App->GetSelectedAssetPath();
    const std::string& cachePath = m_App->GetSelectedAssetCachePath();
    bool hasCached = m_App->GetSelectedAssetHasCached();

    namespace fs = std::filesystem;

    TexData texData;
    bool hasTexData = hasCached && !cachePath.empty() && TextureImporter::ReadTexFile(cachePath, texData);

    DekiEditor::FullBleedSeparator();
    if (DekiEditor::SchematicSectionBegin("Data"))
    {
        if (hasTexData)
        {
            InspectorUtils::BeginPropertyRow("Data Size");
            ImGui::Text("%.2f KB", texData.header.dataSize / 1024.0f);

            if (fs::exists(cachePath))
            {
                auto fileSize = fs::file_size(cachePath);
                InspectorUtils::BeginPropertyRow("File Size");
                ImGui::Text("%.2f KB", fileSize / 1024.0f);
            }

            if (texData.hasAlpha())
            {
                InspectorUtils::BeginPropertyRow("Has Alpha");
                ImGui::TextUnformatted("Yes");
            }
            if (texData.hasTransparency())
            {
                InspectorUtils::BeginPropertyRow("Has Transparency");
                ImGui::TextUnformatted("Yes");
            }
        }

        // Source file info
        fs::path path(assetPath);
        if (fs::exists(assetPath))
        {
            auto sourceSize = fs::file_size(assetPath);
            InspectorUtils::BeginPropertyRow("Source");
            ImGui::Text("%s (%.2f KB)", path.filename().string().c_str(), sourceSize / 1024.0f);
        }
        DekiEditor::SchematicSectionEnd();
    }
}

float TextureInspector::GetCacheStatusBarHeight()
{
    return ImGui::GetTextLineHeight() + 4.0f;
}

void TextureInspector::DrawCacheStatus(int overrideCached)
{
    bool hasCached = (overrideCached >= 0) ? (overrideCached != 0) : m_App->GetSelectedAssetHasCached();
    float barHeight = GetCacheStatusBarHeight();
    float padding = 8.0f;

    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();
    ImVec2 barMin(windowPos.x, windowPos.y + windowSize.y - barHeight);
    ImVec2 barMax(windowPos.x + windowSize.x, windowPos.y + windowSize.y);

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Background + hairline, from the palette (this is the same strip the
    // activity status bar draws, and it should read as the same chrome).
    drawList->AddRectFilled(barMin, barMax,
                            ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Panel));
    drawList->AddLine(barMin, ImVec2(barMax.x, barMin.y),
                      ImGui::ColorConvertFloat4ToU32(DekiEditor::Palette::Line2));

    // Centered text
    float textY = barMin.y + (barHeight - ImGui::GetTextLineHeight()) * 0.5f;
    float cursorX = barMin.x + padding;

    const char* label = hasCached ? ICON_TI_CIRCLE_CHECK " Cache" : ICON_TI_CIRCLE_X " Cache";
    ImU32 color = ImGui::ColorConvertFloat4ToU32(hasCached ? DekiEditor::Palette::Green
                                                           : DekiEditor::Palette::Red);
    drawList->AddText(ImVec2(cursorX, textY), color, label);
}

void TextureInspector::DrawCacheStatusBar()
{
    const std::string& cachePath = m_App->GetSelectedAssetCachePath();
    bool hasCached = m_App->GetSelectedAssetHasCached();

    namespace fs = std::filesystem;

    ImGui::Spacing();
    ImGui::Separator();

    if (!hasCached || cachePath.empty())
    {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Cache: not built");
        return;
    }

    // Cache info line
    uintmax_t fileSize = 0;
    try
    {
        if (fs::exists(cachePath)) fileSize = fs::file_size(cachePath);
    }
    catch (...)
    { /* a size probe; the fileSize > 0 check below covers a failure */
    }

    if (fileSize > 0)
    {
        if (fileSize < 1024)
            ImGui::Text("Cache: %llu B", (unsigned long long)fileSize);
        else
            ImGui::Text("Cache: %.1f KB", fileSize / 1024.0f);
    }
    else
    {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Cached");
    }

    // Small thumbnail preview
    UpdatePreviewTexture(cachePath);
    if (m_PreviewTextureId != 0)
    {
        // Scale to fit in a small area (max 64px height)
        float maxH = 64.0f;
        float scale = maxH / static_cast<float>(m_PreviewHeight);
        if (scale > 1.0f) scale = 1.0f;
        float displayW = m_PreviewWidth * scale;
        float displayH = m_PreviewHeight * scale;

        ImGui::Image((ImTextureID)(intptr_t)m_PreviewTextureId,
                     ImVec2(displayW, displayH));
        ImGui::SameLine();
        ImGui::TextDisabled("%d x %d", m_PreviewWidth, m_PreviewHeight);
    }
}

bool TextureInspector::IsTextureFile(const std::string& path)
{
    namespace fs = std::filesystem;
    std::string ext = fs::path(path).extension().string();

    for (char& c : ext)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".bmp" || ext == ".tga" || ext == ".gif";
}

void TextureInspector::UpdatePreviewTexture(const std::string& cachePath)
{
    if (m_PreviewTexturePath == cachePath && m_PreviewTextureId != 0)
        return;

    CleanupPreviewTexture();

    TexData texData;
    if (!TextureImporter::ReadTexFile(cachePath, texData))
    {
        m_PreviewTexturePath = cachePath + " (read failed)";
        return;
    }

    std::vector<uint8_t> rgbaData = TextureImporter::ConvertToRGBA(texData);
    if (rgbaData.empty())
    {
        m_PreviewTexturePath = cachePath + " (convert failed)";
        return;
    }

    GLuint textureId;
    glGenTextures(1, &textureId);
    glBindTexture(GL_TEXTURE_2D, textureId);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Unpack state is global; other GL users (e.g. ImGui glyph uploads) can leave
    // a row stride behind, which would shear/overread this tightly packed upload.
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 texData.header.width, texData.header.height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaData.data());

    glBindTexture(GL_TEXTURE_2D, 0);

    m_PreviewTextureId = textureId;
    m_PreviewTexturePath = cachePath;
    m_PreviewWidth = texData.header.width;
    m_PreviewHeight = texData.header.height;
}

void TextureInspector::CleanupPreviewTexture()
{
    if (m_PreviewTextureId != 0)
    {
        GLuint texId = m_PreviewTextureId;
        glDeleteTextures(1, &texId);
        m_PreviewTextureId = 0;
    }
    m_PreviewTexturePath.clear();
    m_PreviewWidth = 0;
    m_PreviewHeight = 0;
}

}  // namespace DekiEditor
