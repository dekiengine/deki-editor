#include "PlatformEditorDialog.h"
#include <vector>
#include <cfloat>
#include "inspector/InspectorUtils.h"
#include <deki-editor/EditorTheme.h>
#include "EditorApp.h"
#include <deki-editor/build/BuilderRegistry.h>
#include <deki-editor/build/TargetBuilder.h>
#include <deki-editor/IconsTabler.h>
#include "imgui.h"
#include <cstring>
#include <algorithm>

namespace DekiEditor
{

// ============================================================================
// Shared utility: editable string list (used by builder UIs)
// ============================================================================


// ============================================================================
// Constructor / Destructor
// ============================================================================

PlatformEditorDialog::PlatformEditorDialog(EditorApp* app)
    : m_App(app)
{
}

PlatformEditorDialog::~PlatformEditorDialog() = default;

// ============================================================================
// Builder list
// ============================================================================

void PlatformEditorDialog::PopulateBuilderList()
{
    m_AvailableBuilders.clear();

    // Purely from what is registered. There used to be a hardcoded "Desktop
    // Simulator" row here as well as NativeBuilder's own, so the simulator
    // appeared twice; and a hardcoded list cannot show a builder the editor
    // was not compiled with, which is the whole point of the plugin path.
    BuilderRegistry::Instance().EnsureLoaded();
    for (auto* builder : BuilderRegistry::Instance().GetAllBuilders())
    {
        if (builder == nullptr)
            continue;
        BuilderEntry entry;
        entry.name = builder->GetName();
        entry.frameworkId = builder->GetFrameworkId();
        entry.icon = builder->GetIcon();
        entry.description = builder->GetDescription();
        m_AvailableBuilders.push_back(std::move(entry));
    }

    // Custom last: a platform whose framework no installed builder claims.
    m_AvailableBuilders.push_back({ "Custom", "", "custom", nullptr, "" });
}

void PlatformEditorDialog::ApplyBuilderDefaults(int builderIndex)
{
    if (builderIndex < 0 || builderIndex >= (int)m_AvailableBuilders.size())
        return;

    m_FamilyIndex = builderIndex;

    // Create editor UI from builder with default config
    const auto& entry = m_AvailableBuilders[builderIndex];
    m_EditorUI.reset();

    if (!entry.frameworkId.empty())
    {
        auto* builder = BuilderRegistry::Instance().GetBuilder(entry.frameworkId);
        if (builder)
        {
            PlatformConfig defaultConfig;
            defaultConfig.framework = entry.frameworkId;
            defaultConfig.family = entry.family;
            defaultConfig.screenWidth = 320;
            defaultConfig.screenHeight = 240;
            m_EditorUI = builder->CreateEditorUI(defaultConfig);
        }
    }

    m_ScreenWidth = 320;
    m_ScreenHeight = 240;
}

// ============================================================================
// Show / Reset / Load
// ============================================================================

void PlatformEditorDialog::ShowNew()
{
    m_Open = true;
    m_JustOpened = true;
    m_IsEditing = false;
    m_OriginalId.clear();
    m_ErrorMessage.clear();
    m_ShowError = false;

    ResetToDefaults();
    PopulateBuilderList();
    m_ShowFrameworkPicker = true;
    m_SelectedBuilderIndex = -1;
}

void PlatformEditorDialog::ShowEdit(const PlatformConfig* platform)
{
    if (!platform)
        return;

    m_Open = true;
    m_JustOpened = true;
    m_IsEditing = true;
    m_OriginalId = platform->id;
    m_ErrorMessage.clear();
    m_ShowError = false;
    m_ShowFrameworkPicker = false;

    PopulateBuilderList();
    LoadFromConfig(platform);
}

void PlatformEditorDialog::ResetToDefaults()
{
    strcpy(m_Id, "");
    strcpy(m_DisplayName, "");
    m_FamilyIndex = 0;
    m_SelectedBuilderIndex = -1;
    m_ScreenWidth = 320;
    m_ScreenHeight = 240;
    m_Loaded = PlatformConfig{};
    m_AssetStorageIndex = 0;
    m_EditorUI.reset();
}

void PlatformEditorDialog::LoadFromConfig(const PlatformConfig* config)
{
    strncpy(m_Id, config->id.c_str(), sizeof(m_Id) - 1);
    strncpy(m_DisplayName, config->displayName.c_str(), sizeof(m_DisplayName) - 1);

    // Find builder index matching this platform's family
    m_FamilyIndex = (int)m_AvailableBuilders.size() - 1;  // Default to Custom (last)
    for (int i = 0; i < (int)m_AvailableBuilders.size(); i++)
    {
        if (!m_AvailableBuilders[i].frameworkId.empty() &&
            m_AvailableBuilders[i].frameworkId == config->framework)
        {
            m_FamilyIndex = i;
            break;
        }
    }
    m_SelectedBuilderIndex = m_FamilyIndex;

    // Native fallback fields
    m_ScreenWidth = config->screenWidth > 0 ? config->screenWidth : 320;
    m_ScreenHeight = config->screenHeight > 0 ? config->screenHeight : 240;

    m_Loaded = *config;
    m_AssetStorageIndex = config->assetStorage == "external" ? 1 : 0;

    // Create editor UI from builder (loads all fields from config)
    CreateEditorUI(*config);
}

void PlatformEditorDialog::CreateEditorUI(const PlatformConfig& config)
{
    m_EditorUI.reset();

    if (!config.framework.empty())
    {
        auto* builder = BuilderRegistry::Instance().GetBuilderForPlatform(config);
        if (builder)
        {
            m_EditorUI = builder->CreateEditorUI(config);
        }
    }
}

// ============================================================================
// BuildConfig / Validate
// ============================================================================

PlatformConfig PlatformEditorDialog::BuildConfig()
{
    PlatformConfig config = m_IsEditing ? m_Loaded : PlatformConfig{};

    config.id = m_Id;
    config.displayName = m_DisplayName;

    // Set family from selected builder
    if (m_FamilyIndex >= 0 && m_FamilyIndex < (int)m_AvailableBuilders.size())
    {
        config.family = m_AvailableBuilders[m_FamilyIndex].family;
        config.framework = m_AvailableBuilders[m_FamilyIndex].frameworkId;
    }
    else
    {
        config.family = "custom";
    }

    config.assetStorage = m_AssetStorageIndex == 1 ? "external" : "internal";

    // Delegate builder-specific fields to editor UI
    if (m_EditorUI)
    {
        m_EditorUI->ApplyToConfig(config);
    }
    else
    {
        // Native / Custom fallback
        config.screenWidth = m_ScreenWidth;
        config.screenHeight = m_ScreenHeight;

        if (config.framework.empty())
            config.framework = "custom";
    }

    // bootScene is auto-managed by Platform Manager
    if (m_IsEditing)
    {
        auto* existing = PlatformRegistry::Instance().GetPlatform(m_OriginalId);
        if (existing)
            config.bootScene = existing->bootScene;
    }

    return config;
}

bool PlatformEditorDialog::Validate(std::string& errorMessage)
{
    if (strlen(m_Id) == 0)
    {
        errorMessage = "Platform ID cannot be empty.";
        return false;
    }

    std::string id(m_Id);
    for (char c : id)
    {
        if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
        {
            errorMessage = "Platform ID contains invalid characters. Use lowercase letters, numbers, and underscores.";
            return false;
        }
    }

    if (strlen(m_DisplayName) == 0)
    {
        errorMessage = "Display name cannot be empty.";
        return false;
    }

    if (!m_IsEditing || m_OriginalId != m_Id)
    {
        if (PlatformRegistry::Instance().GetPlatform(m_Id) != nullptr)
        {
            errorMessage = "A platform with this ID already exists.";
            return false;
        }
    }

    return true;
}

// ============================================================================
// Draw (windowed)
// ============================================================================

void PlatformEditorDialog::Draw()
{
    if (!m_Open)
        return;

    // Two steps, two dialogs: pick a framework, then edit the platform.
    // Opening the second at the same level closes the first.
    const char* title = m_ShowFrameworkPicker ? "New Platform##framework"
                                              : (m_IsEditing ? "Edit Platform" : "New Platform");
    if (!ImGui::IsPopupOpen(title))
        ImGui::OpenPopup(title);

    if (m_ShowFrameworkPicker)
    {
        if (!BeginSchematicModal(title, 440.0f))
        {
            m_Open = false;  // closed (Escape): it reopens itself while m_Open
            return;
        }
        DrawFrameworkPicker();
        if (SchematicDialogButtons({ { "Cancel" } }) == 0)
        {
            m_Open = false;
            ImGui::CloseCurrentPopup();
        }
        EndSchematicModal();
        return;
    }

    if (!BeginSchematicModal(title, 560.0f))
    {
        m_Open = false;  // closed (Escape): it reopens itself while m_Open
        return;
    }

    ImGui::BeginChild("PlatformEditorContent", ImVec2(0.0f, 520.0f * ImGui::GetStyle().FontScaleDpi), false);
    DrawBasicSection();
    ImGui::Spacing();
    // Builder-specific UI or native fallback
    if (m_EditorUI)
        m_EditorUI->Draw();
    else
        DrawNativeEditor();
    DrawStorageSection();
    ImGui::EndChild();

    if (m_ShowError && !m_ErrorMessage.empty())
    {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Red);
        ImGui::TextWrapped("%s", m_ErrorMessage.c_str());
        ImGui::PopStyleColor();
    }

