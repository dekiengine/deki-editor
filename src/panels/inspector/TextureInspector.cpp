#include "TextureInspector.h"
#include <deki-editor/EditorTheme.h>
#include "InspectorUtils.h"
#include "../../EditorApp.h"
#include <deki-editor/TextureImporter.h>
#include <deki-editor/AssetData.h>
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetDatabase.h>
#include <deki-editor/TextureFormatResolve.h>
#include <deki-editor/TextureSettings.h>
#include <deki-editor/Paths.h>
#include <deki-editor/build/PlatformConfig.h>
#include "../../core/Project.h"
#include "../../services/ToolHost.h"
#include "imgui.h"
#include "imgui_internal.h"  // ImGuiSelectableFlags_SpanAvailWidth, as SchematicCombo uses
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

static int MaxSizeToIndex(int maxSize)
{
    for (int i = 0; i < kTextureMaxSizeCount; ++i)
        if (kTextureMaxSizes[i] == maxSize)
            return i;
    // Not one of the choices (written by hand): the largest one within it.
    int best = 0;
    for (int i = 1; i < kTextureMaxSizeCount; ++i)
        if (kTextureMaxSizes[i] <= maxSize)
            best = i;
    return best;
}

static const char* MaxSizeLabel(int index)
{
    static const char* labels[] = { "None", "32", "64", "128", "256", "512", "1024", "2048", "4096", "8192" };
    static_assert(sizeof(labels) / sizeof(labels[0]) == kTextureMaxSizeCount, "one label per Max Size");
    return labels[index];
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

        if (!frameData && (m_StoredWidth != m_PreviewWidth || m_StoredHeight != m_PreviewHeight))
            ImGui::TextDisabled("%d x %d, shown at %u x %u (Max Size)", srcWidth, srcHeight, m_StoredWidth,
                                m_StoredHeight);
        else
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

    // Load format from .data sidecar when asset selection changes.
    // When the sidecar lacks an explicit format, fall back to the format
    // that was actually baked into the .tex cache (the auto-default in
    // ImageCacheHandler picks RGB565 vs RGB565A8 based on alpha content).
    if (assetPath != m_LastFormatAssetPath)
    {
        m_LastFormatAssetPath = assetPath;
        m_SelectedFormatIndex = 0;  // Automatic
        m_TargetFormatIndex.clear();
        m_MaxSizeIndex = 0;  // no limit
        m_TargetMaxIndex.clear();
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
                        const TextureSettings settings = ReadTextureSettings(tex);
                        m_SelectedFormatIndex = FormatNameToIndex(settings.format);
                        m_MaxSizeIndex = MaxSizeToIndex(settings.maxSize);
                        for (const auto& [id, t] : settings.targets)
                        {
                            if (!t.format.empty())
                                m_TargetFormatIndex[id] = FormatNameToIndex(t.format);
                            if (t.maxSize >= 0)
                                m_TargetMaxIndex[id] = MaxSizeToIndex(t.maxSize);
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
        {
            // Closed before the reimport below reads it: an open stream had
            // not written anything yet, so the import saw an empty sidecar.
            std::ofstream outFile(dataPath);
            if (outFile.is_open())
                outFile << dataJson.dump(2);
        }
        // The pipeline keys assets by their project-relative path; the
        // selection holds the absolute one.
        AssetDatabase::ImportAsset(AssetDatabase::GUIDToAssetPath(m_App->GetSelectedAssetGuid()));
        InvalidatePreview();
    };

    // Every section in the asset inspector opens the way a component does in the
    // object inspector: a full-bleed hairline, then the band hanging off it.
    DekiEditor::FullBleedSeparator();
    if (DekiEditor::SchematicSectionBegin("Settings"))
    {
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

    if (hasCached && m_App->GetProject())
        DrawTargets(writeTextureSettings);
}

// Two sections. Default holds what every target uses, as ordinary inspector
// rows. Per Target is a stack of cards, one per target, each overriding any
// of those values for itself: a card folds to its header, which with its
// summary line says what the target stores without opening it. A value that
// follows Default is dimmed and says what that gives the target; a target
// with an override has a dot. Each property is a row, so another one (a
// compression setting, say) is another row, not a narrower control.
void TextureInspector::DrawTargets(const std::function<void(const std::function<void(nlohmann::json&)>&)>& write)
{
    const std::vector<const PlatformConfig*> targets = ProjectPlatforms(m_App->GetProject()->GetPath());
    const std::string assetFormat = m_SelectedFormatIndex > 0 ? s_FormatNames[m_SelectedFormatIndex] : "";

    auto targetFormatIndex = [&](const std::string& id) {
        auto it = m_TargetFormatIndex.find(id);
        return it == m_TargetFormatIndex.end() ? 0 : it->second;
    };
    auto targetMaxIndex = [&](const std::string& id) {
        auto it = m_TargetMaxIndex.find(id);
        return it == m_TargetMaxIndex.end() ? -1 : it->second;
    };
    // A target's values, written together as its entry.
    auto writeTarget = [&](const std::string& id) {
        TextureTargetSettings t;
        const int fi = targetFormatIndex(id);
        if (fi > 0)
            t.format = s_FormatNames[fi];
        const int mi = targetMaxIndex(id);
        if (mi >= 0)
            t.maxSize = kTextureMaxSizes[mi];
        write([&](nlohmann::json& tex) { WriteTextureTarget(tex, id, t); });
    };
    // A dropdown whose closed box shows `preview` (dimmed when it comes from
    // Default) and whose list starts with "Default (...)" (index
    // firstIndex - 1). Returns true when `index` changed.
    auto overrideCombo = [&](const char* id, float width, const char* preview, bool fromDefault,
                             const std::string& defaultLabel, const char* (*label)(int), int count, int firstIndex,
                             int& index) {
        ImGui::SetNextItemWidth(width);
        if (fromDefault)
            ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
        const bool open = DekiEditor::SchematicBeginCombo(id, preview);
        if (fromDefault)
            ImGui::PopStyleColor();
        bool changed = false;
        if (open)
        {
            if (ImGui::Selectable(defaultLabel.c_str(), fromDefault, ImGuiSelectableFlags_SpanAvailWidth) && !fromDefault)
            {
                index = firstIndex - 1;
                changed = true;
            }
            for (int i = firstIndex; i < count; ++i)
            {
                const bool selected = !fromDefault && i == index;
                if (ImGui::Selectable(label(i), selected, ImGuiSelectableFlags_SpanAvailWidth) && !selected)
                {
                    index = i;
                    changed = true;
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            DekiEditor::SchematicEndCombo();
        }
        return changed;
    };
    static const char* (*formatLabel)(int) = [](int i) { return s_FormatNames[i]; };
    static const char* (*maxLabel)(int) = [](int i) { return MaxSizeLabel(i); };
    char buf[96];

    // ---- Default ----
    DekiEditor::FullBleedSeparator();
    if (DekiEditor::SchematicSectionBegin("Default"))
    {
        InspectorUtils::SetNextRowTooltip("How every target stores the image, unless its card below overrides it. "
                                          "Automatic uses each target's screen format.");
        InspectorUtils::BeginPropertyRow("Format");
        if (DekiEditor::SchematicCombo("##Format", &m_SelectedFormatIndex, s_FormatNames, s_FormatCount))
            write([&](nlohmann::json& tex) { tex["format"] = s_FormatNames[m_SelectedFormatIndex]; });

        InspectorUtils::SetNextRowTooltip("An image whose larger side is over this is stored scaled down so that side "
                                          "is exactly this. None never shrinks it.");
        InspectorUtils::BeginPropertyRow("Max Size");
        const char* items[kTextureMaxSizeCount];
        for (int i = 0; i < kTextureMaxSizeCount; ++i)
            items[i] = MaxSizeLabel(i);
        if (DekiEditor::SchematicCombo("##MaxSize", &m_MaxSizeIndex, items, kTextureMaxSizeCount))
            write([&](nlohmann::json& tex) {
                if (m_MaxSizeIndex == 0)
                    tex.erase("maxSize");
                else
                    tex["maxSize"] = kTextureMaxSizes[m_MaxSizeIndex];
            });
        DekiEditor::SchematicSectionEnd();
    }

    // ---- Per Target ----
    DekiEditor::FullBleedSeparator();
    if (!DekiEditor::SchematicSectionBegin("Per Target"))
        return;

    const float dpi = m_App->Dpi(1.0f);
    const float cardGap = 8.0f * dpi;
    const float pad = 10.0f * dpi;
    const float rounding = 3.0f * dpi;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // One card. `body` draws its rows (only when open); returns whether the
    // header was clicked.
    auto card = [&](const char* id, bool dot, const std::string& name, bool dimName, const char* note,
                    const char* noteTip, const char* nameTip, const std::string& summary, bool& open,
                    const std::function<void(float labelX, float valueX, float valueW)>& body) {
        ImGui::PushID(id);
        const float left = ImGui::GetCursorScreenPos().x;
        const float right = left + ImGui::GetContentRegionAvail().x - m_App->Dpi(Metrics::InspectorRightPad);
        const float innerL = left + pad, innerR = right - pad;
        const ImVec2 top = ImGui::GetCursorScreenPos();

        ImDrawListSplitter split;
        split.Split(dl, 2);
        split.SetCurrentChannel(dl, 1);

        // Header: chevron, dot, name, and the note on the right. The whole
        // header is the fold toggle.
        const float headerH = ImGui::GetFrameHeight();
        ImGui::SetCursorScreenPos(ImVec2(left, top.y + pad * 0.5f));
        if (ImGui::InvisibleButton("##header", ImVec2(right - left, headerH)))
            open = !open;
        const bool headerHovered = ImGui::IsItemHovered();
        const float textY = top.y + pad * 0.5f + (headerH - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText(ImVec2(innerL, textY), ImGui::GetColorU32(Palette::Dim),
                    open ? ICON_TI_CHEVRON_DOWN : ICON_TI_CHEVRON_RIGHT);
        float x = innerL + ImGui::CalcTextSize(ICON_TI_CHEVRON_DOWN).x + 6.0f * dpi;
        if (dot)
        {
            dl->AddText(ImVec2(x, textY), ImGui::GetColorU32(Palette::Accent), ICON_TI_POINT_FILLED);
            x += ImGui::CalcTextSize(ICON_TI_POINT_FILLED).x + 3.0f * dpi;
        }
        const float noteW = note ? ImGui::CalcTextSize(note).x : 0.0f;
        const float nameRoom = innerR - x - (note ? noteW + 8.0f * dpi : 0.0f);
        std::string shown = name;
        if (ImGui::CalcTextSize(shown.c_str()).x > nameRoom)
        {
            while (!shown.empty() && ImGui::CalcTextSize((shown + "...").c_str()).x > nameRoom)
                shown.pop_back();
            shown += "...";
        }
        dl->AddText(ImVec2(x, textY), ImGui::GetColorU32(dimName ? Palette::Dim : Palette::Fg), shown.c_str());
        if (note)
            dl->AddText(ImVec2(innerR - noteW, textY), ImGui::GetColorU32(Palette::Dim), note);
        if (headerHovered)
        {
            const float mx = ImGui::GetIO().MousePos.x;
            if (note && noteTip && mx >= innerR - noteW)
                DekiEditor::Tooltip(noteTip);
            else if (nameTip)
                DekiEditor::Tooltip(nameTip);
        }

        const float labelX = innerL + ImGui::CalcTextSize(ICON_TI_CHEVRON_DOWN).x + 6.0f * dpi;
        if (!open)
        {
            // Folded: one dim line saying what the target stores.
            if (!summary.empty())
            {
                ImGui::SetCursorScreenPos(ImVec2(labelX, ImGui::GetCursorScreenPos().y));
                ImGui::TextDisabled("%s", summary.c_str());
            }
        }
        else
        {
            // Controls line up with the Default section's, one value column
            // down the inspector (never closer than a label's width).
            const float sectionLabelX = InspectorUtils::SectionAlignedCursorX();
            const float columnX = ImGui::GetWindowPos().x + sectionLabelX + InspectorUtils::PropertyLabelWidth(sectionLabelX);
            const float valueX = std::max(labelX + 70.0f * dpi, columnX);
            body(labelX, valueX, std::max(40.0f * dpi, innerR - valueX));
        }
        ImGui::SetCursorScreenPos(ImVec2(left, ImGui::GetCursorScreenPos().y));
        ImGui::Dummy(ImVec2(0.0f, pad * 0.5f));
        const float bottom = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;

        // The card behind it, now that its height is known.
        split.SetCurrentChannel(dl, 0);
        dl->AddRectFilled(top, ImVec2(right, bottom),
                          ImGui::GetColorU32(headerHovered ? Palette::Bg2 : Palette::Panel), rounding);
        dl->AddRect(top, ImVec2(right, bottom), ImGui::GetColorU32(Palette::Line2), rounding);
        split.Merge(dl);

        ImGui::SetCursorScreenPos(ImVec2(left, bottom + cardGap));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PopID();
    };
    // A row inside a card: the label, then the control at valueX.
    auto cardRow = [&](const char* label, float labelX, float valueX) {
        ImGui::SetCursorScreenPos(ImVec2(labelX, ImGui::GetCursorScreenPos().y));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetCursorScreenPos(ImVec2(valueX, ImGui::GetCursorScreenPos().y));
    };

    ImGui::Spacing();
    for (const PlatformConfig* platform : targets)
    {
        const std::string name = platform->displayName.empty() ? platform->id : platform->displayName;
        int formatIndex = targetFormatIndex(platform->id);
        int maxIndex = targetMaxIndex(platform->id);
        const bool formatOverridden = formatIndex > 0;
        const bool maxOverridden = maxIndex >= 0;
        const bool overridden = formatOverridden || maxOverridden;

        Deki::ColorFormat screen = Deki::ColorFormat::RGB565;
        const bool knownScreen = ParseColorFormat(platform->colorFormat, screen);
        const TextureFormat fromDefault = ResolveTextureFormat(assetFormat, "", screen, m_SourceHasAlpha);
        TextureFormat stored = fromDefault;
        if (formatOverridden)
            ParseTextureFormat(s_FormatNames[formatIndex], stored);
        const int shownMax = maxOverridden ? maxIndex : m_MaxSizeIndex;

        // What it takes there: the stored format at the stored size.
        char memory[32] = "";
        char memoryTip[48] = "";
        if (m_PreviewWidth > 0 && m_PreviewHeight > 0)
        {
            int w = 0, h = 0;
            MaxSizeDims(static_cast<int>(m_PreviewWidth), static_cast<int>(m_PreviewHeight),
                        kTextureMaxSizes[shownMax], w, h);
            snprintf(memory, sizeof(memory), "%.1f KB", static_cast<double>(w) * h * BytesPerTexel(stored) / 1024.0);
            snprintf(memoryTip, sizeof(memoryTip), "Stored at %d x %d", w, h);
        }
        std::string nameTip = name;
        if (knownScreen)
            nameTip += "\n" + platform->colorFormat + " screen";
        if (overridden)
            nameTip += "\nOverrides Default";
        const std::string summary = std::string(TextureFormatName(stored)) + "  \xC2\xB7  Max Size " +
                                    MaxSizeLabel(shownMax);

        auto openIt = m_TargetOpen.find(platform->id);
        if (openIt == m_TargetOpen.end())
            openIt = m_TargetOpen.emplace(platform->id, overridden).first;

        card(platform->id.c_str(), overridden, name, false, memory[0] ? memory : nullptr, memoryTip, nameTip.c_str(),
             summary, openIt->second, [&](float labelX, float valueX, float valueW) {
                 cardRow("Format", labelX, valueX);
                 snprintf(buf, sizeof(buf), "Default (%s)", TextureFormatName(fromDefault));
                 if (overrideCombo("##TargetFormat", valueW, TextureFormatName(stored), !formatOverridden, buf,
                                   formatLabel, s_FormatCount, 1, formatIndex))
                 {
                     m_TargetFormatIndex[platform->id] = formatIndex;
                     writeTarget(platform->id);
                 }
                 cardRow("Max Size", labelX, valueX);
                 snprintf(buf, sizeof(buf), "Default (%s)", MaxSizeLabel(m_MaxSizeIndex));
                 const std::string maxDefaultLabel = buf;
                 if (overrideCombo("##TargetMax", valueW, MaxSizeLabel(shownMax), !maxOverridden, maxDefaultLabel,
                                   maxLabel, kTextureMaxSizeCount, 0, maxIndex))
                 {
                     if (maxIndex < 0)
                         m_TargetMaxIndex.erase(platform->id);
                     else
                         m_TargetMaxIndex[platform->id] = maxIndex;
                     writeTarget(platform->id);
                 }
             });
    }

    // Overrides for targets this project no longer has, so they can be
    // removed.
    std::vector<std::string> gone;
    for (const auto& [id, index] : m_TargetFormatIndex)
        if (index > 0)
            gone.push_back(id);
    for (const auto& [id, index] : m_TargetMaxIndex)
        gone.push_back(id);
    std::sort(gone.begin(), gone.end());
    gone.erase(std::unique(gone.begin(), gone.end()), gone.end());
    for (const std::string& id : gone)
    {
        bool known = false;
        for (const PlatformConfig* platform : targets)
            known = known || platform->id == id;
        if (known)
            continue;
        const int fi = targetFormatIndex(id);
        const int mi = targetMaxIndex(id);
        std::string held;
        if (fi > 0)
            held += s_FormatNames[fi];
        if (mi >= 0)
            held += std::string(held.empty() ? "" : "  \xC2\xB7  ") + "Max Size " + MaxSizeLabel(mi);
        bool open = true;
        card(id.c_str(), true, id + " (removed)", true, nullptr, nullptr, "Not a target of this project any more",
             held, open, [&](float labelX, float, float) {
                 ImGui::SetCursorScreenPos(ImVec2(labelX, ImGui::GetCursorScreenPos().y));
                 ImGui::TextDisabled("%s", held.c_str());
                 ImGui::SetCursorScreenPos(ImVec2(labelX, ImGui::GetCursorScreenPos().y));
                 if (ImGui::SmallButton(ICON_TI_X " Remove override"))
                 {
                     m_TargetFormatIndex.erase(id);
                     m_TargetMaxIndex.erase(id);
                     write([&](nlohmann::json& tex) { WriteTextureTarget(tex, id, TextureTargetSettings{}); });
                 }
             });
    }

    DekiEditor::SchematicSectionEnd();
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
    m_StoredWidth = texData.header.width;
    m_StoredHeight = texData.header.height;
    int imageW = static_cast<int>(m_StoredWidth), imageH = static_cast<int>(m_StoredHeight);
    TextureImporter::ReadSourceSize(cachePath, imageW, imageH);
    m_PreviewWidth = static_cast<uint32_t>(imageW);
    m_PreviewHeight = static_cast<uint32_t>(imageH);
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
