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

static int BytesPerTexel(TextureFormat f)
{
    switch (f)
    {
        case TextureFormat::RGB888: return 3;
        case TextureFormat::RGBA8888: return 4;
        case TextureFormat::RGB565: return 2;
        case TextureFormat::RGB565A8: return 3;
        case TextureFormat::ALPHA8: return 1;
    }
    return 3;
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
    // Start the scroll child at the panel's left edge (cancel the inspector
    // body indent) so the section headers full-bleed like the component
    // windows; the indent is re-applied inside for the content.
    ImGui::SetCursorPosX(0.0f);
    ImGui::BeginChild("TextureInspectorScroll", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
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

        // Formats, as Unity's platform tabs: Default holds the Format every
        // target uses, and each target's tab can override it. The tab bar
        // scrolls and lists every tab under its arrow, so a project with many
        // targets is still one strip.
        if (hasCached && m_App->GetProject())
        {
            const std::vector<const PlatformConfig*> targets = ProjectPlatforms(m_App->GetProject()->GetPath());
            const std::string assetFormat = m_SelectedFormatIndex > 0 ? s_FormatNames[m_SelectedFormatIndex] : "";
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
            auto indexOf = [](TextureFormat f) {
                for (int i = 1; i < s_FormatCount; ++i)
                    if (TextureFormatName(f) == std::string(s_FormatNames[i]))
                        return i;
                return 0;
            };

            ImGui::Spacing();
            if (ImGui::BeginTabBar("##formatTargets",
                                   ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton))
            {
                if (ImGui::BeginTabItem("Default"))
                {
                    InspectorUtils::BeginPropertyRow("Format");
                    if (DekiEditor::SchematicCombo("##Format", &m_SelectedFormatIndex, s_FormatNames, s_FormatCount))
                    {
                        writeTextureSettings([&](nlohmann::json& tex)
                                             { tex["format"] = s_FormatNames[m_SelectedFormatIndex]; });
                    }
                    ImGui::TextDisabled(m_SelectedFormatIndex == 0
                                            ? "Automatic: each target stores it in its screen's format."
                                            : "Every target stores it in this format, unless its tab overrides it.");
                    ImGui::EndTabItem();
                }

                for (const PlatformConfig* platform : targets)
                {
                    const std::string name = platform->displayName.empty() ? platform->id : platform->displayName;
                    int& index = m_TargetFormatIndex[platform->id];
                    const std::string tabLabel = name + (index > 0 ? " *" : "") + "###" + platform->id;
                    if (!ImGui::BeginTabItem(tabLabel.c_str()))
                        continue;

                    Deki::ColorFormat screen = Deki::ColorFormat::RGB565;
                    const bool knownScreen = ParseColorFormat(platform->colorFormat, screen);
                    const TextureFormat fromDefault = ResolveTextureFormat(assetFormat, "", screen, m_SourceHasAlpha);

                    bool overridden = index > 0;
                    InspectorUtils::BeginPropertyRow(("Override for " + name).c_str());
                    if (DekiEditor::SchematicCheckbox("##override", &overridden))
                    {
                        // Turning it on starts at what the target stores now, so
                        // nothing changes until a format is picked.
                        index = overridden ? indexOf(fromDefault) : 0;
                        writeOverride(platform->id, index);
                    }

                    // Not overridden: greyed, showing what the Default gives.
                    InspectorUtils::BeginPropertyRow("Format");
                    int shown = (overridden ? index : indexOf(fromDefault)) - 1;
                    ImGui::BeginDisabled(!overridden);
                    if (DekiEditor::SchematicCombo("##TargetFormat", &shown, s_FormatNames + 1, s_FormatCount - 1) &&
                        overridden)
                    {
                        index = shown + 1;
                        writeOverride(platform->id, index);
                    }
                    ImGui::EndDisabled();
                    // What it costs there: the stored format at the image's size.
                    if (knownScreen && m_PreviewWidth > 0 && m_PreviewHeight > 0)
                    {
                        TextureFormat f = fromDefault;
                        if (overridden)
                            ParseTextureFormat(s_FormatNames[index], f);
                        const double kb = static_cast<double>(m_PreviewWidth) * m_PreviewHeight * BytesPerTexel(f) / 1024.0;
                        ImGui::TextDisabled("%.2f KB on this target (%s screen)", kb, platform->colorFormat.c_str());
                    }
                    ImGui::EndTabItem();
                }

                // Overrides for targets this project no longer has, so they
                // can be removed.
                for (auto& [id, index] : m_TargetFormatIndex)
                {
                    if (index <= 0)
                        continue;
                    bool known = false;
                    for (const PlatformConfig* platform : targets)
                        known = known || platform->id == id;
                    if (known)
                        continue;
                    const std::string tabLabel = id + " (removed) *###" + id;
                    if (!ImGui::BeginTabItem(tabLabel.c_str()))
                        continue;
                    ImGui::TextDisabled("This target is not in the project any more.");
                    if (DekiEditor::SchematicAddButton(ICON_TI_X "  Remove override"))
                    {
                        const std::string gone = id;
                        index = 0;
                        writeOverride(gone, 0);
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        DekiEditor::SchematicSectionEnd();
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