    const int button = SchematicDialogButtons({ { "Cancel" }, { "Save", DialogButton::Primary } });
    if (button == 1)
    {
        if (Validate(m_ErrorMessage))
        {
            PlatformConfig config = BuildConfig();
            if (m_OnPlatformSaved)
                m_OnPlatformSaved(config, !m_IsEditing);
            m_Open = false;
            ImGui::CloseCurrentPopup();
        }
        else
        {
            m_ShowError = true;
        }
    }
    else if (button == 0)
    {
        m_Open = false;
        ImGui::CloseCurrentPopup();
    }
    EndSchematicModal();
}

// ============================================================================
// DrawInline - embed in parent panel (no window wrapper)
// Returns true if Save was clicked successfully.
// ============================================================================

bool PlatformEditorDialog::DrawInline()
{
    if (!m_Open)
        return false;

    if (m_ShowFrameworkPicker)
    {
        DrawFrameworkPicker();
        if (SchematicDialogButtons({ { "Cancel" } }) == 0)
            m_Open = false;
        return false;
    }

    DrawBasicSection();
    ImGui::Spacing();

    if (m_EditorUI)
        m_EditorUI->Draw();
    else
        DrawNativeEditor();
    DrawStorageSection();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (m_ShowError && !m_ErrorMessage.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Red);
        ImGui::TextWrapped("%s", m_ErrorMessage.c_str());
        ImGui::PopStyleColor();
    }

    bool saved = false;
    const int button = SchematicDialogButtons({ { "Cancel" }, { "Save", DialogButton::Primary } });
    if (button == 1)
    {
        if (Validate(m_ErrorMessage))
        {
            PlatformConfig config = BuildConfig();
            if (m_OnPlatformSaved)
                m_OnPlatformSaved(config, !m_IsEditing);
            m_Open = false;
            saved = true;
        }
        else
        {
            m_ShowError = true;
        }
    }
    else if (button == 0)
    {
        m_Open = false;
    }

    return saved;
}

