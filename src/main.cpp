// main - argument parsing, then either a CLI verb or the interactive loop.
//
// Both take the same route in: EditorBoot brings the editor up, with a window
// or without one. Nothing about opening a project is decided here, which is
// what keeps `DekiEditor --verify-scenes <project>` and double-clicking the
// editor equivalent.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_opengl.h>
#include "imgui.h"
#include "imgui_internal.h"  // For DockBuilder
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"

#include "EditorApp.h"
#include "EditorBoot.h"
#include "EditorCli.h"
#include "EditorHeadless.h"
#include "EditorVersion.h"
#include "PanelMaximize.h"
#include <deki-editor/EditorTheme.h>  // DrawDockSeams, DrawAllTabTopAccents
#include "services/CrashHandler.h"
#include <deki-editor/build/BuilderRegistry.h>
#include <memory>
#include "services/ProgressModal.h"
#include "services/EditorCapture.h"
#include "services/EditorInput.h"
#include "services/TouchInput.h"
#include "services/FrameTiming.h"
#include "services/Prerequisites.h"
#include <optional>
#include <deki/profiling/Profiler.h>

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>  // _exit
#endif

// The editor's default dock arrangement:
//
//     Hierarchy      |  Scene View / Play View  |
//     ---------------|---------------------------|  Inspector
//     Asset Browser  |  Console                  |
//
// Used on first run (no imgui.ini yet) and by View > Restore Layout, so the
// layout the menu restores is by definition the one the editor ships with.
// Rebuilding replaces whatever is there: windows not named here keep their own
// dock ids and simply stay wherever they were.
static void BuildDefaultDockLayout(ImGuiID dockspace_id, ImVec2 size)
{
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, size);

    // Columns: left strip, right strip, and whatever is left in the middle.
    ImGuiID dock_left, dock_center, dock_right;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.20f, &dock_left, &dock_center);
    ImGui::DockBuilderSplitNode(dock_center, ImGuiDir_Right, 0.25f, &dock_right, &dock_center);

    // Height of the bottom row, as a fraction of the dockspace. Both columns
    // are split by this same value so Asset Browser and Console line up: the
    // left and middle columns span the full height, so equal fractions mean
    // equal heights and one continuous band across the bottom.
    constexpr float kBottomRowRatio = 0.30f;

    // Left column: Hierarchy over Asset Browser.
    ImGuiID dock_left_top, dock_left_bottom;
    ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Down, kBottomRowRatio, &dock_left_bottom, &dock_left_top);

    // Middle column: the views over the Console.
    ImGuiID dock_center_top, dock_center_bottom;
    ImGui::DockBuilderSplitNode(dock_center, ImGuiDir_Down, kBottomRowRatio, &dock_center_bottom, &dock_center_top);

    ImGui::DockBuilderDockWindow("HIERARCHY###Hierarchy", dock_left_top);
    ImGui::DockBuilderDockWindow("ASSET BROWSER###Asset Browser", dock_left_bottom);
    // Both views share the middle node, so they come back as tabs of each other.
    ImGui::DockBuilderDockWindow("SCENE VIEW###Scene View", dock_center_top);
    ImGui::DockBuilderDockWindow("PLAY VIEW###Play View", dock_center_top);
    ImGui::DockBuilderDockWindow("CONSOLE###Console", dock_center_bottom);
    ImGui::DockBuilderDockWindow("INSPECTOR###Inspector", dock_right);

    ImGui::DockBuilderFinish(dockspace_id);
}

