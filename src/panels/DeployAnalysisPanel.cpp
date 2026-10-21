#include "DeployAnalysisPanel.h"
#include <deki/LogSystem.h>
#include <deki-editor/Paths.h>
#include <deki-editor/build/ProjectPaths.h>
#include "AssetBrowserPanel.h"
#include "EditorApp.h"
#include <deki-editor/EditorTheme.h>
#include <deki-editor/Motion.h>
#include <deki-editor/IconsTabler.h>
#include "core/Project.h"
#include "services/AssetExporter.h"
#include <deki-editor/AssetPipeline.h>
#include <deki-editor/AssetData.h>
#include <deki-editor/EditorUI.h>
#include <deki-editor/Localization.h>
#include "imgui.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace DekiEditor
{

DeployAnalysisPanel::DeployAnalysisPanel(EditorApp* app)
    : m_App(app)
{
}

DeployAnalysisPanel::~DeployAnalysisPanel() = default;

void DeployAnalysisPanel::Draw()
{
    if (!isOpen)
    {
        return;
    }

    auto& ui = EditorUI::Get();
    ui.SetNextWindowSize(550.0f, 400.0f, /*firstUseEver*/ true);

    if (!ui.Begin(TrLabel("DEPLOY ANALYSIS###Deploy Analysis"), &isOpen))
    {
        ui.End();
        return;
    }

    // A different project needs a new scan.
    void* currentProject = static_cast<void*>(m_App->GetProject());
    if (currentProject != m_LastProject)
    {
        m_LastProject = currentProject;
        m_HasScannedAssets = false;
        m_AssetEntries.clear();
        m_SelectedIndex = -1;
    }

    // Scan on first open and after a project change.
    bool hasProject = m_App->GetProject() != nullptr;
    if (!m_HasScannedAssets && hasProject)
    {
        ScanAssets();
    }

    // Toolbar strip styled like the Console's: a flat Rescan button between
    // 1px dividers, with a hairline below. Dimmed and inactive until a
    // project is open.
    {
        // A theme ImVec4 as the RGBA word EditorUI's Draw* calls take.
        auto pack = [](const ImVec4& c)
        {
            return EditorUI::Rgba((uint8_t)(c.x * 255.0f), (uint8_t)(c.y * 255.0f), (uint8_t)(c.z * 255.0f),
                                  (uint8_t)(c.w * 255.0f));
        };
        const float dpi = m_App->Dpi(1.0f);
        const float rowH = 34.0f * dpi;
        float rowMinX = 0.0f, rowMinY = 0.0f;
        ui.GetCursorScreenPos(&rowMinX, &rowMinY);
        float toolbarAvailW = 0.0f;
        ui.GetContentRegionAvail(&toolbarAvailW, nullptr);
        const float rowMaxX = rowMinX + toolbarAvailW;
        const float rowMaxY = rowMinY + rowH;

        const uint32_t colLine = pack(Palette::Line2);
        const uint32_t colDim = pack(Palette::Dim);
        const uint32_t colDim2 = pack(Palette::Dim2);
        const uint32_t colHoverBg = EditorUI::Rgba(255, 255, 255, 6);

        float cursorX = rowMinX;
        auto drawDivider = [&](float x) { ui.DrawLine(x, rowMinY, x, rowMaxY, colLine, 1.0f); };

        drawDivider(cursorX);

        // Rescan button.
        {
            const char* icon = ICON_TI_REFRESH;
            float iconW = 0.0f, iconH = 0.0f;
            ui.MeasureText(icon, &iconW, &iconH);
            const float w = iconW + 22.0f * dpi;
            ui.SetCursorScreenPos(cursorX, rowMinY);
            bool clicked = false;
            if (hasProject)
            {
                clicked = ui.InvisibleButton("##rescan", w, rowH);
                bool hovered = ui.IsItemHovered();
                if (hovered)
                {
                    ui.SetTooltip(Tr("Rescan scenes to determine which assets are used"));
                }
                if (const float hoverT = DekiEditor::Motion::ItemHover(hovered); hoverT > 0.0f)
                {
                    ui.DrawRectFilled(cursorX, rowMinY, cursorX + w, rowMaxY,
                                      DekiEditor::Motion::WithAlpha(colHoverBg, hoverT), 0.0f);
                }
                if (const float pressT = DekiEditor::Motion::ItemPress(); pressT > 0.0f)
                {
                    ui.DrawRectFilled(cursorX, rowMinY, cursorX + w, rowMaxY, DekiEditor::Motion::PressShade(pressT),
                                      0.0f);
                }
            }
            ui.DrawTextAt(0.0f, cursorX + (w - iconW) * 0.5f, rowMinY + (rowH - iconH) * 0.5f,
                          hasProject ? colDim : colDim2, icon);
            cursorX += w;
            drawDivider(cursorX);
            if (clicked)
            {
                ScanAssets();
            }
        }

        // Reserve the row in the layout and draw the hairline below it.
        ui.SetCursorScreenPos(rowMinX, rowMaxY);
        ui.DrawLine(rowMinX, rowMaxY, rowMaxX, rowMaxY, colLine, 1.0f);
    }

    ui.Spacing();

    // Asset table, drawn through EditorUI with the theme's colours.
    if (m_HasScannedAssets)
    {
        const float pad = m_App->Dpi(12.0f);  // content side gutter

        // Indent everything so nothing touches the edges.
        ui.Indent(pad);

        char header[64];
        std::snprintf(header, sizeof(header), Tr("Assets (%d total)"), (int)m_AssetEntries.size());
        ui.TextColored(Palette::Dim.x, Palette::Dim.y, Palette::Dim.z, Palette::Dim.w, header);
        ui.Spacing();

        // Leave room below for the summary, and the same gutter on the right.
        float availW = 0.0f, availH = 0.0f;
        ui.GetContentRegionAvail(&availW, &availH);
        const float reserveHeight = ui.GetFrameHeightWithSpacing() * 3.0f;
        float tableHeight = availH - reserveHeight;
        if (tableHeight < 100.0f)
        {
            tableHeight = 100.0f;
        }
        const float tableWidth = availW - pad;  // right gutter

        // Extra cell padding, for taller rows and more space between columns.
        ui.PushStyleVar(EditorUI::StyleVar::CellPadding, m_App->Dpi(8.0f), m_App->Dpi(5.0f));
        if (ui.BeginTable("AssetTable", 5,
                          EditorUI::TableResizable | EditorUI::TableBordersInnerV | EditorUI::TableRowBg |
                              EditorUI::TableScrollY,
                          tableWidth, tableHeight))
        {
            ui.TableSetupScrollFreeze(0, 1);
            ui.TableSetupColumn(TrLabel("Asset"), EditorUI::TableColumnWidthStretch);
            ui.TableSetupColumn(TrLabel("Size"), EditorUI::TableColumnWidthFixed, 70);
            ui.TableSetupColumn(TrLabel("Status"), EditorUI::TableColumnWidthFixed, 70);
            ui.TableSetupColumn(TrLabel("Pack"), EditorUI::TableColumnWidthFixed, 90);
            ui.TableHeadersRow();

            for (size_t i = 0; i < m_AssetEntries.size(); i++)
            {
                const auto& entry = m_AssetEntries[i];
                ui.TableNextRow();

                bool isSelected = (m_SelectedIndex == (int)i);

                // Asset path; the selectable spans all columns.
                ui.TableNextColumn();
                if (ui.Selectable(entry.displayPath.c_str(), isSelected, /*spanAllColumns*/ true))
                {
                    m_SelectedIndex = (int)i;

                    // Select it in the editor so the Inspector shows it.
                    auto* pipeline = m_App->GetAssetPipeline();
                    if (pipeline)
                    {
                        bool isBaked = entry.projectRelPath.empty();
                        std::string absPath = isBaked ? "" : pipeline->GetAbsolutePath(entry.projectRelPath);
                        std::string absCachePath = pipeline->GetAbsolutePath(entry.cachePath);
                        m_App->SetSelectedAsset(absPath, absCachePath, entry.hasCached, entry.guid);

                        // Show it in the Asset Browser. Baked entries exist only in
                        // the cache, so they have nothing to show.
                        if (!isBaked)
                        {
                            auto* browser = m_App->GetAssetBrowserPanel();
                            if (browser)
                            {
                                browser->RevealAsset(absPath);
                            }
                        }
                    }
                }

                // Size
                ui.TableNextColumn();
                ui.Text(FormatFileSize(entry.cacheSize).c_str());

                // Status
                ui.TableNextColumn();
                if (entry.isUsed)
                {
                    ui.TextColored(Palette::Green.x, Palette::Green.y, Palette::Green.z, Palette::Green.w, Tr("Used"));
                }
                else if (entry.forceIncluded)
                {
                    ui.TextColored(Palette::Amber.x, Palette::Amber.y, Palette::Amber.z, Palette::Amber.w,
                                   Tr("Forced"));
                }
                else
                {
                    ui.TextColored(Palette::Dim.x, Palette::Dim.y, Palette::Dim.z, Palette::Dim.w, Tr("Unused"));
                }

                // Pack
                ui.TableNextColumn();
                if (!entry.packName.empty())
                {
                    ui.TextColored(Palette::Accent.x, Palette::Accent.y, Palette::Accent.z, Palette::Accent.w,
                                   entry.packName.c_str());
                }
            }

            ui.EndTable();
        }
        ui.PopStyleVar();  // CellPadding

        // Size summary, set off from the table.
        ui.Spacing();
        ui.Separator();
        ui.Spacing();
        uintmax_t totalDeploySize = m_TotalUsedSize + m_TotalForceIncludedSize;

        // Packed assets that deploy.
        int packedCount = 0;
        for (const auto& entry : m_AssetEntries)
        {
            if (!entry.packName.empty() && (entry.isUsed || entry.forceIncluded))
            {
                packedCount++;
            }
        }

        char summary[256];
        std::snprintf(summary, sizeof(summary), Tr("Deploy: %d used + %d forced = %d assets (%s)"), m_UsedCount,
                      m_ForceIncludedCount, m_UsedCount + m_ForceIncludedCount,
                      FormatFileSize(totalDeploySize).c_str());
        ui.Text(summary);
        if (packedCount > 0)
        {
            char packLine[128];
            std::snprintf(packLine, sizeof(packLine), Tr("  %d assets in packs (fewer SD card reads)"), packedCount);
            ui.TextColored(Palette::Accent.x, Palette::Accent.y, Palette::Accent.z, Palette::Accent.w, packLine);
        }

        ui.Unindent(pad);
    }
    else
    {
        // Nothing scanned: a dim hint.
        const float pad = m_App->Dpi(12.0f);
        ui.Spacing();
        ui.Indent(pad);
        ui.TextColored(Palette::Dim.x, Palette::Dim.y, Palette::Dim.z, Palette::Dim.w,
                       hasProject ? Tr("Scanning assets...") : Tr("Open a project to analyze deploy assets."));
        ui.Unindent(pad);
    }

    ui.End();
}

void DeployAnalysisPanel::ScanAssets()
{
    namespace fs = std::filesystem;

    m_AssetEntries.clear();

    Project* project = m_App->GetProject();
    AssetPipeline* pipeline = m_App->GetAssetPipeline();
    if (!project || !pipeline)
    {
        return;
    }

    // GUIDs the scenes use.
    AssetExporter exporter(project, pipeline);
    AssetUsageCollection usage = exporter.CollectUsedAssets();

    // CollectUsedAssets adds a font's own GUID only. Add the GUIDs of its baked
    // variants and atlases too; they are derived from the font GUID and size.
    for (const auto& fu : usage.fontUsages)
    {
        usage.usedGuids.insert(fu.FontGuid());
        usage.usedGuids.insert(fu.AtlasGuid());
    }

    const auto& forceGuids = project->GetForceIncludeGuids();

    // GUID -> pack name, from the .dpack files.
    std::unordered_map<std::string, std::string> guidToPackName;
    {
        std::string assetsPath = project->GetAssetsPath();
        if (fs::exists(assetsPath))
        {
            for (const auto& dpackEntry : fs::recursive_directory_iterator(assetsPath))
            {
                if (!dpackEntry.is_regular_file() || dpackEntry.path().extension() != ".dpack")
                {
                    continue;
                }

                std::string packDisplayName = dpackEntry.path().stem().string();

                std::ifstream dpackFile(dpackEntry.path());
                if (!dpackFile.is_open())
                {
                    continue;
                }

                try
                {
                    nlohmann::json dpack = nlohmann::json::parse(dpackFile);
                    if (dpack.contains("assets") && dpack["assets"].is_array())
                    {
                        for (const auto& asset : dpack["assets"])
                        {
                            if (!asset.contains("guid"))
                            {
                                continue;
                            }
                            std::string guid = asset["guid"].get<std::string>();
                            guidToPackName[guid] = packDisplayName;

                            // A packed font packs its baked variants and atlases too.
                            std::string type = asset.value("type", "");
                            if (type == "BitmapFont" || type == "Font")
                            {
                                for (const auto& fu : usage.fontUsages)
                                {
                                    if (fu.sourceGuid != guid)
                                    {
                                        continue;
                                    }
                                    guidToPackName[fu.FontGuid()] = packDisplayName;
                                    guidToPackName[fu.AtlasGuid()] = packDisplayName;
                                }
                            }
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    DEKI_LOG_WARNING(
                        "DeployAnalysisPanel: atlas metadata unreadable (%s); pack names will show as unknown",
                        e.what());
                }
            }
        }
    }

    const auto& allAssets = pipeline->GetAllAssets();

    for (const auto& [path, info] : allAssets)
    {
        if (info.guid.empty())
        {
            continue;
        }
        if (!info.hasCachedVersion || info.cachePath.empty())
        {
            continue;
        }

        AssetEntry entry;
        entry.guid = info.guid;
        entry.projectRelPath = info.path;
        entry.cachePath = info.cachePath;
        entry.hasCached = info.hasCachedVersion;

        // Drop the assets folder prefix for display. Cut by the prefix's real
        // length, never a fixed count, or the first letters of names go missing.
        entry.displayPath = info.path;
        const std::string assetsPrefix = GetAssetsDirectoryRelative() + "/";
        if (entry.displayPath.rfind(assetsPrefix, 0) == 0)
        {
            entry.displayPath = entry.displayPath.substr(assetsPrefix.size());
        }

        std::string absoluteCachePath = pipeline->GetAbsolutePath(info.cachePath);
        try
        {
            entry.cacheSize = fs::exists(absoluteCachePath) ? fs::file_size(absoluteCachePath) : 0;
        }
        catch (...)
        {
            entry.cacheSize = 0;
        }

        entry.isUsed = usage.usedGuids.count(info.guid) > 0;
        entry.forceIncluded = forceGuids.count(info.guid) > 0;

        auto packIt = guidToPackName.find(info.guid);
        if (packIt != guidToPackName.end())
        {
            entry.packName = packIt->second;
        }

        m_AssetEntries.push_back(entry);
    }

    // Baked font variants and atlases exist only in the cache, not in the
    // pipeline, so add them here.
    {
        std::unordered_set<std::string> existingGuids;
        for (const auto& e : m_AssetEntries)
        {
            existingGuids.insert(e.guid);
        }

        std::string projectPath = project->GetPath();
        std::string cachePath = ProjectPaths::Cache(projectPath).string();

        for (const auto& fu : usage.fontUsages)
        {
            std::string variantGuid = fu.FontGuid();
            std::string atlasGuid = fu.AtlasGuid();

            auto addBakedEntry = [&](const std::string& guid, const std::string& label)
            {
                if (existingGuids.count(guid))
                {
                    return;
                }
                existingGuids.insert(guid);

                std::string cPath = cachePath + "/" + guid;
                AssetEntry entry;
                entry.guid = guid;
                entry.displayPath = label;
                entry.projectRelPath = "";
                entry.cachePath = GetCacheDirectoryRelative() + "/" + guid;
                entry.hasCached = fs::exists(cPath);
                entry.cacheSize = entry.hasCached ? fs::file_size(cPath) : 0;
                entry.isUsed = usage.usedGuids.count(guid) > 0;
                entry.forceIncluded = false;

                auto packIt = guidToPackName.find(guid);
                if (packIt != guidToPackName.end())
                {
                    entry.packName = packIt->second;
                }

                m_AssetEntries.push_back(entry);
            };

            std::string fontName = fu.sourceGuid;
            const AssetInfo* fontAsset = pipeline->GetAssetInfoByGuid(fu.sourceGuid);
            if (fontAsset)
            {
                fontName = fs::path(fontAsset->path).stem().string();
            }

            // Shown in the table; made at scan time, in the language of the moment.
            char baked[256];
            std::snprintf(baked, sizeof(baked), Tr("(baked) %s @ %dpx"), fontName.c_str(), (int)fu.fontSize);
            addBakedEntry(variantGuid, baked);
            std::snprintf(baked, sizeof(baked), Tr("(baked) %s atlas @ %dpx"), fontName.c_str(), (int)fu.fontSize);
            addBakedEntry(atlasGuid, baked);
        }
    }

    // Used first, then by name.
    std::sort(m_AssetEntries.begin(), m_AssetEntries.end(),
              [](const AssetEntry& a, const AssetEntry& b)
              {
                  if (a.isUsed != b.isUsed)
                  {
                      return a.isUsed > b.isUsed;
                  }
                  return a.displayPath < b.displayPath;
              });

    m_HasScannedAssets = true;
    RecalculateTotals();
}

void DeployAnalysisPanel::RecalculateTotals()
{
    m_TotalUsedSize = 0;
    m_TotalForceIncludedSize = 0;
    m_UsedCount = 0;
    m_ForceIncludedCount = 0;

    for (const auto& entry : m_AssetEntries)
    {
        if (entry.isUsed)
        {
            m_TotalUsedSize += entry.cacheSize;
            m_UsedCount++;
        }
        else if (entry.forceIncluded)
        {
            m_TotalForceIncludedSize += entry.cacheSize;
            m_ForceIncludedCount++;
        }
    }
}

std::string DeployAnalysisPanel::FormatFileSize(uintmax_t bytes)
{
    if (bytes < 1024)
    {
        return std::to_string(bytes) + " B";
    }
    if (bytes < 1024 * 1024)
    {
        return std::to_string(bytes / 1024) + " KB";
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    return buf;
}

}  // namespace DekiEditor
