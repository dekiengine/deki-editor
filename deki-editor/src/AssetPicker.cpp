#include <deki-editor/AssetPicker.h>
#include <deki-editor/EditorApplication.h>
#include <deki-editor/EditorTheme.h>
#include <deki-editor/IconsTabler.h>
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetTypeRegistry.h>
#include <deki-editor/EditorAssets.h>
#include "imgui.h"
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

// Force full definition visibility (MSVC workaround)
static_assert(sizeof(DekiEditor::AssetPickerEntry) > 0, "AssetPickerEntry must be complete");

namespace DekiEditor
{

namespace
{
// EditorApp::Dpi(x) for code that cannot see EditorApp: the same scale.
float Dpi(float v)
{
    return v * ImGui::GetStyle().FontScaleDpi;
}
}  // namespace

namespace
{
enum class IconKind
{
    Texture,
    Procedural,
    Audio,
    Script,
    Font,
    Scene,
    Animation,
    Unknown
};

// Classification is driven by AssetTypeRegistry::GetCategoryForExtension so
// the package that owns the file extension is the single source of truth. The
// editor does not carry per-extension lists. See CLAUDE.md (project).
IconKind ClassifyByExtension(const std::string& path)
{
    std::string ext;
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos)
        ext = path.substr(dot);

    switch (AssetTypeRegistry::Instance().GetCategoryForExtension(ext))
    {
        case AssetCategory::Texture: return IconKind::Texture;
        case AssetCategory::Procedural: return IconKind::Procedural;
        case AssetCategory::Audio: return IconKind::Audio;
        case AssetCategory::Font: return IconKind::Font;
        case AssetCategory::Scene: return IconKind::Scene;
        case AssetCategory::Animation: return IconKind::Animation;
        case AssetCategory::Script: return IconKind::Script;
        case AssetCategory::Pipeline:
        case AssetCategory::Data:
        case AssetCategory::Unknown:
        default:
            return IconKind::Unknown;
    }
}

// Icon painters draw at an explicit position on the given draw list and
// advance no cursor - rows place them inside their own (clipped) rect.

void DrawPlaceholderIconAt(ImDrawList* dl, ImVec2 p, float size, IconKind kind)
{
    ImU32 color;
    const char* letter;
    switch (kind)
    {
        case IconKind::Script:
            color = IM_COL32(70, 130, 200, 255);
            letter = "S";
            break;
        case IconKind::Font:
            color = IM_COL32(220, 140, 60, 255);
            letter = "F";
            break;
        case IconKind::Scene:
            color = IM_COL32(80, 180, 100, 255);
            letter = "P";
            break;
        case IconKind::Animation:
            color = IM_COL32(160, 100, 200, 255);
            letter = "A";
            break;
        case IconKind::Audio:
            color = IM_COL32(70, 180, 180, 255);
            letter = "M";
            break;
        case IconKind::Procedural:
            color = IM_COL32(210, 90, 150, 255);
            letter = "A";
            break;
        default:
            color = IM_COL32(100, 100, 100, 255);
            letter = "?";
            break;
    }
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), color);
    ImFont* font = ImGui::GetFont();
    float fontSize = size * 0.55f;
    ImVec2 ts = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, letter);
    dl->AddText(font, fontSize,
                ImVec2(p.x + (size - ts.x) * 0.5f, p.y + (size - ts.y) * 0.5f),
                IM_COL32(255, 255, 255, 255), letter);
}