int main(int argc, char* argv[])
{
    // Register crash handler as early as possible
    DekiEditor::CrashHandler::Initialize();


    DekiEditor::CliOptions cli;
    const bool parsed = DekiEditor::ParseArgs(argc, argv, cli);

    // A verb means this is a CLI run: headless, and its output has to be able
    // to reach the terminal that started it. --mcp is a GUI run, but it prints
    // the URL (with its session token) a client needs, so it wants a console
    // too.
    const bool headless = DekiEditor::HasCommand(cli);
    if (headless || cli.help || cli.version || cli.mcp || !parsed)
        DekiEditor::AttachParentConsole();

    if (!parsed)
    {
        DekiEditor::PrintUsage(argv[0]);
        return 1;
    }

    if (cli.help)
    {
        DekiEditor::PrintUsage(argv[0]);
        return 0;
    }

    if (cli.version)
    {
        printf("%s\n", DekiEditor::EditorVersionLine().c_str());
        return 0;
    }

    if (headless && !cli.projectPath)
    {
        printf("Error: this command requires a project path\n");
        DekiEditor::PrintUsage(argv[0]);
        return 1;
    }

    // The short list of verbs that read the project directory and nothing
    // else. See EditorCli.h - everything not on it boots.
    if (headless && DekiEditor::IsLightCommand(cli))
        return DekiEditor::RunLightCommand(cli);

    // A project a running editor has open: run the tools there, live, rather
    // than boot a second editor on it.
    if (headless)
    {
        int rc = 0;
        if (DekiEditor::TryRunInLiveEditor(cli, rc))
            return rc;
    }

    // The build-tool check a project open makes, answered while the window
    // and GL come up rather than in the middle of the open.
    DekiEditor::WarmPrerequisitesInBackground();

    DekiEditor::BootOptions boot;
    boot.headless = headless;
    boot.projectPath = cli.projectPath;
    boot.startMcp = cli.mcp;
    boot.mcpPort = cli.mcpPort;

    DekiEditor::BootContext ctx;
    if (!DekiEditor::BootPlatform(boot, ctx))
        return 1;

    DekiEditor::EditorApp editor;
    const bool opened = DekiEditor::BootEditor(boot, ctx, editor);

    if (headless)
    {
        // Every command below runs against a project that opened exactly as it
        // would have in the GUI. If the open failed there is nothing to run
        // against, and BootEditor has already said why.
        const int rc = opened ? DekiEditor::RunCommand(cli, editor) : 1;
        fflush(stdout);
        fflush(stderr);

        // Exit without unwinding, and without ShutdownPlatform. Package DLLs
        // handed engine-side registries function pointers into their own code
        // at load time and nothing unregisters them, so any teardown here
        // risks running through code those registries still point at - the
        // crash-at-exit --verify-scenes used to have. Everything the command
        // produced is already flushed to disk, and the process is a moment
        // from gone, so there is nothing to reclaim.
#ifdef _WIN32
        ExitProcess((UINT)rc);
#else
        _exit(rc);
#endif
    }

    ImGuiIO& io = ImGui::GetIO();

    // Track if we've set up the initial layout
    bool firstFrame = true;

    // Main loop
    bool running = true;
    while (running)
    {
        // The frame's phases, for the MCP tool frame_timing (off until asked).
        const auto frameStart = DekiEditor::FrameTiming::Clock::now();
        std::optional<DekiEditor::FrameTiming::Scope> phase;
        phase.emplace("Frame: events");

        // Note: Profiling BeginFrame/EndFrame is in UpdatePlayMode()
        // so profiler frames match game frames (like Unity).
        // Don't profile MainLoop here - it runs faster than game frames.

        // Poll events
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            DekiEditor::TouchInput::Get().ProcessEvent(event);

            // Quit is a request, not a fact: the editor asks about unsaved
            // changes first and grants it via ConsumeQuitRequest() below.
            if (event.type == SDL_EVENT_QUIT)
                editor.RequestQuit();
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(ctx.window))
                editor.RequestQuit();
            if (event.type == SDL_EVENT_DROP_FILE)
            {
                const char* droppedFile = event.drop.data;
                editor.HandleFileDrop(droppedFile);
            }
            if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED)
            {
                editor.OnWindowFocusGained();
            }
        }

        // A close request that arrived while a long operation was pumping
        // events itself (see ProgressModal) is replayed here.
        if (DekiEditor::ProgressModal::TakeDeferredQuit())
            editor.RequestQuit();
        if (editor.ConsumeQuitRequest())
        {
            running = false;
            break;
        }

        // Process deferred tasks (like hot reload) BEFORE ImGui frame starts
        // This allows RenderProgressModal to safely run its own ImGui frame
        phase.emplace("Frame: deferred tasks");
        editor.ProcessDeferredTasks();
        phase.emplace("Frame: begin ImGui frame");

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        // While capture_sequence records, animations advance exactly one 60 fps
        // frame per captured frame, so a recording shows motion as it looks
        // live even though each capture is slow to write. Set before the
        // automation input below, whose timed steps (a held click, a hover)
        // count the same clock.
        if (DekiEditor::EditorCapture::Get().IsRecordingSequence())
            ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        // Automation input (MCP click, key, ...): after the backend's report of
        // the real cursor so an injected position wins, before NewFrame so
        // ImGui sees it this frame.
        DekiEditor::EditorInput::Get().InjectPending();
        // Touchscreen: same slot and reason as EditorInput's (see TouchInput.h).
        DekiEditor::TouchInput::Get().InjectPending();
        ImGui::NewFrame();

        // Focus requests queued by MCP tools between frames. Applied here, with
        // RestoreTabFocus below, so a docked panel asked for by an automation
        // client is the selected tab before the tab bar is drawn.
        DekiEditor::EditorCapture::Get().ApplyPendingFocus();

        // Restore dock tab focus BEFORE DockSpace processes tab bars.
        // Must happen here so DockNodeUpdateTabBar() sees the correct SelectedTabId.
        editor.RestoreTabFocus();

        // Create dockspace
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGuiWindowFlags window_flags_dock =
            ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_MenuBar;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        // Suppress the menu bar's auto bottom border (imgui.cpp:7609 draws an
        // AA-stroked line when FrameBorderSize > 0). DrawMenuBar draws its own
        // crisp 1px line instead so it visually matches the global toolbar's
        // bottom border.
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        // FramePadding controls menu bar height AND menu item vertical centering.
        // Pin the bar at 36 logical pixels regardless of font size - pad.y is
        // computed to fill the remainder around the current font. DrawMenuBar
        // re-pushes the same pad.y so AlignTextToFramePadding centers items.
        {
            const float scale = ImGui::GetStyle().FontScaleDpi;
            const float barH = 36.0f * scale;          // physical
            const float fontH = ImGui::GetFontSize();  // physical
            const float padY = (barH - fontH) * 0.5f;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(8.0f * scale, padY));
        }

        ImGui::Begin("DockSpace", nullptr, window_flags_dock);
        ImGui::PopStyleVar(5);

        // No project open: the project manager screen owns the window. It
        // draws its own title bar and fills the frame, so the toolbar,
        // dockspace and status bar are all skipped - a dockspace here would
        // put empty panel chrome behind it.
        const bool projectManagerMode = editor.IsProjectManagerMode();

        // Schematic global toolbar (RUN / PAUSE / STOP transport, future
        // mode/target chips). Drawn BEFORE DockSpace so it claims a 36px
        // strip above the docked panels - the DockSpace call below uses
        // ImVec2(0, -statusBarHeight) which fills the remaining height.
        if (!projectManagerMode)
            editor.DrawGlobalToolbar();

        // DockSpace (leave room for status bar at bottom)
        if (!projectManagerMode)
        {
            float statusBarHeight = DekiEditor::ActivityPanel::GetStatusBarHeight();
            ImGuiID dockspace_id = ImGui::GetID("EditorDockSpace");
            // NoWindowMenuButton hides the small dropdown arrow on every dock
            // node. That arrow opens the dock context menu whose "Hide tab bar"
            // entry is a one-way trap (no UI to undo). With it gone, the menu is
            // unreachable - it's the only entry point per imgui.cpp:19583.
            //
            // NoCloseButton hides the close (X) at the dock-node level (the one
            // that closes the currently visible tab). Per-tab close buttons on
            // the tab strip itself remain visible - those come from each
            // window's p_open and are unaffected by this flag.
            // A restored panel's tab is put back in its old slot here, BEFORE the
            // layout below: the tab already exists (the dock request was processed
            // in NewFrame) but the strip has not been drawn yet, so it comes back
            // in the right order on its very first frame instead of snapping.
            DekiEditor::BeginPanelMaximizeFrame();

            ImGui::DockSpace(dockspace_id, ImVec2(0.0f, -statusBarHeight),
                             ImGuiDockNodeFlags_NoWindowMenuButton |
                                 ImGuiDockNodeFlags_NoCloseButton);

            // Set up initial dock layout only if no saved layout exists
            if (firstFrame)
            {
                firstFrame = false;

                // Only build default layout if imgui.ini doesn't exist (first run)
                ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspace_id);
                if (!node || node->IsEmpty())
                    BuildDefaultDockLayout(dockspace_id, viewport->WorkSize);
            }

            // View > Restore Layout. Same builder as first run, so the layout the
            // menu restores is by definition the one the editor ships with.
            if (editor.ConsumeRestoreLayoutRequest())
                BuildDefaultDockLayout(dockspace_id, viewport->WorkSize);

            // Double-click a tab to blow that panel up over the dockspace, and its
            // title bar to put it back. Must run after DockSpace() (the node rect
            // it fills is known only then) and before the panels are submitted, so
            // the geometry it forces lands on their own Begin() this frame.
            DekiEditor::UpdatePanelMaximize(dockspace_id);
        }

        // Update editor. In project manager mode this draws that screen (and
        // its title bar) instead of the menu bar and panels.
        phase.emplace("Frame: editor update (panels)");
        editor.Update();
        phase.emplace("Frame: status bar, dock chrome");

        // Draw status bar at the bottom of the dockspace window
        if (!projectManagerMode)
            editor.DrawStatusBar();

        ImGui::End();

        // Docked panels have no window border (the dock separator is their
        // seam); this re-adds the frame for standalone floating windows.
        DekiEditor::DrawDockSeams();

        // Overlay 1px accent line on top of every selected dock tab so panel
        // tabs visibly read as tabs in the schematic style.
        DekiEditor::DrawAllTabTopAccents();

        // Rendering
        phase.emplace("Frame: ImGui render + GL draw");
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Screenshot / sequence capture reads the backbuffer, so it has to run
        // after the draw data is submitted and before the swap discards it.
        DekiEditor::EditorCapture::Get().ProcessEndOfFrame(
            (int)io.DisplaySize.x, (int)io.DisplaySize.y);

        // Blocks until vsync: time here is waiting, not work.
        phase.emplace("Frame: swap (waits for vsync)");
        SDL_GL_SwapWindow(ctx.window);
        phase.reset();
        DekiEditor::FrameTiming::EndFrame(frameStart);

        // Tracy frame marker - must be called exactly once per frame
        DEKI_PROFILE_FRAME;
    }

    // Save editor layout (panel states + imgui.ini) before ExitProcess skips destructors
    editor.SaveLayout();
    // Likewise the live session: stops the server and removes this editor's
    // sessions/<pid>.json, which ~EditorApp would have done.
    editor.StopMcpServer();
    // ShutdownPlatform destroys the ImGui context, and ExitProcess still runs
    // every DLL's static destructors. Those in deki-editor.dll free ImGui
    // containers (EditorTheme's section stack), and ImGui::MemFree reads the
    // context it was given: a destroyed one crashed every exit after a project
    // had been open, which a debugger shows as a session that never ends.
    // Not before the shutdown on Linux: there the exe and libdeki-editor.so
    // share ImGui's symbols, the backend shutdown's ImGui calls land in the
    // .so's copy, and a released context made every quit a null-pointer crash.
    // _exit below runs no static destructors, so there is nothing to protect.
#ifdef _WIN32
    editor.ReleaseImGuiContext();
#endif

    DekiEditor::ShutdownPlatform(ctx);

#ifndef _WIN32
    editor.ReleaseImGuiContext();
#endif

    // Force-exit to avoid Tracy profiler's static destructor hanging.
    // All meaningful cleanup (editor, ImGui, SDL) is already done above.
#ifdef _WIN32
    ExitProcess(0);
#else
    _exit(0);
#endif

    return 0;
}
