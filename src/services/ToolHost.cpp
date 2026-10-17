#include "ToolHost.h"
#include <deki/LogSystem.h>
#include "../EditorApp.h"
#include "SafeDispatch.h"
#include <deki-editor/EditorWindow.h>
#include <deki-editor/EditorDLL.h>
#include <deki-editor/Localization.h>
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <cstring>

namespace DekiEditor
{

ToolHost::ToolHost(EditorApp* app)
    : m_App(app)
{
}

ToolHost::~ToolHost()
{
    for (auto& instance : m_Windows)
    {
        if (instance.window && instance.window->IsOpen())
        {
            instance.window->OnClose();
        }
    }
}

void ToolHost::ClearWindows()
{
    for (auto& instance : m_Windows)
    {
        if (instance.window && instance.window->IsOpen())
        {
            // Windows (from the app or a package) can keep state across the
            // reload as plain JSON; DiscoverTools() restores it on the
            // recreated instance.
            std::string json;
            if (instance.window->SaveSession(json))
            {
                m_PendingSessions.push_back({ instance.title, std::move(json) });
            }

            instance.window->OnClose();
        }
    }
    m_Windows.clear();
}

void ToolHost::DiscoverTools()
{
    // Share the ImGui context with the engine DLL, for windows that use ImGui.
    Deki::SetImGuiContext(ImGui::GetCurrentContext());

    m_Windows.clear();
    int windowCount = GetRegisteredWindowCount();
    for (int i = 0; i < windowCount; i++)
    {
        const EditorWindowMeta* meta = GetRegisteredWindowMeta(i);
        if (meta && meta->createFunc)
        {
            if (onToolDiscovered)
            {
                onToolDiscovered(meta->title ? meta->title : "");
            }

            WindowInstance instance;
            instance.window.reset(meta->createFunc());
            instance.menuPath = meta->menuPath ? meta->menuPath : meta->title;
            instance.title = meta->title ? meta->title : "";
            instance.createFunc = meta->createFunc;  // to recreate it later
            m_Windows.push_back(std::move(instance));
        }
    }

    // Restore windows that were open (with session state) before ClearWindows.
    for (const auto& pending : m_PendingSessions)
    {
        for (auto& instance : m_Windows)
        {
            if (!instance.window || instance.title != pending.title)
            {
                continue;
            }

            instance.window->SetOpen(true);
            instance.window->OnOpen();
            instance.window->RestoreSession(pending.json);
            break;
        }
    }
    m_PendingSessions.clear();
}

void ToolHost::DrawToolsMenu()
{
    for (auto& instance : m_Windows)
    {
        if (!instance.window)
        {
            continue;
        }

        const char* title = instance.window->GetTitle();
        const char* shortcut = instance.window->GetShortcut();

        // Titles are English from the package; shown in the editor's language.
        if (ImGui::MenuItem(DekiEditor::TrLabel(title), shortcut))
        {
            if (!instance.window->IsOpen())
            {
                instance.window->SetOpen(true);
                instance.window->OnOpen();  // EditorWindow uses the singleton; no context needed
            }
        }
    }
}

void ToolHost::Update(float deltaTime)
{
    for (auto& instance : m_Windows)
    {
        if (!instance.window)
        {
            continue;
        }

        bool isOpen = instance.window->IsOpen();

        if (isOpen)
        {
            const char* title = instance.window->GetTitle();
            if (!title)
            {
                title = "tool";
            }

            // Each call guarded on its own: a buggy OnUpdate must not skip
            // OnGUI for other windows, and a bad OnGUI must not break the
            // rest of ToolHost::Update.
            Guard(title, [&] { instance.window->OnUpdate(deltaTime); });
            Guard(title, [&] { instance.window->OnGUI(); });

            if (!instance.window->IsOpen() && instance.wasOpenLastFrame)
            {
                Guard(title, [&] { instance.window->OnClose(); });

                // Recreate the window, which resets all its state.
                if (instance.createFunc)
                {
                    instance.window.reset(instance.createFunc());
                }
            }
        }

        instance.wasOpenLastFrame = isOpen;
    }
}

void ToolHost::OpenTool(const char* name)
{
    for (auto& instance : m_Windows)
    {
        if (instance.window && std::strcmp(instance.window->GetTitle(), name) == 0)
        {
            if (!instance.window->IsOpen())
            {
                instance.window->SetOpen(true);
                instance.window->OnOpen();
            }
            return;
        }
    }
}

bool ToolHost::OpenToolForFile(const char* filePath, const char* cachePath, const char* toolTitle)
{
    if (!filePath)
    {
        return false;
    }

    if (toolTitle && toolTitle[0])
    {
        for (auto& instance : m_Windows)
        {
            if (instance.window && std::strcmp(instance.window->GetTitle(), toolTitle) == 0)
            {
                instance.window->SetOpen(true);
                instance.window->OnOpen();
                instance.window->OpenFile(filePath, cachePath ? cachePath : "");
                return true;
            }
        }
    }

    std::filesystem::path path(filePath);
    std::string ext = path.extension().string();

    // .asset files: read the JSON "type" field and dispatch through CanOpenAssetType.
    if (ext == ".asset")
    {
        try
        {
            std::ifstream f(filePath);
            nlohmann::json j;
            f >> j;
            std::string assetType = j.value("type", "");

            if (!assetType.empty())
            {
                for (auto& instance : m_Windows)
                {
                    if (instance.window && instance.window->CanOpenAssetType(assetType.c_str()))
                    {
                        instance.window->SetOpen(true);
                        instance.window->OnOpen();
                        instance.window->OpenFile(filePath, cachePath ? cachePath : "");
                        return true;
                    }
                }
            }
        }
        catch (const std::exception& e)
        {
            DEKI_LOG_WARNING("ToolHost: tool window state unreadable (%s); the file is not reopened", e.what());
        }
    }

    // Otherwise match by extension.
    for (auto& instance : m_Windows)
    {
        if (instance.window && instance.window->CanOpenFile(ext.c_str()))
        {
            instance.window->SetOpen(true);
            instance.window->OnOpen();
            instance.window->OpenFile(filePath, cachePath ? cachePath : "");
            return true;
        }
    }

    return false;
}

EditorWindow* ToolHost::GetWindow(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_Windows.size()))
    {
        return nullptr;
    }

    return m_Windows[index].window.get();
}

}  // namespace DekiEditor