void DrawFittedTextureAt(ImDrawList* dl, ImVec2 p, float boxSize,
                         uint32_t texId, uint32_t texW, uint32_t texH,
                         float u0, float v0, float u1, float v1)
{
    float contentW = (u1 - u0) * static_cast<float>(texW);
    float contentH = (v1 - v0) * static_cast<float>(texH);
    if (contentW <= 0.0f) contentW = 1.0f;
    if (contentH <= 0.0f) contentH = 1.0f;

    float scale = std::min(boxSize / contentW, boxSize / contentH);
    float drawW = contentW * scale;
    float drawH = contentH * scale;

    float offX = (boxSize - drawW) * 0.5f;
    float offY = (boxSize - drawH) * 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + boxSize, p.y + boxSize),
                      ImGui::ColorConvertFloat4ToU32(Palette::Bg3));
    dl->AddImage(static_cast<ImTextureID>(static_cast<intptr_t>(texId)),
                 ImVec2(p.x + offX, p.y + offY),
                 ImVec2(p.x + offX + drawW, p.y + offY + drawH),
                 ImVec2(u0, v0), ImVec2(u1, v1));
}

void DrawAssetRowIconAt(ImDrawList* dl, ImVec2 p, float size, const AssetPickerEntry& asset)
{
    IconKind kind = ClassifyByExtension(asset.relativePath);
    // Procedural .asset entries that target Texture compile to a cached DTEX
    // stored under the asset's GUID, same as image textures.
    bool canHaveTexture = (kind == IconKind::Texture || kind == IconKind::Procedural);
    if (canHaveTexture && !asset.guid.empty())
    {
        uint32_t w = 0, h = 0;
        uint32_t texId = EditorAssets::Get()->LoadTexture(asset.guid, &w, &h);
        if (texId != 0 && w > 0 && h > 0)
        {
            DrawFittedTextureAt(dl, p, size, texId, w, h, 0.0f, 0.0f, 1.0f, 1.0f);
            return;
        }
    }
    DrawPlaceholderIconAt(dl, p, size, kind);
}

void DrawFrameRowIconAt(ImDrawList* dl, ImVec2 p, float size, const std::string& frameGuid)
{
    uint32_t w = 0, h = 0;
    uint32_t texId = EditorAssets::Get()->LoadFrameTexture(frameGuid, &w, &h);
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    if (texId != 0 && w > 0 && h > 0 &&
        EditorAssets::Get()->GetFrameUVs(frameGuid, &u0, &v0, &u1, &v1))
    {
        DrawFittedTextureAt(dl, p, size, texId, w, h, u0, v0, u1, v1);
        return;
    }
    DrawPlaceholderIconAt(dl, p, size, IconKind::Texture);
}
}  // namespace

AssetPicker::AssetPicker()
{
}

bool AssetPicker::Open(const std::string& popupId, const std::string& currentValue,
                       AssetFilterType filter, std::string& outSelectedPath)
{
    // Refresh if filter changed or first time
    if (filter != m_CurrentFilter || m_NeedsRefresh)
    {
        m_CurrentFilter = filter;
        RefreshAssets(filter);
        m_NeedsRefresh = false;
    }

    // Find current selection (match by GUID or relativePath for backwards compat)
    m_SelectedIndex = -1;
    for (size_t i = 0; i < m_Assets.size(); ++i)
    {
        if (m_Assets[i].guid == currentValue || m_Assets[i].relativePath == currentValue)
        {
            m_SelectedIndex = static_cast<int>(i);
            break;
        }
    }

    ImGui::OpenPopup(popupId.c_str());
    return Draw(popupId, outSelectedPath);
}