// ============================================================================
// Framework Picker (step 1 of creating a new platform)
// ============================================================================

// Fallbacks for a builder that does not describe itself. A builder's own
// GetIcon()/GetDescription() win, so a third-party backend is not anonymous.
static const char* GetFrameworkIcon(const char* builderIcon, const std::string& frameworkId)
{
    if (builderIcon != nullptr)
        return builderIcon;
    if (frameworkId.empty())
        return ICON_TI_TOOL;  // the Custom row
    return ICON_TI_CPU;       // a backend that did not pick one
}

static std::string GetFrameworkDescription(const std::string& builderDesc,
                                           const std::string& frameworkId)
{
    if (!builderDesc.empty())
        return builderDesc;
    // Only the Custom row is the editor's own; every real framework is
    // described by the backend that provides it.
    if (frameworkId.empty())
        return "Configure a custom platform with manual settings";
    return "";
}

void PlatformEditorDialog::DrawFrameworkPicker()
{
    ImGui::TextColored(DekiEditor::Palette::Dim, "Pick the framework the platform builds with.");
    ImGui::Spacing();

    float cardHeight = 72.0f;
    float iconAreaWidth = 40.0f;
    float padding = 8.0f;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImU32 iconColor = ImGui::GetColorU32(ImGuiCol_Text);
    ImU32 descColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    for (int i = 0; i < (int)m_AvailableBuilders.size(); i++)
    {
        const auto& entry = m_AvailableBuilders[i];
        const char* icon = GetFrameworkIcon(entry.icon, entry.frameworkId);
        const std::string desc = GetFrameworkDescription(entry.description, entry.frameworkId);

        ImGui::PushID(i);

        ImVec2 pos = ImGui::GetCursorScreenPos();

        if (ImGui::Selectable("##framework", false, 0, ImVec2(0, cardHeight)))
        {
            m_SelectedBuilderIndex = i;
            m_ShowFrameworkPicker = false;
            ApplyBuilderDefaults(i);
        }

        float iconY = pos.y + (cardHeight - ImGui::GetTextLineHeight()) * 0.5f;
        drawList->AddText(ImVec2(pos.x + padding, iconY), iconColor, icon);

        float textX = pos.x + iconAreaWidth + padding;
        float nameY = pos.y + padding;
        drawList->AddText(ImVec2(textX, nameY), iconColor, entry.name.c_str());

        float descY = pos.y + padding + ImGui::GetTextLineHeight() + 2.0f;
        drawList->AddText(ImVec2(textX, descY), descColor, desc.c_str());

        ImGui::PopID();
    }

}

