#include "PlatformEditorDialog.h"
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
    PlatformConfig config;

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

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (m_ShowFrameworkPicker)
    {
        ImGui::SetNextWindowSize(ImVec2(400, 350), ImGuiCond_Appearing);

        if (ImGui::Begin("New Platform - Select Framework", &m_Open,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking))
        {
            DrawFrameworkPicker();
        }
        ImGui::End();
    }
    else
    {
        ImGui::SetNextWindowSize(ImVec2(550, 650), ImGuiCond_Appearing);

        const char* title = m_IsEditing ? "Edit Platform" : "New Platform";
        if (ImGui::Begin(title, &m_Open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking))
        {
            ImGui::BeginChild("PlatformEditorContent", ImVec2(0, -60), false);

            DrawBasicSection();
            ImGui::Spacing();

            // Builder-specific UI or native fallback
            if (m_EditorUI)
                m_EditorUI->Draw();
            else
                DrawNativeEditor();

            ImGui::EndChild();

            ImGui::Separator();

            if (m_ShowError && !m_ErrorMessage.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, DekiEditor::Palette::Red);
                ImGui::TextWrapped("%s", m_ErrorMessage.c_str());
                ImGui::PopStyleColor();
            }

            float buttonWidth = 120;
            float spacing = 10;
            float totalWidth = buttonWidth * 2 + spacing;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalWidth) * 0.5f);

            if (ImGui::Button("Save", ImVec2(buttonWidth, 0)))
            {
                if (Validate(m_ErrorMessage))
                {
                    PlatformConfig config = BuildConfig();
                    if (m_OnPlatformSaved)
                        m_OnPlatformSaved(config, !m_IsEditing);
                    m_Open = false;
                }
                else
                {
                    m_ShowError = true;
                }
            }

            ImGui::SameLine(0, spacing);

            if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0)))
            {
                m_Open = false;
            }
        }
        ImGui::End();
    }
}

// ============================================================================
// DrawInline — embed in parent panel (no window wrapper)
// Returns true if Save was clicked successfully.
// ============================================================================

bool PlatformEditorDialog::DrawInline()
{
    if (!m_Open)
        return false;

    if (m_ShowFrameworkPicker)
    {
        DrawFrameworkPicker();
        return false;
    }

    DrawBasicSection();
    ImGui::Spacing();

    if (m_EditorUI)
        m_EditorUI->Draw();
    else
        DrawNativeEditor();

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
    if (ImGui::Button("Save", ImVec2(120, 0)))
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
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)))
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
        return ICON_TI_TOOL;               // the Custom row
    if (frameworkId == "native")
        return ICON_TI_DEVICE_DESKTOP;
    return ICON_TI_CPU;
}

static std::string GetFrameworkDescription(const std::string& builderDesc,
                                           const std::string& frameworkId)
{
    if (!builderDesc.empty())
        return builderDesc;
    if (frameworkId.empty())
        return "Configure a custom platform with manual settings";
    if (frameworkId == "native")
        return "Run your project on desktop with SDL3 simulator";
    if (frameworkId == "espidf")
        return "Build for ESP32, ESP32-S3, and other Espressif chips";
    return "";
}

void PlatformEditorDialog::DrawFrameworkPicker()
{
    ImGui::Text("New Platform");
    ImGui::TextDisabled("Select a framework to get started");
    ImGui::Spacing();
    ImGui::Separator();
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

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Cancel"))
    {
        m_Open = false;
    }
}

// ============================================================================
// Basic Info section (id, displayName, framework dropdown)
// ============================================================================

void PlatformEditorDialog::DrawBasicSection()
{
    if (ImGui::CollapsingHeader("Basic Info", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();

        // ID
        ImGui::Text("Platform ID");
        ImGui::SetNextItemWidth(-1);
        if (m_JustOpened)
        {
            ImGui::SetKeyboardFocusHere();
            m_JustOpened = false;
        }
        ImGui::InputText("##PlatformId", m_Id, sizeof(m_Id));
        if (ImGui::IsItemHovered())
            DekiEditor::Tooltip("Unique identifier (e.g., esp32s3_my_board)");

        ImGui::Spacing();

        // Display Name
        ImGui::Text("Display Name");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##DisplayName", m_DisplayName, sizeof(m_DisplayName));
        if (ImGui::IsItemHovered())
            DekiEditor::Tooltip("Human-readable name shown in UI");

        ImGui::Spacing();

        // Framework dropdown
        ImGui::Text("Framework");
        ImGui::SetNextItemWidth(-1);

        std::string currentFamily;
        if (m_FamilyIndex >= 0 && m_FamilyIndex < (int)m_AvailableBuilders.size())
            currentFamily = m_AvailableBuilders[m_FamilyIndex].name;

        if (ImGui::BeginCombo("##Framework", currentFamily.c_str()))
        {
            for (int i = 0; i < (int)m_AvailableBuilders.size(); i++)
            {
                bool selected = (i == m_FamilyIndex);
                if (ImGui::Selectable(m_AvailableBuilders[i].name.c_str(), selected))
                {
                    ApplyBuilderDefaults(i);
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // Custom family hint
        if (m_FamilyIndex >= 0 && m_FamilyIndex < (int)m_AvailableBuilders.size() &&
            m_AvailableBuilders[m_FamilyIndex].frameworkId.empty())
        {
            ImGui::Spacing();
            ImGui::TextDisabled("Custom platforms use user-defined toolchains.");
            ImGui::TextDisabled("Configure build settings in your project's CMakeLists.txt.");
        }

        ImGui::Unindent();
    }
}

// ============================================================================
// Native editor (fallback for Desktop Simulator / Custom)
// ============================================================================

void PlatformEditorDialog::DrawNativeEditor()
{
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Indent();

        ImGui::Text("Screen Resolution");
        ImGui::SetNextItemWidth(m_App->Dpi(100));
        ImGui::InputInt("##ScreenWidth", &m_ScreenWidth);
        if (m_ScreenWidth < 1) m_ScreenWidth = 1;
        ImGui::SameLine();
        ImGui::Text("x");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(m_App->Dpi(100));
        ImGui::InputInt("##ScreenHeight", &m_ScreenHeight);
        if (m_ScreenHeight < 1) m_ScreenHeight = 1;

        ImGui::Unindent();
    }
}

}  // namespace DekiEditor