bool AssetPicker::Draw(const std::string& popupId, std::string& outSelectedPath)
{
    bool selected = false;

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Dpi(620.0f), Dpi(580.0f)), ImGuiCond_FirstUseEver);

    // The global theme zeroes WindowPadding (panels manage their own inset);
    // give the dialog a real one. It's baked at Begin, so pop right after.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(Dpi(12.0f), Dpi(10.0f)));
    const bool open = ImGui::BeginPopupModal(popupId.c_str(), nullptr, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();  // WindowPadding

    if (open)
    {
        // Schematic search band (dim magnifier + borderless input) with a
        // full-width hairline beneath it, fresh + focused each time it opens.
        if (ImGui::IsWindowAppearing())
        {
            m_SearchBuffer[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        SearchField("##assetpicker_search", m_SearchBuffer, sizeof(m_SearchBuffer),
                    0.0f, /*bottomBorder*/ true);

        // Meta row: dim uppercase filter/count summary + right-aligned REFRESH link
        const char* filterName = "ALL";
        switch (m_CurrentFilter)
        {
            case AssetFilterType::Textures: filterName = "TEXTURES"; break;
            case AssetFilterType::Audio: filterName = "AUDIO"; break;
            case AssetFilterType::Scripts: filterName = "SCRIPTS"; break;
            case AssetFilterType::Scenes: filterName = "SCENES"; break;
            case AssetFilterType::Fonts: filterName = "FONTS"; break;
            case AssetFilterType::Animations: filterName = "ANIMATIONS"; break;
            default: break;
        }
        {
            ImFont* metaFont = g_FontUIBold ? g_FontUIBold : ImGui::GetFont();
            ImGui::PushFont(metaFont, Fonts::CssPx(metaFont, 10.5f));
            ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
            ImGui::Text("FILTER: %s   %zu ASSETS", filterName, m_Assets.size());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        if (SchematicActionLink("REFRESH##assetpicker_refresh"))
            RefreshAssets(m_CurrentFilter);
        ImGui::Spacing();

        // Asset list - fills the space above the footer (hairline + buttons)
        const float footerH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2.0f + Dpi(8.0f);
        ImGui::BeginChild("AssetList", ImVec2(0, -footerH), ImGuiChildFlags_None);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, Dpi(2.0f)));

        // Row metrics - compact schematic rows; every icon/text draw is
        // clipped to the row rect so nothing bleeds outside the list.
        const float rowH = Dpi(48.0f);
        const float rowPadX = Dpi(6.0f);
        const float iconSize = Dpi(38.0f);
        const float chevronW = Dpi(20.0f);
        const float textGap = Dpi(10.0f);
        const float indentW = Dpi(26.0f);
        const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
        const ImU32 dimCol = ImGui::GetColorU32(Palette::Dim);

        enum class Chevron
        {
            None,
            Closed,
            Open
        };

        // One list row: leading chevron slot (open/close for spritesheets),
        // icon box, vertically centered name. Returns true when clicked;
        // *outChevronClicked reports whether the click landed on the chevron.
        auto drawAssetRow = [&](const char* label, bool isSelected, float indent, Chevron chevron,
                                const std::function<void(ImDrawList*, ImVec2, float)>& drawIcon,
                                bool* outChevronClicked) -> bool
        {
            // The theme's Header color is transparent (hover-only rows); give
            // the persistent selection the accent-subtle fill instead.
            if (isSelected)
                ImGui::PushStyleColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            // Size.x = 0 spans the remaining row width
            const bool clicked = ImGui::Selectable("##row", isSelected,
                                                   ImGuiSelectableFlags_None,
                                                   ImVec2(0, rowH));
            if (isSelected)
                ImGui::PopStyleColor();
            const ImVec2 itemMin = ImGui::GetItemRectMin();
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(itemMin, itemMax, true);

            float x = itemMin.x + rowPadX + indent;

            if (chevron != Chevron::None)
            {
                const char* icon = (chevron == Chevron::Open) ? ICON_TI_CHEVRON_DOWN
                                                              : ICON_TI_CHEVRON_RIGHT;
                ImFont* f = ImGui::GetFont();
                const float isz = ImGui::GetFontSize();
                const ImVec2 cs = f->CalcTextSizeA(isz, FLT_MAX, 0.0f, icon);
                dl->AddText(f, isz, ImVec2(x, itemMin.y + (rowH - cs.y) * 0.5f), dimCol, icon);
            }
            x += chevronW;  // slot always reserved so icons align across rows

            if (drawIcon)
            {
                drawIcon(dl, ImVec2(x, itemMin.y + (rowH - iconSize) * 0.5f), iconSize);
                x += iconSize + textGap;
            }

            dl->AddText(ImVec2(x, itemMin.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f),
                        textCol, label);
            dl->PopClipRect();

            if (outChevronClicked)
                *outChevronClicked = clicked &&
                                     ImGui::GetMousePos().x < itemMin.x + rowPadX + indent + chevronW;
            return clicked;
        };

        // "(None)" option
        {
            bool isNoneSelected = m_SelectedIndex == -1;
            ImGui::PushID("none");
            if (drawAssetRow("(None)", isNoneSelected, 0.0f, Chevron::None, nullptr, nullptr))
            {
                m_SelectedIndex = -1;
                m_SelectedSubAsset.clear();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
            {
                outSelectedPath = "";
                selected = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }

        // Search filter
        std::string searchLower = m_SearchBuffer;
        for (char& c : searchLower)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        // Asset entries
        auto* pipeline = AssetPipeline::Instance();
        for (size_t i = 0; i < m_Assets.size(); ++i)
        {
            const auto& asset = m_Assets[i];

            // Skip SubAssets in main list (they appear under parent)
            if (asset.isSubAsset)
                continue;

            // Apply search filter (names were lower-cased when the list was built)
            if (!searchLower.empty() && asset.displayNameLower.find(searchLower) == std::string::npos)
                continue;

            ImGui::PushID(static_cast<int>(i));

            if (asset.isExpandable)
            {
                // Expandable entry (spritesheet with frames)
                bool isExpanded = m_ExpandedAssets.count(asset.guid) > 0;
                bool isSelected = (static_cast<int>(i) == m_SelectedIndex && m_SelectedSubAsset.empty());

                bool chevronClicked = false;
                bool clicked = drawAssetRow(asset.displayName.c_str(), isSelected, 0.0f, isExpanded ? Chevron::Open : Chevron::Closed, [&asset](ImDrawList* dl, ImVec2 p, float sz)
                                            { DrawAssetRowIconAt(dl, p, sz, asset); }, &chevronClicked);
                if (clicked)
                {
                    if (chevronClicked)
                    {
                        if (isExpanded)
                            m_ExpandedAssets.erase(asset.guid);
                        else
                            m_ExpandedAssets.insert(asset.guid);
                    }
                    else
                    {
                        m_SelectedIndex = static_cast<int>(i);
                        m_SelectedSubAsset.clear();
                    }
                }

                // Double-click to select parent
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                {
                    outSelectedPath = asset.guid.empty() ? asset.relativePath : asset.guid;
                    selected = true;
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered())
                {
                    DekiEditor::Tooltip("%s", asset.fullPath.c_str());
                }

                if (isExpanded && pipeline)
                {
                    // SubAssets (frames), indented under their spritesheet
                    const auto* subAssets = pipeline->GetSubAssets(asset.guid);
                    if (subAssets)
                    {
                        int subIdx = 0;
                        for (const auto& subAsset : *subAssets)
                        {
                            bool subSelected = (m_SelectedSubAsset == subAsset.guid);
                            std::string subLabel = subAsset.name.empty()
                                                       ? "Frame " + std::to_string(subAsset.subAssetIndex)
                                                       : subAsset.name;

                            ImGui::PushID(subIdx++);
                            if (drawAssetRow(subLabel.c_str(), subSelected, indentW, Chevron::None, [&subAsset](ImDrawList* dl, ImVec2 p, float sz)
                                             { DrawFrameRowIconAt(dl, p, sz, subAsset.guid); }, nullptr))
                            {
                                m_SelectedIndex = static_cast<int>(i);
                                m_SelectedSubAsset = subAsset.guid;
                            }

                            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                            {
                                outSelectedPath = subAsset.guid;
                                selected = true;
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::PopID();
                        }
                    }
                }
            }
            else
            {
                // Regular entry
                bool isSelected = (static_cast<int>(i) == m_SelectedIndex && m_SelectedSubAsset.empty());
                if (drawAssetRow(asset.displayName.c_str(), isSelected, 0.0f, Chevron::None, [&asset](ImDrawList* dl, ImVec2 p, float sz)
                                 { DrawAssetRowIconAt(dl, p, sz, asset); }, nullptr))
                {
                    m_SelectedIndex = static_cast<int>(i);
                    m_SelectedSubAsset.clear();
                }

                // Double-click to select and close
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                {
                    // Return GUID for asset references (like Unity)
                    outSelectedPath = asset.guid.empty() ? asset.relativePath : asset.guid;
                    selected = true;
                    ImGui::CloseCurrentPopup();
                }

                // Tooltip with full path
                if (ImGui::IsItemHovered())
                {
                    DekiEditor::Tooltip("%s", asset.fullPath.c_str());
                }
            }

            ImGui::PopID();
        }

        ImGui::PopStyleVar();  // list ItemSpacing
        ImGui::EndChild();

        FullBleedSeparator();
        // The separator itself charges no space (it is a boundary line), so the
        // footer states its own breathing room instead of inheriting it.
        ImGui::Dummy(ImVec2(0.0f, Dpi(8.0f)));

        // Footer: right-aligned Select / Cancel
        const float btnW = Dpi(110.0f);
        const float freeW = ImGui::GetContentRegionAvail().x - (btnW * 2.0f + ImGui::GetStyle().ItemSpacing.x);
        if (freeW > 0.0f)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + freeW);

        if (ImGui::Button("Select", ImVec2(btnW, 0)))
        {
            if (!m_SelectedSubAsset.empty())
            {
                // Return SubAsset GUID
                outSelectedPath = m_SelectedSubAsset;
            }
            else if (m_SelectedIndex >= 0 && m_SelectedIndex < static_cast<int>(m_Assets.size()))
            {
                // Return GUID for asset references (like Unity)
                const auto& asset = m_Assets[m_SelectedIndex];
                outSelectedPath = asset.guid.empty() ? asset.relativePath : asset.guid;
            }
            else
            {
                outSelectedPath = "";
            }
            selected = true;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(btnW, 0)))
        {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    return selected;
}

void AssetPicker::RefreshAssets(AssetFilterType filter)
{
    m_Assets.clear();
    m_SelectedIndex = -1;

    auto* pipeline = AssetPipeline::Instance();
    if (!pipeline)
        return;

    const std::string& assetsPath = pipeline->GetAssetsPath();
    if (assetsPath.empty() || !fs::exists(assetsPath))
        return;

    auto extensions = GetExtensionsForFilter(filter);
    ScanDirectory(assetsPath, assetsPath, extensions, pipeline);

    // Sort by name
    auto comparator = [](const AssetPickerEntry& a, const AssetPickerEntry& b)
    {
        return a.displayName < b.displayName;
    };
    std::sort(m_Assets.begin(), m_Assets.end(), comparator);
}

std::vector<std::string> AssetPicker::GetExtensionsForFilter(AssetFilterType filter)
{
    // The packages own the extension lists (AssetTypeRegistry, filled in when
    // they load). The literal lists are only the fallback for a category no
    // loaded package has registered, so the picker still works in that case.
    auto categoryOf = [](AssetFilterType f) -> AssetCategory
    {
        switch (f)
        {
        case AssetFilterType::Textures: return AssetCategory::Texture;
        case AssetFilterType::Audio: return AssetCategory::Audio;
        case AssetFilterType::Scripts: return AssetCategory::Script;
        case AssetFilterType::Scenes: return AssetCategory::Scene;
        case AssetFilterType::Fonts: return AssetCategory::Font;
        case AssetFilterType::Animations: return AssetCategory::Animation;
        default: return AssetCategory::Unknown;
        }
    };
    auto fallback = [](AssetFilterType f) -> std::vector<std::string>
    {
        switch (f)
        {
        case AssetFilterType::Textures: return { ".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif" };
        case AssetFilterType::Audio: return { ".wav", ".mp3", ".ogg", ".flac" };
        case AssetFilterType::Scripts: return { ".cpp", ".h", ".hpp", ".c" };
        case AssetFilterType::Scenes: return { ".scene" };
        case AssetFilterType::Fonts: return { ".ttf", ".otf", ".fnt", ".bdf" };
        case AssetFilterType::Animations: return { ".anim", ".spritedef" };
        default: return {};
        }
    };
    auto forFilter = [&](AssetFilterType f)
    {
        std::vector<std::string> exts = AssetTypeRegistry::Instance().GetExtensionsForCategory(categoryOf(f));
        return exts.empty() ? fallback(f) : exts;
    };

    if (filter != AssetFilterType::All)
        return forFilter(filter);

    // "All" = every asset family except scripts, as before.
    std::vector<std::string> all;
    for (AssetFilterType f : { AssetFilterType::Textures, AssetFilterType::Audio, AssetFilterType::Scenes,
                               AssetFilterType::Fonts, AssetFilterType::Animations })
    {
        std::vector<std::string> exts = forFilter(f);
        all.insert(all.end(), exts.begin(), exts.end());
    }
    return all;
}

void AssetPicker::ScanDirectory(const std::string& dir, const std::string& baseDir,
                                const std::vector<std::string>& extensions,
                                AssetPipeline* pipeline)
{
    try
    {
        for (const auto& entry : fs::directory_iterator(dir))
        {
            if (entry.is_directory())
            {
                ScanDirectory(entry.path().string(), baseDir, extensions, pipeline);
            }
            else if (entry.is_regular_file())
            {
                std::string ext = entry.path().extension().string();
                for (char& c : ext)
                {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }

                bool matches = false;
                for (const auto& filterExt : extensions)
                {
                    if (ext == filterExt)
                    {
                        matches = true;
                        break;
                    }
                }

                // Special handling for .asset files - check if they compile to the right type
                if (!matches && ext == ".asset" && m_CurrentFilter == AssetFilterType::Textures)
                {
                    // Read the JSON to check the type
                    std::ifstream file(entry.path().string());
                    if (file.is_open())
                    {
                        try
                        {
                            nlohmann::json data = nlohmann::json::parse(file);
                            std::string assetType = data.value("type", "");
                            const AssetTypeHandler* handler = AssetTypeRegistry::Instance().GetHandler(assetType);
                            if (handler && handler->compilesTo == AssetCompileTarget::Texture)
                            {
                                matches = true;
                            }
                        }
                        catch (const nlohmann::json::exception&)
                        {
                            // Invalid JSON, skip
                        }
                    }
                }

                if (matches)
                {
                    AssetPickerEntry asset;
                    asset.fullPath = entry.path().string();
                    asset.relativePath = fs::relative(entry.path(), baseDir).string();
                    asset.displayName = asset.relativePath;

                    // Convert backslashes to forward slashes for consistency
                    for (char& c : asset.relativePath)
                    {
                        if (c == '\\') c = '/';
                    }
                    for (char& c : asset.displayName)
                    {
                        if (c == '\\') c = '/';
                    }

                    // Get GUID from AssetPipeline using project-relative path
                    if (pipeline)
                    {
                        const std::string projectPath = pipeline->GetProjectPath();
                        if (!projectPath.empty())
                        {
                            std::string projectRelativePath = fs::relative(entry.path(), projectPath).string();
                            for (char& c : projectRelativePath)
                            {
                                if (c == '\\') c = '/';
                            }
                            const AssetInfo* info = pipeline->GetAssetInfo(projectRelativePath);
                            if (info)
                            {
                                asset.guid = info->guid;

                                // Check if this asset has SubAssets (spritesheets with frames)
                                const auto* subAssets = pipeline->GetSubAssets(info->guid);
                                if (subAssets && !subAssets->empty())
                                {
                                    asset.isExpandable = true;
                                }
                            }
                        }
                    }

                    asset.displayNameLower = asset.displayName;

                    for (char& c : asset.displayNameLower)

                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                    m_Assets.push_back(asset);
                }
            }
        }
    }
    catch (const std::exception&)
    {
        // Ignore directory access errors
    }
}

}  // namespace DekiEditor