// ============================================================================
// Basic Info section (id, displayName, framework dropdown)
// ============================================================================

void PlatformEditorDialog::DrawBasicSection()
{
    if (!DekiEditor::SchematicSectionBegin("Basic Info"))
        return;
    DekiEditor::BeginPropertyContext();

    InspectorUtils::SetNextRowTooltip("Unique identifier: lowercase letters, digits and '_' (e.g. my_board).");
    InspectorUtils::BeginPropertyRow("Platform ID");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (m_JustOpened)
    {
        ImGui::SetKeyboardFocusHere();
        m_JustOpened = false;
    }
    ImGui::InputText("##PlatformId", m_Id, sizeof(m_Id));

    InspectorUtils::SetNextRowTooltip("The name the editor shows for it.");
    InspectorUtils::BeginPropertyRow("Display Name");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##DisplayName", m_DisplayName, sizeof(m_DisplayName));

    InspectorUtils::BeginPropertyRow("Framework");
    std::vector<const char*> frameworks;
    for (const auto& b : m_AvailableBuilders)
        frameworks.push_back(b.name.c_str());
    int framework = m_FamilyIndex;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!frameworks.empty() &&
        DekiEditor::SchematicCombo("##Framework", &framework, frameworks.data(), static_cast<int>(frameworks.size())) &&
        framework != m_FamilyIndex)
    {
        ApplyBuilderDefaults(framework);
    }

    // Custom family hint
    if (m_FamilyIndex >= 0 && m_FamilyIndex < (int)m_AvailableBuilders.size() &&
        m_AvailableBuilders[m_FamilyIndex].frameworkId.empty())
    {
        InspectorUtils::BeginPropertyRow("");
        ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Dim);
        ImGui::TextWrapped("A custom platform builds with your own toolchain, set up in the project's CMakeLists.txt.");
        ImGui::PopStyleColor();
    }

    DekiEditor::EndPropertyContext();
    DekiEditor::SchematicSectionEnd();
}

// ============================================================================
// Storage: where the target keeps its assets. Only a target with external
// storage (an SD card) has a choice; every other one keeps them inside.
// ============================================================================

void PlatformEditorDialog::DrawStorageSection()
{
    if (!m_IsEditing || m_Loaded.Capabilities().count("external-storage") == 0)
        return;
    if (!DekiEditor::SchematicSectionBegin("Storage"))
        return;
    DekiEditor::BeginPropertyContext();
    InspectorUtils::BeginPropertyRow("Assets On");
    ImGui::SetNextItemWidth(-FLT_MIN);
    DekiEditor::SchematicCombo("##AssetStorage", &m_AssetStorageIndex, "Internal storage\0External storage\0");
    InspectorUtils::BeginPropertyRow("");
    ImGui::TextColored(DekiEditor::Palette::Dim, "%s",
                       m_AssetStorageIndex == 1 ? "Copy the assets to the card with SD Card Deploy."
                                                : "The assets are flashed with the game.");
    DekiEditor::EndPropertyContext();
    DekiEditor::SchematicSectionEnd();
}

// ============================================================================
// Native editor (fallback for Desktop Simulator / Custom)
// ============================================================================

void PlatformEditorDialog::DrawNativeEditor()
{
    if (!DekiEditor::SchematicSectionBegin("Display"))
        return;
    DekiEditor::BeginPropertyContext();
    InspectorUtils::SetNextRowTooltip("The screen the editor previews for this platform, in pixels.");
    InspectorUtils::BeginPropertyRow("Resolution");
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float xW = ImGui::CalcTextSize("x").x;
    const float fieldW = (ImGui::GetContentRegionAvail().x - xW - gap * 2.0f) * 0.5f;
    ImGui::SetNextItemWidth(fieldW);
    DekiEditor::SchematicDragInt("##ScreenWidth", &m_ScreenWidth, 1.0f, 1, 16384);
    ImGui::SameLine(0.0f, gap);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(DekiEditor::Palette::Dim, "x");
    ImGui::SameLine(0.0f, gap);
    ImGui::SetNextItemWidth(fieldW);
    DekiEditor::SchematicDragInt("##ScreenHeight", &m_ScreenHeight, 1.0f, 1, 16384);
    m_ScreenWidth = std::max(m_ScreenWidth, 1);
    m_ScreenHeight = std::max(m_ScreenHeight, 1);
    DekiEditor::EndPropertyContext();
    DekiEditor::SchematicSectionEnd();
}

}  // namespace DekiEditor
