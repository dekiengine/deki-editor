#include "MemoryPanel.h"

#include "EditorApp.h"
#include "../PanelMaximize.h"
#include "core/Project.h"
#include "inspector/InspectorUtils.h"
#include "services/DeviceScreenshot.h"

#include <deki-editor/AssetPipeline.h>
#include <deki-editor/EditorTheme.h>
#include <deki-editor/EditorUI.h>
#include <deki-editor/IconsTabler.h>
#include <deki-editor/Localization.h>
#include <deki-editor/SerialPorts.h>
#include <deki-editor/build/PlatformConfig.h>
#include <deki-editor/Paths.h>

#include <deki/Scene.h>

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace DekiEditor
{

struct MemoryPanel::DeviceJob
{
    std::mutex mutex;
    bool done = false;
    bool ok = false;
    std::string port;
    std::string error;
    MemoryReport report;
};

namespace
{

uint32_t Pack(const ImVec4& c)
{
    return EditorUI::Rgba(static_cast<uint8_t>(c.x * 255.0f), static_cast<uint8_t>(c.y * 255.0f),
                          static_cast<uint8_t>(c.z * 255.0f), static_cast<uint8_t>(c.w * 255.0f));
}

void TextIn(const ImVec4& c, const char* text)
{
    EditorUI::Get().TextColored(c.x, c.y, c.z, c.w, text);
}

// "void Deki::Sprite::Load(const char*)" -> "Deki::Sprite::Load": the
// signature is in the tooltip; the row needs the name.
std::string ShortFunction(const std::string& f)
{
    std::string s = f;
    const size_t paren = s.find('(');
    if (paren != std::string::npos)
    {
        s.resize(paren);
    }
    // Template arguments can hold spaces; the name is after the last space
    // outside them.
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '<')
        {
            ++depth;
        }
        else if (s[i] == '>')
        {
            --depth;
        }
        else if (s[i] == ' ' && depth == 0)
        {
            start = i + 1;
        }
    }
    return s.substr(start);
}

// The targets the project has its own copy of (platforms/<id>/), as the
// texture inspector lists them.
std::vector<const PlatformConfig*> ProjectTargets(const std::string& projectPath)
{
    std::vector<const PlatformConfig*> out;
    const std::string dir = GetPlatformsDirectory(projectPath);
    for (const auto& [id, name] : PlatformRegistry::Instance().GetPlatformDisplayList())
    {
        std::error_code ec;
        if (!fs::is_directory(fs::path(dir) / id, ec))
        {
            continue;
        }
        if (const PlatformConfig* p = PlatformRegistry::Instance().GetPlatform(id))
        {
            out.push_back(p);
        }
    }
    return out;
}

std::string TargetLabel(const PlatformConfig* p)
{
    if (!p)
    {
        return Tr("Editor (desktop)");
    }
    return p->displayName.empty() ? p->id : p->displayName;
}

// The share of `budget` that `bytes` is, as " (12%)", or "" with no budget.
std::string Share(uint64_t bytes, uint64_t budget)
{
    if (budget == 0)
    {
        return "";
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), " (%.0f%%)", 100.0 * static_cast<double>(bytes) / static_cast<double>(budget));
    return buf;
}

// A table in the panel's style: row backgrounds and inner borders, no scroll
// of its own (the panel scrolls), the first column stretching.
// It starts where the rows' labels do and keeps the same gutter on the right.
bool BeginSizeTable(const char* id, int columns)
{
    auto& ui = EditorUI::Get();
    const float startX = ImGui::GetCursorPosX();
    const float x = InspectorUtils::SectionAlignedCursorX();
    ImGui::SetCursorPosX(x);
    float w = 0.0f;
    ui.GetContentRegionAvail(&w, nullptr);
    return ui.BeginTable(id, columns, EditorUI::TableRowBg | EditorUI::TableBordersInnerV | EditorUI::TableResizable,
                         w - (x - startX), 0.0f);
}

}  // namespace

MemoryPanel::MemoryPanel(EditorApp* app)
    : m_App(app)
{
}

MemoryPanel::~MemoryPanel() = default;

void MemoryPanel::Draw()
{
    if (!isOpen)
    {
        return;
    }

    DekiEditor::FirstUseWindowPlacement(560, 640);
    if (ImGui::Begin(TrLabel("MEMORY###Memory"), &isOpen, DekiEditor::MaximizedWindowFlags("MEMORY###Memory")))
    {
        auto& ui = EditorUI::Get();
        ui.BeginToolbar();
        if (ui.ToolbarButton(Tr("Scene"), true, m_View == View::Scene))
        {
            m_View = View::Scene;
        }
        if (ui.ToolbarButton(Tr("Live"), true, m_View == View::Live))
        {
            m_View = View::Live;
        }
        if (ui.ToolbarIconButton("##refresh", ICON_TI_REFRESH,
                                 m_View == View::Scene ? Tr("Work the estimate out again")
                                                       : Tr("Read the memory again")))
        {
            if (m_View == View::Scene)
            {
                m_EstimateValid = false;
            }
            else
            {
                m_Ports = EnumerateSerialPorts();  // a board plugged in since
                RefreshLive(true);
            }
        }
        if (m_View == View::Live && ui.ToolbarButton(Tr("Auto"), true, m_Auto))
        {
            m_Auto = !m_Auto;
        }
        ui.EndToolbar();

        if (m_View == View::Scene)
        {
            DrawSceneView();
        }
        else
        {
            DrawLiveView();
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// Scene: the estimate
// ---------------------------------------------------------------------------

void MemoryPanel::RefreshEstimate()
{
    Project* project = m_App->GetProject();
    Deki::Scene* scene = m_App->GetCurrentScene();
    const PlatformConfig* platform =
        m_TargetId.empty() ? nullptr : PlatformRegistry::Instance().GetPlatform(m_TargetId);
    int64_t total = -1, free = -1;
    // A board's figures are a board's: the editor's desktop has no budget.
    if (m_HasDeviceReport && platform && platform->family != "native")
    {
        total = m_DeviceReport.internalTotal;
        // What was free before the engine placed anything, about what the
        // render system saw when it chose whole frames or strips at startup.
        if (m_DeviceReport.internalFree >= 0)
        {
            free = m_DeviceReport.internalFree + static_cast<int64_t>(m_DeviceReport.internalUsed);
        }
    }
    m_Estimate =
        EstimateSceneMemory(project ? project->GetPath() : "", m_App->GetAssetPipeline(), scene, platform, total, free);
    m_EstimateScene = scene;
    m_EstimateValid = true;
    m_EstimateTime = std::chrono::steady_clock::now();
}

void MemoryPanel::DrawRegionBar(const char* label, uint64_t used, uint64_t budget, bool measured)
{
    auto& ui = EditorUI::Get();
    InspectorUtils::BeginPropertyRow(label);

    // A meter along the bottom of the value column, under the figures.
    float x = 0.0f, y = 0.0f, w = 0.0f;
    ui.GetCursorScreenPos(&x, &y);
    ui.GetContentRegionAvail(&w, nullptr);
    const float h = ui.GetFrameHeight();
    const float barH = m_App->Dpi(3.0f);
    if (budget > 0)
    {
        const double share = static_cast<double>(used) / static_cast<double>(budget);
        const ImVec4& fill = share > 1.0 ? Palette::Red : share > 0.8 ? Palette::Amber : Palette::Accent;
        ui.DrawRectFilled(x, y + h - barH, x + w, y + h, Pack(Palette::Line2));
        ui.DrawRectFilled(x, y + h - barH, x + w * static_cast<float>(std::min(share, 1.0)), y + h, Pack(fill));
    }

    char text[160];
    if (budget > 0)
    {
        std::snprintf(text, sizeof(text), measured ? Tr("%s of %s%s, measured size") : Tr("%s of %s%s"),
                      FormatMemoryBytes(used).c_str(), FormatMemoryBytes(budget).c_str(), Share(used, budget).c_str());
    }
    else
    {
        std::snprintf(text, sizeof(text), "%s", FormatMemoryBytes(used).c_str());
    }
    ui.ValueText(text);
}

void MemoryPanel::DrawSceneView()
{
    auto& ui = EditorUI::Get();
    Project* project = m_App->GetProject();
    if (!project)
    {
        EmptyStateText(Tr("Open a project to see what its scenes take."));
        return;
    }

    // The target: the project's active one until another is picked.
    const std::vector<const PlatformConfig*> targets = ProjectTargets(project->GetPath());
    if (!m_EstimateValid && m_TargetId.empty() && m_EstimateScene == nullptr)
    {
        m_TargetId = project->GetActivePlatformId();
        if (m_TargetId.empty())
        {
            for (const PlatformConfig* t : targets)
            {
                if (t->family != "native")
                {
                    m_TargetId = t->id;  // the first board, the one budgets matter on
                    break;
                }
            }
        }
    }

    // Worked out again when the scene changes, and every few seconds while it
    // is edited; it reads image files, so not every frame.
    const auto now = std::chrono::steady_clock::now();
    if (!m_EstimateValid || m_EstimateScene != m_App->GetCurrentScene() ||
        now - m_EstimateTime > std::chrono::seconds(3))
    {
        RefreshEstimate();
    }
    const SceneMemoryEstimate& e = m_Estimate;

    if (DekiEditor::SchematicSectionBegin("Target"))
    {
        DekiEditor::BeginPropertyContext();
        InspectorUtils::BeginPropertyRow("Target");
        {
            std::vector<std::string> labels{ TargetLabel(nullptr) };
            int current = 0;
            for (size_t i = 0; i < targets.size(); ++i)
            {
                labels.push_back(TargetLabel(targets[i]));
                if (targets[i]->id == m_TargetId)
                {
                    current = static_cast<int>(i + 1);
                }
            }
            std::vector<const char*> items;
            for (const std::string& l : labels)
            {
                items.push_back(l.c_str());
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (SchematicCombo("##target", &current, items.data(), static_cast<int>(items.size())))
            {
                m_TargetId = current == 0 ? "" : targets[current - 1]->id;
                m_EstimateValid = false;
            }
        }

        DrawRegionBar("Internal RAM", e.internalBytes, e.internalBudget, e.internalBudgetMeasured);
        if (e.hasExternal)
        {
            DrawRegionBar("External RAM", e.externalBytes, e.externalBudget, false);
        }
        if (!e.rendering.empty())
        {
            InspectorUtils::BeginPropertyRow("Frames");
            ui.ValueText(Tr(e.rendering.front().name.c_str()));
        }
        DekiEditor::EndPropertyContext();
        if (!e.frameModeNote.empty())
        {
            ui.Hint(Tr(e.frameModeNote.c_str()));
        }

        if (!e.targetId.empty() && e.internalBudget == 0)
        {
            ui.Hint(Tr("A target's description does not give its internal RAM. Read a board in Live and it is "
                       "measured against what the board has."));
        }
        else if (e.internalBudgetMeasured)
        {
            ui.Hint(Tr("Internal RAM is the heap the board reported in Live. The board's own system takes the "
                       "rest of its RAM."));
        }
        DekiEditor::SchematicSectionEnd();
    }

    // Everything in one list, biggest first, so the few that matter are at
    // the top whatever kind they are.
    struct Row
    {
        std::string name;
        std::string kind;
        std::string detail;
        uint64_t bytes;
        bool external;
        bool fileSize;
        std::string guid;
        Deki::Object* object;
    };
    std::vector<Row> biggest;
    for (const MemoryEstimateItem& r : e.rendering)
    {
        biggest.push_back({ Tr(r.name.c_str()), Tr(r.kind.c_str()), r.detail, r.bytes, false, false, "", nullptr });
    }
    for (const MemoryEstimateItem& a : e.assets)
    {
        biggest.push_back({ a.name, a.kind, a.detail, a.bytes, a.external, a.fileSize, a.guid, nullptr });
    }
    for (const ObjectMemoryEstimate& o : e.objects)
    {
        biggest.push_back({ o.path, Tr("Object"), "", o.ownBytes, false, false, "", o.object });
    }
    std::sort(biggest.begin(), biggest.end(), [](const Row& a, const Row& b) { return a.bytes > b.bytes; });

    auto selectAsset = [this](const std::string& guid)
    {
        AssetPipeline* pipeline = m_App->GetAssetPipeline();
        const AssetInfo* info = pipeline ? pipeline->GetAssetInfoByGuid(guid) : nullptr;
        if (info)
        {
            m_App->SetSelectedAsset(pipeline->GetAbsolutePath(info->path), pipeline->GetAbsolutePath(info->cachePath),
                                    info->hasCachedVersion, guid);
        }
    };
    auto whereText = [](bool external) { return external ? Tr("External") : Tr("Internal"); };

    if (DekiEditor::SchematicSectionBegin("Biggest"))
    {
        if (BeginSizeTable("##biggest", 4))
        {
            ui.TableSetupColumn(TrLabel("Name"), EditorUI::TableColumnWidthStretch);
            ui.TableSetupColumn(TrLabel("Kind"), EditorUI::TableColumnWidthFixed, m_App->Dpi(90.0f));
            ui.TableSetupColumn(TrLabel("Where"), EditorUI::TableColumnWidthFixed, m_App->Dpi(70.0f));
            ui.TableSetupColumn(TrLabel("Size"), EditorUI::TableColumnWidthFixed, m_App->Dpi(80.0f));
            ui.TableHeadersRow();
            const size_t shown = std::min<size_t>(biggest.size(), 10);
            for (size_t i = 0; i < shown; ++i)
            {
                const Row& r = biggest[i];
                ui.PushID(static_cast<int>(i));
                ui.TableNextRow();
                ui.TableNextColumn();
                if (ui.Selectable(r.name.c_str(), false, true))
                {
                    if (r.object)
                    {
                        m_App->SetSelectedObject(r.object);
                    }
                    else if (!r.guid.empty())
                    {
                        selectAsset(r.guid);
                    }
                }
                if (!r.detail.empty() && ui.IsItemHovered())
                {
                    ui.SetTooltip(r.fileSize ? Tr("Its built file's size: what it takes once loaded is not worked "
                                                  "out for this kind of asset")
                                             : r.detail.c_str());
                }
                ui.TableNextColumn();
                TextIn(Palette::Dim, r.kind.c_str());
                ui.TableNextColumn();
                TextIn(r.external ? Palette::Dim : Palette::Fg, whereText(r.external));
                ui.TableNextColumn();
                ui.Text(FormatMemoryBytes(r.bytes).c_str());
                ui.PopID();
            }
            ui.EndTable();
        }
        DekiEditor::SchematicSectionEnd();
    }

    if (DekiEditor::SchematicSectionBegin("Rendering"))
    {
        DekiEditor::BeginPropertyContext();
        for (const MemoryEstimateItem& r : e.rendering)
        {
            InspectorUtils::BeginPropertyRow(r.name.c_str());
            char text[128];
            std::snprintf(text, sizeof(text), "%s  %s", FormatMemoryBytes(r.bytes).c_str(), r.detail.c_str());
            ui.ValueText(text);
        }
        DekiEditor::EndPropertyContext();
        if (e.rendering.empty())
        {
            ui.Hint(Tr("This target has no screen size, so there is no frame to size."));
        }
        else
        {
            ui.Hint(Tr("In internal RAM. A display driver adds buffers of its own, which Live shows."));
        }
        DekiEditor::SchematicSectionEnd();
    }

    char assetsTitle[64];
    std::snprintf(assetsTitle, sizeof(assetsTitle), "%s (%d)###assets", Tr("Assets"),
                  static_cast<int>(e.assets.size()));
    if (DekiEditor::SchematicSectionBegin(assetsTitle))
    {
        if (e.assets.empty())
        {
            ui.Hint(Tr("No component in this scene names an asset."));
        }
        else if (BeginSizeTable("##assets", 5))
        {
            ui.TableSetupColumn(TrLabel("Asset"), EditorUI::TableColumnWidthStretch);
            ui.TableSetupColumn(TrLabel("Stored As"), EditorUI::TableColumnWidthFixed, m_App->Dpi(120.0f));
            ui.TableSetupColumn(TrLabel("Users"), EditorUI::TableColumnWidthFixed, m_App->Dpi(50.0f));
            ui.TableSetupColumn(TrLabel("Where"), EditorUI::TableColumnWidthFixed, m_App->Dpi(70.0f));
            ui.TableSetupColumn(TrLabel("Size"), EditorUI::TableColumnWidthFixed, m_App->Dpi(80.0f));
            ui.TableHeadersRow();
            for (size_t i = 0; i < e.assets.size(); ++i)
            {
                const MemoryEstimateItem& a = e.assets[i];
                ui.PushID(static_cast<int>(i));
                ui.TableNextRow();
                ui.TableNextColumn();
                if (ui.Selectable(a.name.c_str(), false, true))
                {
                    selectAsset(a.guid);
                }
                if (ui.IsItemHovered())
                {
                    ui.SetTooltip(a.kind.c_str());
                }
                ui.TableNextColumn();
                TextIn(Palette::Dim, a.fileSize ? Tr("file size") : a.detail.c_str());
                ui.TableNextColumn();
                ui.Text(std::to_string(a.users).c_str());
                ui.TableNextColumn();
                TextIn(a.external ? Palette::Dim : Palette::Fg, whereText(a.external));
                ui.TableNextColumn();
                ui.Text(FormatMemoryBytes(a.bytes).c_str());
                ui.PopID();
            }
            ui.EndTable();
        }
        DekiEditor::SchematicSectionEnd();
    }

    char objectsTitle[64];
    std::snprintf(objectsTitle, sizeof(objectsTitle), "%s (%d)###objects", Tr("Objects"),
                  static_cast<int>(e.objects.size()));
    if (DekiEditor::SchematicSectionBegin(objectsTitle))
    {
        if (BeginSizeTable("##objects", 4))
        {
            ui.TableSetupColumn(TrLabel("Object"), EditorUI::TableColumnWidthStretch);
            ui.TableSetupColumn(TrLabel("Components"), EditorUI::TableColumnWidthFixed, m_App->Dpi(80.0f));
            ui.TableSetupColumn(TrLabel("Own"), EditorUI::TableColumnWidthFixed, m_App->Dpi(70.0f));
            ui.TableSetupColumn(TrLabel("Assets"), EditorUI::TableColumnWidthFixed, m_App->Dpi(80.0f));
            ui.TableHeadersRow();
            ui.BeginListClipper(static_cast<int>(e.objects.size()));
            int start = 0, end = 0;
            while (ui.ListClipperStep(&start, &end))
            {
                for (int i = start; i < end; ++i)
                {
                    const ObjectMemoryEstimate& o = e.objects[i];
                    ui.PushID(i);
                    ui.TableNextRow();
                    ui.TableNextColumn();
                    if (ui.Selectable(o.path.c_str(), false, true))
                    {
                        m_App->SetSelectedObject(o.object);
                    }
                    if (ui.IsItemHovered() && !o.components.empty())
                    {
                        std::string list;
                        for (const std::string& c : o.components)
                        {
                            list += (list.empty() ? "" : "\n") + c;
                        }
                        ui.SetTooltip(list.c_str());
                    }
                    ui.TableNextColumn();
                    TextIn(Palette::Dim, std::to_string(o.components.size()).c_str());
                    ui.TableNextColumn();
                    ui.Text(FormatMemoryBytes(o.ownBytes).c_str());
                    ui.TableNextColumn();
                    TextIn(o.assetBytes ? Palette::Fg : Palette::Dim, FormatMemoryBytes(o.assetBytes).c_str());
                    ui.PopID();
                }
            }
            ui.EndListClipper();
            ui.EndTable();
        }
        ui.Hint(Tr("Own is the object and its components as this editor lays them out; on a 32-bit board they are "
                   "smaller. What a component allocates when the game runs (a particle pool, a text cache) shows in "
                   "Live. Assets counts each asset the object uses in full, though objects share them."));
        DekiEditor::SchematicSectionEnd();
    }

    ui.Hint(Tr("Everything here is an estimate, worked out from the scene and the target without running it. Live "
               "shows what a running game measures."));
}

// ---------------------------------------------------------------------------
// Live: measured
// ---------------------------------------------------------------------------

void MemoryPanel::StartDeviceQuery(const std::string& port)
{
    auto job = std::make_shared<DeviceJob>();
    job->port = port;
    m_Job = job;
    std::thread(
        [job]()
        {
            MemoryReport report;
            std::string error;
            const bool ok = QueryDeviceMemory(job->port, 5000, report, error);
            std::lock_guard<std::mutex> lock(job->mutex);
            job->ok = ok;
            job->error = error;
            job->report = std::move(report);
            job->done = true;
        })
        .detach();
}

void MemoryPanel::RefreshLive(bool force)
{
    const auto now = std::chrono::steady_clock::now();

    // A board's answer, when one is due.
    if (m_Job)
    {
        std::lock_guard<std::mutex> lock(m_Job->mutex);
        if (m_Job->done)
        {
            if (m_Job->ok)
            {
                m_DeviceReport = m_Job->report;
                m_HasDeviceReport = true;
                m_DevicePort = m_Job->port;
                m_LiveError.clear();
                m_EstimateValid = false;  // the Scene view's budget may have changed
            }
            else
            {
                m_LiveError = m_Job->error;
            }
            m_LiveTime = now;
            m_Job.reset();
        }
    }

    if (m_Source == 0)
    {
        if (force || (m_Auto && now - m_LiveTime > std::chrono::milliseconds(500)))
        {
            m_Live = CaptureLocalMemory();
            m_HasLive = true;
            m_LiveError.clear();
            m_LiveTime = now;
        }
        return;
    }

    const size_t port = static_cast<size_t>(m_Source - 1);
    if (port < m_Ports.size() && !m_Job && (force || (m_Auto && now - m_LiveTime > std::chrono::seconds(2))))
    {
        StartDeviceQuery(m_Ports[port]);
    }
}

void MemoryPanel::DrawLiveView()
{
    auto& ui = EditorUI::Get();
    RefreshLive(false);

    if (DekiEditor::SchematicSectionBegin("Source"))
    {
        DekiEditor::BeginPropertyContext();
        InspectorUtils::BeginPropertyRow("Source");
        {
            // Ports are listed when the list opens, not every frame.
            if (m_Ports.empty() || ImGui::IsWindowAppearing())
            {
                m_Ports = EnumerateSerialPorts();
            }
            std::vector<std::string> labels{ Tr("Play mode in this editor") };
            for (const std::string& p : m_Ports)
            {
                char label[96];
                std::snprintf(label, sizeof(label), Tr("Board on %s"), p.c_str());
                labels.push_back(label);
            }
            std::vector<const char*> items;
            for (const std::string& l : labels)
            {
                items.push_back(l.c_str());
            }
            m_Source = std::min(m_Source, static_cast<int>(items.size()) - 1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (SchematicCombo("##source", &m_Source, items.data(), static_cast<int>(items.size())))
            {
                m_LiveError.clear();
                RefreshLive(true);
            }
        }
        if (m_Source == 0)
        {
            InspectorUtils::BeginPropertyRow("Since Play Started");
            SchematicCheckbox("##sinceplay", &m_SincePlayStart);
        }
        DekiEditor::EndPropertyContext();
        if (m_Source == 0)
        {
            ui.Hint(m_App->IsPlaying() ? Tr("The editor runs the game in its own process, so the edit scene's "
                                            "memory is there too. Since Play Started leaves it out.")
                                       : Tr("Not playing: this is the editor's own scene. Press Play to measure "
                                            "the game."));
        }
        else
        {
            ui.Hint(Tr("Asks the board over its serial port every two seconds while Auto is on. Close the Serial "
                       "Monitor first if it has the port open."));
        }
        DekiEditor::SchematicSectionEnd();
    }

    const bool fromBoard = m_Source > 0;
    if (fromBoard && m_Job && !m_HasDeviceReport)
    {
        TextIn(Palette::Dim, Tr("Asking the board..."));
        return;
    }
    if (!m_LiveError.empty())
    {
        TextIn(Palette::Red, m_LiveError.c_str());
        if (!fromBoard || !m_HasDeviceReport)
        {
            return;
        }
    }
    if (fromBoard ? !m_HasDeviceReport : !m_HasLive)
    {
        return;
    }

    MemoryReport report = fromBoard ? m_DeviceReport : m_Live;
    const MemoryReport* start = PlayStartMemory();
    if (!fromBoard && m_SincePlayStart && m_App->IsPlaying() && start)
    {
        report = SincePlayStart(report, *start);
    }

    if (DekiEditor::SchematicSectionBegin("Regions"))
    {
        DekiEditor::BeginPropertyContext();
        auto region = [&](const char* label, uint64_t used, uint64_t peak, int64_t free, int64_t minFree, int64_t total)
        {
            InspectorUtils::BeginPropertyRow(label);
            std::string text = FormatMemoryBytes(used);
            char part[96];
            std::snprintf(part, sizeof(part), Tr(", peak %s"), FormatMemoryBytes(peak).c_str());
            text += part;
            if (free >= 0)
            {
                std::snprintf(part, sizeof(part), Tr(", %s free"),
                              FormatMemoryBytes(static_cast<uint64_t>(free)).c_str());
                text += part;
            }
            if (minFree >= 0)
            {
                std::snprintf(part, sizeof(part), Tr(", lowest %s"),
                              FormatMemoryBytes(static_cast<uint64_t>(minFree)).c_str());
                text += part;
            }
            if (total > 0)
            {
                std::snprintf(part, sizeof(part), Tr(" of %s"),
                              FormatMemoryBytes(static_cast<uint64_t>(total)).c_str());
                text += part;
            }
            ui.ValueText(text.c_str());
        };
        region("Internal RAM", report.internalUsed, report.internalPeak, report.internalFree, report.internalMinFree,
               report.internalTotal);
        if (report.externalTotal > 0 || report.externalUsed > 0)
        {
            region("External RAM", report.externalUsed, report.externalPeak, report.externalFree,
                   report.externalMinFree, report.externalTotal);
        }
        DekiEditor::EndPropertyContext();
        ui.Hint(Tr("The first figure is what the engine placed; peak is the most it held at once. Free, lowest and "
                   "the size come from the heap and also cover what the engine does not place: containers, the "
                   "board's own system."));
        DekiEditor::SchematicSectionEnd();
    }

    // Each category's sites, biggest category first.
    struct Category
    {
        const char* name;
        uint64_t bytes = 0;
        uint64_t peak = 0;
        uint32_t blocks = 0;
        bool external = false;
        std::vector<const MemoryReportSite*> sites;
    };
    std::map<std::string, Category> byName;
    for (const MemoryReportSite& s : report.sites)
    {
        const char* name = MemoryCategory(s);
        Category& c = byName[name];
        c.name = name;
        c.bytes += s.Bytes();
        c.peak += s.peakBytes;
        c.blocks += s.blocks;
        c.external = c.external || s.externalBytes > 0;
        c.sites.push_back(&s);
    }
    std::vector<Category> categories;
    for (auto& [name, c] : byName)
    {
        std::sort(c.sites.begin(), c.sites.end(),
                  [](const MemoryReportSite* a, const MemoryReportSite* b) { return a->Bytes() > b->Bytes(); });
        categories.push_back(c);
    }
    std::sort(categories.begin(), categories.end(), [](const Category& a, const Category& b)
              { return a.bytes != b.bytes ? a.bytes > b.bytes : a.peak > b.peak; });

    if (DekiEditor::SchematicSectionBegin("By Category"))
    {
        if (categories.empty())
        {
            ui.Hint(Tr("No call sites: this build strips them (DEKI_STRIP_ALLOC_CONTEXT), or its firmware is older "
                       "than the memory report."));
        }
        else if (BeginSizeTable("##categories", 4))
        {
            ui.TableSetupColumn(TrLabel("What"), EditorUI::TableColumnWidthStretch);
            ui.TableSetupColumn(TrLabel("Blocks"), EditorUI::TableColumnWidthFixed, m_App->Dpi(55.0f));
            ui.TableSetupColumn(TrLabel("Now"), EditorUI::TableColumnWidthFixed, m_App->Dpi(80.0f));
            ui.TableSetupColumn(TrLabel("Peak"), EditorUI::TableColumnWidthFixed, m_App->Dpi(80.0f));
            ui.TableHeadersRow();
            int id = 0;
            for (const Category& c : categories)
            {
                const bool open =
                    std::find(m_OpenCategories.begin(), m_OpenCategories.end(), c.name) != m_OpenCategories.end();
                ui.PushID(id++);
                ui.TableNextRow();
                ui.TableNextColumn();
                char label[160];
                std::snprintf(label, sizeof(label), "%s %s", open ? ICON_TI_CHEVRON_DOWN : ICON_TI_CHEVRON_RIGHT,
                              Tr(c.name));
                if (ui.Selectable(label, false, true))
                {
                    if (open)
                    {
                        m_OpenCategories.erase(std::find(m_OpenCategories.begin(), m_OpenCategories.end(), c.name));
                    }
                    else
                    {
                        m_OpenCategories.push_back(c.name);
                    }
                }
                ui.TableNextColumn();
                TextIn(Palette::Dim, std::to_string(c.blocks).c_str());
                ui.TableNextColumn();
                TextIn(c.bytes ? Palette::Fg : Palette::Dim, FormatMemoryBytes(c.bytes).c_str());
                ui.TableNextColumn();
                TextIn(Palette::Dim, FormatMemoryBytes(c.peak).c_str());
                if (open)
                {
                    for (const MemoryReportSite* s : c.sites)
                    {
                        ui.PushID(s);
                        ui.TableNextRow();
                        ui.TableNextColumn();
                        const std::string name =
                            s->function.empty() ? std::string(Tr("Other call sites")) : ShortFunction(s->function);
                        char row[320];
                        std::snprintf(row, sizeof(row), "      %s", name.c_str());
                        TextIn(Palette::Fg, row);
                        if (ui.IsItemHovered() && !s->file.empty())
                        {
                            char tip[512];
                            std::snprintf(tip, sizeof(tip), "%s\n%s:%u\n%s %s, %s %s", s->function.c_str(),
                                          s->file.c_str(), s->line, Tr("Internal"),
                                          FormatMemoryBytes(s->internalBytes).c_str(), Tr("External"),
                                          FormatMemoryBytes(s->externalBytes).c_str());
                            ui.SetTooltip(tip);
                        }
                        ui.TableNextColumn();
                        TextIn(Palette::Dim, std::to_string(s->blocks).c_str());
                        ui.TableNextColumn();
                        TextIn(s->Bytes() ? Palette::Fg : Palette::Dim, FormatMemoryBytes(s->Bytes()).c_str());
                        ui.TableNextColumn();
                        TextIn(Palette::Dim, FormatMemoryBytes(s->peakBytes).c_str());
                        ui.PopID();
                    }
                }
                ui.PopID();
            }
            ui.EndTable();
        }
        ui.Hint(Tr("Measured by call site: each place in the code that allocates through the engine, grouped by the "
                   "file it is in. A category's peak adds up its call sites' peaks, which need not have come at the "
                   "same time."));
        DekiEditor::SchematicSectionEnd();
    }
}

}  // namespace DekiEditor
