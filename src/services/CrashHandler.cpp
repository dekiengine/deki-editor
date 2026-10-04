#include "CrashHandler.h"
#ifndef _MSC_VER
#include "FaultGuard.h"
#include "TouchInput.h"
#endif
#include <deki-editor/EditorTheme.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <functional>
#include <filesystem>
#include <exception>
#include <chrono>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>
#include <Psapi.h>
#include <set>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")
#else
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/wait.h>
#include <ucontext.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <sys/sysctl.h>
#else
#include <dirent.h>
#include <sys/syscall.h>
#endif
#endif

namespace DekiEditor
{

static std::function<void()> s_PreCrashCallback;
static std::string s_RecoveryMessage;
static std::string s_LastDumpPath;
static const char* s_PlayModePhase = nullptr;
static const char* s_PlayModeSceneName = nullptr;
static bool s_SymbolsInitialized = false;

// DbgHelp is not thread-safe, and it is used from more than one thread: the
// symbol refresh started on a worker when a project opens, a fault guard's
// stack capture, a reload's refresh. One lock for every Sym* call. The crash
// paths only wait a little for it, then go ahead: a report beats a deadlock.
static std::recursive_timed_mutex s_SymLock;

void CrashHandler::SetPreCrashCallback(std::function<void()> callback)
{
    s_PreCrashCallback = std::move(callback);
}

void CrashHandler::SetRecoveryMessage(const std::string& message)
{
    s_RecoveryMessage = message;
}

std::string CrashHandler::GetLastDumpPath()
{
    return s_LastDumpPath;
}

void CrashHandler::RefreshSymbols()
{
#ifdef _WIN32
    std::lock_guard<std::recursive_timed_mutex> lock(s_SymLock);
    if (s_SymbolsInitialized)
    {
        SymRefreshModuleList(GetCurrentProcess());
    }
#endif
}

void CrashHandler::SetPlayModeContext(const char* phase, const char* sceneName)
{
    s_PlayModePhase = phase;
    s_PlayModeSceneName = sceneName;
}

void CrashHandler::ClearPlayModeContext()
{
    s_PlayModePhase = nullptr;
    s_PlayModeSceneName = nullptr;
}

static void InvokePreCrashCallback()
{
    s_RecoveryMessage.clear();
    if (s_PreCrashCallback)
    {
        try
        {
            s_PreCrashCallback();
        }
        catch (...)
        {
            // A failed recovery must not stop the crash dialog.
        }
    }
}

static std::string FormatPlayModeContext()
{
    if (!s_PlayModePhase)
    {
        return {};
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "Play Mode Context:\n  Phase: %s\n  Scene: %s\n\n", s_PlayModePhase,
             s_PlayModeSceneName ? s_PlayModeSceneName : "<unknown>");
    return buf;
}

// The executable's directory, for symbol lookup.
static std::string GetBaseDirPath()
{
    const char* basePath = SDL_GetBasePath();
    return basePath ? std::string(basePath) : std::string();
}

// The crash/ folder next to the executable, created when missing.
static std::string GetCrashDirPath()
{
    const char* basePath = SDL_GetBasePath();
    std::string base = basePath ? std::string(basePath) : std::string();
    std::string crashDir = base + "crash/";
    try
    {
        std::filesystem::create_directories(crashDir);
    }
    catch (...)
    { /* never throw from the crash path: this runs while handling a fault, and an exception here loses the report
         entirely */
    }
    return crashDir;
}

// Where the crash log goes, in the crash/ folder.
static std::string GetCrashLogPath()
{
    std::string crashDir = GetCrashDirPath();
    return crashDir + "deki_crash.log";
}

// A timestamped dump file name in the crash/ folder.
static std::string GetTimestampedDumpPath(const char* extension)
{
    std::string base = GetCrashDirPath();
    time_t now = time(nullptr);
    struct tm tmBuf;
#ifdef _WIN32
    localtime_s(&tmBuf, &now);
#else
    localtime_r(&now, &tmBuf);
#endif
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &tmBuf);

    char filename[256];
    snprintf(filename, sizeof(filename), "deki_crash_%s%s", timestamp, extension);

    if (!base.empty())
    {
        return base + filename;
    }
    return filename;
}

static void ShowCrashDialog(const std::string& title, const std::string& callstack)
{
    std::string logPath = GetCrashLogPath();
    FILE* f = fopen(logPath.c_str(), "w");
    if (f)
    {
        fprintf(f, "%s\n\n%s\n\nCrash log: %s\n", title.c_str(), callstack.c_str(), logPath.c_str());
        fclose(f);
    }

    fprintf(stderr, "\n=== DEKI EDITOR CRASH ===\n%s\n\n%s\n\nCrash log written to: %s\n", title.c_str(),
            callstack.c_str(), logPath.c_str());

    // Show an ImGui crash dialog in its own window, apart from the editor's
    // SDL and ImGui state.
    bool sdlWasInit = SDL_WasInit(SDL_INIT_VIDEO) != 0;
    // The editor turned SDL's touch-to-mouse emulation off to read touches
    // itself; this dialog has none of that, so a tap has to click again.
    DekiEditor::TouchInput::RestoreSdlTouchMouse();
    if (!sdlWasInit)
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            // Can't show any GUI
            return;
        }
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // Query DPI for window sizing (before window is created)
    float preDpiScale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (preDpiScale <= 0.0f)
    {
        preDpiScale = 1.0f;
    }

    SDL_Window* crashWindow =
        SDL_CreateWindow(title.c_str(), (int)(700 * preDpiScale), (int)(500 * preDpiScale), SDL_WINDOW_OPENGL);

    if (!crashWindow)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), callstack.c_str(), nullptr);
        return;
    }

    SDL_GLContext glCtx = SDL_GL_CreateContext(crashWindow);
    if (!glCtx)
    {
        SDL_DestroyWindow(crashWindow);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), callstack.c_str(), nullptr);
        return;
    }

    SDL_GL_MakeCurrent(crashWindow, glCtx);
    SDL_GL_SetSwapInterval(1);

    float dpiScale = SDL_GetWindowDisplayScale(crashWindow);
    if (dpiScale <= 0.0f)
    {
        dpiScale = 1.0f;
    }

    ImGuiContext* prevCtx = ImGui::GetCurrentContext();
    ImGuiContext* crashCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(crashCtx);

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // Don't save layout
    // ImGui 1.92's DPI pattern, as in main.cpp: the backend updates
    // style.FontScaleDpi itself; this sets FontSizeBase and scales the style's
    // sizes and padding once.
    io.ConfigDpiScaleFonts = true;
    ImGui::GetStyle().FontSizeBase = 13.0f;
    ImGui::GetStyle().FontScaleMain = 1.0f;
    ImGui::GetStyle().FontScaleDpi = dpiScale;  // Initial DPI scale (auto-DPI only updates on monitor change)
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(dpiScale);

    ImGui_ImplSDL3_InitForOpenGL(crashWindow, glCtx);
    ImGui_ImplOpenGL3_Init("#version 330");

    std::string fullText = callstack + "\nCrash log: " + logPath;
    if (!s_LastDumpPath.empty())
    {
        fullText += "\nDump file: " + s_LastDumpPath;
    }
    bool copied = false;

    bool running = true;
    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT)
            {
                running = false;
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            {
                running = false;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##CrashDialog", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse);

        ImGui::TextColored(DekiEditor::Palette::Red, "Deki Editor has crashed");
        if (!s_RecoveryMessage.empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(DekiEditor::Palette::Green, " - %s", s_RecoveryMessage.c_str());
        }
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextWrapped("%s", title.c_str());
        if (!s_LastDumpPath.empty())
        {
            ImGui::Spacing();
            ImGui::TextColored(DekiEditor::Palette::Accent, "Dump saved: %s", s_LastDumpPath.c_str());
        }
        ImGui::Spacing();

        float buttonAreaHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        ImGui::InputTextMultiline("##callstack", fullText.data(), fullText.size() + 1, ImVec2(-1, -buttonAreaHeight),
                                  ImGuiInputTextFlags_ReadOnly);

        if (ImGui::Button("Copy to Clipboard"))
        {
            SDL_SetClipboardText(fullText.c_str());
            copied = true;
        }
        ImGui::SameLine();
        if (copied)
        {
            ImGui::TextColored(DekiEditor::Palette::Green, "Copied!");
            ImGui::SameLine();
        }
        float closeButtonWidth = 80.0f;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - closeButtonWidth - ImGui::GetStyle().WindowPadding.x);
        if (ImGui::Button("Close", ImVec2(closeButtonWidth, 0)))
        {
            running = false;
        }

        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.15f, 0.15f, 0.15f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(crashWindow);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(crashCtx);

    if (prevCtx)
    {
        ImGui::SetCurrentContext(prevCtx);
    }

    SDL_GL_DestroyContext(glCtx);
    SDL_DestroyWindow(crashWindow);
}

// =============================================================================
// Windows implementation
// =============================================================================

#ifdef _WIN32

static void InitializeSymbols()
{
    if (s_SymbolsInitialized)
    {
        return;
    }

    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);

    // Symbol search path: the exe directory plus the shadow directories
    // (_hotreload_N/), so their PDBs are found.
    std::string searchPath = GetBaseDirPath();
    if (!searchPath.empty() && searchPath.back() == '\\')
    {
        searchPath.pop_back();
    }

    try
    {
        for (const auto& entry : std::filesystem::directory_iterator(GetBaseDirPath()))
        {
            if (entry.is_directory())
            {
                std::string name = entry.path().filename().string();
                if (name.find("_hotreload_") == 0)
                {
                    searchPath += ";" + entry.path().string();
                }
            }
        }
    }
    catch (...)
    { /* never throw from the crash path: this runs while handling a fault, and an exception here loses the report
         entirely */
    }

    SymInitialize(process, searchPath.empty() ? NULL : searchPath.c_str(), TRUE);
    s_SymbolsInitialized = true;
}

static std::string CaptureStackTraceWindows(CONTEXT* context, HANDLE threadHandle = nullptr, int maxFrames = 64)
{
    std::string result;
    HANDLE process = GetCurrentProcess();
    HANDLE thread = threadHandle ? threadHandle : GetCurrentThread();

    STACKFRAME64 frame = {};
    DWORD machineType;

#ifdef _M_X64
    machineType = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context->Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context->Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_IX86)
    machineType = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = context->Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context->Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Esp;
    frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_ARM64)
    machineType = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = context->Pc;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context->Fp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Sp;
    frame.AddrStack.Mode = AddrModeFlat;
#endif

    char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    SYMBOL_INFO* symbol = (SYMBOL_INFO*)symbolBuffer;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    int frameNum = 0;

#ifdef _M_X64
    // When RIP is 0 (null function pointer call), StackWalk64 fails immediately
    // because the first frame has AddrPC == 0. The return address is on the stack
    // at RSP (pushed by the call instruction). Read it to recover the caller.
    if (context->Rip == 0 && context->Rsp != 0)
    {
        char nullCallLine[128];
        snprintf(nullCallLine, sizeof(nullCallLine), "#%-3d <null function pointer call>  (call target was 0x0)\n", 0);
        result += nullCallLine;
        frameNum = 1;

        DWORD64 returnAddr = 0;
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(process, (LPCVOID)context->Rsp, &returnAddr, sizeof(returnAddr), &bytesRead) &&
            bytesRead == sizeof(returnAddr) && returnAddr != 0)
        {
            frame.AddrPC.Offset = returnAddr;
            frame.AddrStack.Offset = context->Rsp + sizeof(DWORD64);
        }
    }
#endif

    while (StackWalk64(machineType, process, thread, &frame, context, NULL, SymFunctionTableAccess64,
                       SymGetModuleBase64, NULL))
    {
        if (frame.AddrPC.Offset == 0)
        {
            break;
        }

        char line[512];
        DWORD64 displacement = 0;

        char packageName[64] = "";
        HMODULE hMod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)frame.AddrPC.Offset, &hMod))
        {
            char modPath[MAX_PATH];
            if (GetModuleFileNameA(hMod, modPath, MAX_PATH))
            {
                const char* lastSlash = strrchr(modPath, '\\');
                if (!lastSlash)
                {
                    lastSlash = strrchr(modPath, '/');
                }
                strncpy(packageName, lastSlash ? lastSlash + 1 : modPath, sizeof(packageName) - 1);
            }
        }

        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol))
        {
            IMAGEHLP_LINE64 lineInfo = {};
            lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD lineDisplacement = 0;

            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &lineInfo))
            {
                snprintf(line, sizeof(line), "#%-3d %s!%s  (%s:%lu)", frameNum, packageName[0] ? packageName : "??",
                         symbol->Name, lineInfo.FileName, lineInfo.LineNumber);
            }
            else
            {
                snprintf(line, sizeof(line), "#%-3d %s!%s  +0x%llx", frameNum, packageName[0] ? packageName : "??",
                         symbol->Name, (unsigned long long)displacement);
            }
        }
        else
        {
            snprintf(line, sizeof(line), "#%-3d %s!0x%llx", frameNum, packageName[0] ? packageName : "??",
                     (unsigned long long)frame.AddrPC.Offset);
        }

        result += line;
        result += "\n";
        frameNum++;

        if (frameNum >= maxFrames)
        {
            break;
        }
    }

    return result;
}

static std::string WriteMiniDump(EXCEPTION_POINTERS* exceptionInfo)
{
    std::string dumpPath = GetTimestampedDumpPath(".dmp");

    HANDLE hFile = CreateFileA(dumpPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return {};
    }

    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    MINIDUMP_EXCEPTION_INFORMATION* pMei = nullptr;

    if (exceptionInfo)
    {
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = exceptionInfo;
        mei.ClientPointers = FALSE;
        pMei = &mei;
    }

    MINIDUMP_TYPE dumpType =
        static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                   MiniDumpWithFullMemoryInfo | MiniDumpWithUnloadedModules);

    BOOL success = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, dumpType, pMei, NULL, NULL);

    CloseHandle(hFile);

    if (success)
    {
        s_LastDumpPath = dumpPath;
        return dumpPath;
    }

    // Clean up failed dump file
    DeleteFileA(dumpPath.c_str());
    return {};
}

static std::string FormatAccessViolationDetails(EXCEPTION_RECORD* record)
{
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && record->ExceptionCode != EXCEPTION_IN_PAGE_ERROR)
    {
        return {};
    }

    if (record->NumberParameters < 2)
    {
        return {};
    }

    const char* accessType;
    switch (record->ExceptionInformation[0])
    {
        case 0: accessType = "READ"; break;
        case 1: accessType = "WRITE"; break;
        case 8: accessType = "EXECUTE"; break;
        default: accessType = "ACCESS"; break;
    }

    ULONG_PTR address = record->ExceptionInformation[1];

    char buf[256];
    if (address < 0x10000)
    {
        snprintf(buf, sizeof(buf), "Attempted to %s address 0x%llx (null pointer dereference)\n", accessType,
                 (unsigned long long)address);
    }
    else
    {
        snprintf(buf, sizeof(buf), "Attempted to %s address 0x%llx\n", accessType, (unsigned long long)address);
    }
    return buf;
}

static std::string FormatRegistersWindows(const CONTEXT* ctx)
{
    std::string result = "Registers:\n";
    char line[128];

#ifdef _M_X64
    snprintf(line, sizeof(line), "  RAX=%016llx  RBX=%016llx\n", ctx->Rax, ctx->Rbx);
    result += line;
    snprintf(line, sizeof(line), "  RCX=%016llx  RDX=%016llx\n", ctx->Rcx, ctx->Rdx);
    result += line;
    snprintf(line, sizeof(line), "  RSI=%016llx  RDI=%016llx\n", ctx->Rsi, ctx->Rdi);
    result += line;
    snprintf(line, sizeof(line), "  RBP=%016llx  RSP=%016llx\n", ctx->Rbp, ctx->Rsp);
    result += line;
    snprintf(line, sizeof(line), "  R8 =%016llx  R9 =%016llx\n", ctx->R8, ctx->R9);
    result += line;
    snprintf(line, sizeof(line), "  R10=%016llx  R11=%016llx\n", ctx->R10, ctx->R11);
    result += line;
    snprintf(line, sizeof(line), "  R12=%016llx  R13=%016llx\n", ctx->R12, ctx->R13);
    result += line;
    snprintf(line, sizeof(line), "  R14=%016llx  R15=%016llx\n", ctx->R14, ctx->R15);
    result += line;
    snprintf(line, sizeof(line), "  RIP=%016llx  EFLAGS=%08lx\n", ctx->Rip, ctx->EFlags);
    result += line;
#elif defined(_M_IX86)
    snprintf(line, sizeof(line), "  EAX=%08lx  EBX=%08lx\n", ctx->Eax, ctx->Ebx);
    result += line;
    snprintf(line, sizeof(line), "  ECX=%08lx  EDX=%08lx\n", ctx->Ecx, ctx->Edx);
    result += line;
    snprintf(line, sizeof(line), "  ESI=%08lx  EDI=%08lx\n", ctx->Esi, ctx->Edi);
    result += line;
    snprintf(line, sizeof(line), "  EBP=%08lx  ESP=%08lx\n", ctx->Ebp, ctx->Esp);
    result += line;
    snprintf(line, sizeof(line), "  EIP=%08lx  EFLAGS=%08lx\n", ctx->Eip, ctx->EFlags);
    result += line;
#elif defined(_M_ARM64)
    for (int i = 0; i < 29; i += 2)
    {
        if (i + 1 < 29)
        {
            snprintf(line, sizeof(line), "  X%-2d=%016llx  X%-2d=%016llx\n", i, ctx->X[i], i + 1, ctx->X[i + 1]);
        }
        else
        {
            snprintf(line, sizeof(line), "  X%-2d=%016llx\n", i, ctx->X[i]);
        }
        result += line;
    }
    snprintf(line, sizeof(line), "  FP =%016llx  LR =%016llx\n", ctx->Fp, ctx->Lr);
    result += line;
    snprintf(line, sizeof(line), "  SP =%016llx  PC =%016llx\n", ctx->Sp, ctx->Pc);
    result += line;
#endif

    return result;
}

static std::string FormatLoadedModules()
{
    std::string result = "\nLoaded Packages:\n";
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return result + "  (failed to enumerate)\n";
    }

    MODULEENTRY32 me = {};
    me.dwSize = sizeof(me);
    if (Module32First(snapshot, &me))
    {
        do
        {
            char line[512];
            snprintf(line, sizeof(line), "  0x%p  %6lu KB  %s\n", (void*)me.modBaseAddr,
                     (unsigned long)(me.modBaseSize / 1024), me.szExePath);
            result += line;
        } while (Module32Next(snapshot, &me));
    }
    CloseHandle(snapshot);
    return result;
}

static std::string CaptureAllThreadStacks(DWORD crashingThreadId)
{
    std::string result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return result;
    }

    DWORD currentPid = GetCurrentProcessId();
    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);

    int threadCount = 0;
    if (Thread32First(snapshot, &te))
    {
        do
        {
            if (te.th32OwnerProcessID != currentPid)
            {
                continue;
            }
            if (te.th32ThreadID == crashingThreadId)
            {
                continue;
            }
            if (threadCount >= 16)
            {
                break;
            }

            HANDLE hThread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE,
                                        te.th32ThreadID);
            if (!hThread)
            {
                continue;
            }

            if (SuspendThread(hThread) != (DWORD)-1)
            {
                CONTEXT ctx = {};
                ctx.ContextFlags = CONTEXT_FULL;

                if (GetThreadContext(hThread, &ctx))
                {
                    char header[64];
                    snprintf(header, sizeof(header), "\n--- Thread %lu ---\n", te.th32ThreadID);
                    result += header;
                    result += CaptureStackTraceWindows(&ctx, hThread, 32);
                }

                ResumeThread(hThread);
            }

            CloseHandle(hThread);
            threadCount++;

        } while (Thread32Next(snapshot, &te));
    }

    CloseHandle(snapshot);
    return result;
}

static const char* GetExceptionCodeString(DWORD code)
{
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION: return "Access Violation";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "Array Bounds Exceeded";
        case EXCEPTION_BREAKPOINT: return "Breakpoint";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "Datatype Misalignment";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "Float Divide by Zero";
        case EXCEPTION_FLT_OVERFLOW: return "Float Overflow";
        case EXCEPTION_FLT_UNDERFLOW: return "Float Underflow";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "Illegal Instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "Integer Divide by Zero";
        case EXCEPTION_INT_OVERFLOW: return "Integer Overflow";
        case EXCEPTION_INVALID_DISPOSITION: return "Invalid Disposition";
        case EXCEPTION_STACK_OVERFLOW: return "Stack Overflow";
        default: return "Unknown Exception";
    }
}

static LONG WINAPI CrashExceptionFilter(EXCEPTION_POINTERS* exceptionInfo)
{
    DWORD code = exceptionInfo->ExceptionRecord->ExceptionCode;

    // Skip non-fatal exceptions (like debugger breakpoints in debug mode)
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Write minidump first - this is the most valuable artifact
    std::string dumpPath = WriteMiniDump(exceptionInfo);

    // Then the user's unsaved work, before anything slow or risky (symbol
    // loading, stack walks): a second fault there ends the process, and the
    // recovery snapshot must already be written.
    InvokePreCrashCallback();

    // Refresh symbols in case new packages were loaded since startup
    std::unique_lock<std::recursive_timed_mutex> symLock(s_SymLock, std::chrono::seconds(2));
    if (!s_SymbolsInitialized)
    {
        InitializeSymbols();
    }
    else
    {
        SymRefreshModuleList(GetCurrentProcess());
    }

    std::string title = std::string("Deki Editor Crash: ") + GetExceptionCodeString(code);

    std::string message;

    char header[512];
    snprintf(header, sizeof(header), "Exception Code: 0x%08lX (%s)\n", code, GetExceptionCodeString(code));
    message += header;

    std::string avDetails = FormatAccessViolationDetails(exceptionInfo->ExceptionRecord);
    if (!avDetails.empty())
    {
        message += avDetails;
    }

    if (!dumpPath.empty())
    {
        message += "Minidump: " + dumpPath + "\n";
    }

    message += "\n";

    message += FormatPlayModeContext();

    message += FormatRegistersWindows(exceptionInfo->ContextRecord);
    message += "\n";

    message += "Crashing Thread Callstack:\n";
    message += CaptureStackTraceWindows(exceptionInfo->ContextRecord);

    std::string otherThreads = CaptureAllThreadStacks(GetCurrentThreadId());
    if (!otherThreads.empty())
    {
        message += "\nOther Threads:";
        message += otherThreads;
    }

    message += FormatLoadedModules();

    ShowCrashDialog(title, message);

    return EXCEPTION_EXECUTE_HANDLER;
}

#else  // Linux / macOS

// =============================================================================
// Unix / macOS implementation
// =============================================================================

static std::string CaptureStackTraceUnix()
{
    std::string result;
    void* frames[64];
    int count = backtrace(frames, 64);
    char** symbols = backtrace_symbols(frames, count);

    if (symbols)
    {
        for (int i = 0; i < count; i++)
        {
            // Enhance with dladdr for better package/symbol info
            Dl_info dlInfo;
            char line[1024];

            if (dladdr(frames[i], &dlInfo) && dlInfo.dli_sname)
            {
                ptrdiff_t offset = (char*)frames[i] - (char*)dlInfo.dli_saddr;
                const char* packageName = dlInfo.dli_fname ? dlInfo.dli_fname : "<unknown>";

                const char* slash = strrchr(packageName, '/');
                if (slash)
                {
                    packageName = slash + 1;
                }

                snprintf(line, sizeof(line), "#%-3d 0x%lx  %s  +0x%tx  [%s]", i, (unsigned long)(uintptr_t)frames[i],
                         dlInfo.dli_sname, offset, packageName);
            }
            else
            {
                snprintf(line, sizeof(line), "#%-3d %s", i, symbols[i]);
            }
            result += line;
            result += "\n";
        }
        free(symbols);
    }
    else
    {
        result = "(failed to capture stack trace)\n";
    }

    return result;
}

static const char* GetSignalString(int sig)
{
    switch (sig)
    {
        case SIGSEGV: return "Segmentation Fault (SIGSEGV)";
        case SIGABRT: return "Abort (SIGABRT)";
        case SIGFPE: return "Floating Point Exception (SIGFPE)";
        case SIGBUS: return "Bus Error (SIGBUS)";
        case SIGILL: return "Illegal Instruction (SIGILL)";
        default: return "Unknown Signal";
    }
}

static std::string FormatFaultDetails(int sig, siginfo_t* info)
{
    std::string result;
    char buf[256];

    if (!info)
    {
        return result;
    }

    if (sig == SIGSEGV)
    {
        const char* reason;
        switch (info->si_code)
        {
            case SEGV_MAPERR: reason = "address not mapped"; break;
            case SEGV_ACCERR: reason = "invalid permissions"; break;
            default: reason = "unknown cause"; break;
        }

        uintptr_t addr = (uintptr_t)info->si_addr;
        if (addr < 0x10000)
        {
            snprintf(buf, sizeof(buf), "Fault address: 0x%lx (%s, null pointer dereference)\n", (unsigned long)addr,
                     reason);
        }
        else
        {
            snprintf(buf, sizeof(buf), "Fault address: 0x%lx (%s)\n", (unsigned long)addr, reason);
        }
        result = buf;
    }
    else if (sig == SIGFPE)
    {
        const char* reason;
        switch (info->si_code)
        {
            case FPE_INTDIV: reason = "integer divide by zero"; break;
            case FPE_INTOVF: reason = "integer overflow"; break;
            case FPE_FLTDIV: reason = "float divide by zero"; break;
            case FPE_FLTOVF: reason = "float overflow"; break;
            case FPE_FLTUND: reason = "float underflow"; break;
            case FPE_FLTRES: reason = "float inexact result"; break;
            default: reason = "unknown arithmetic error"; break;
        }
        snprintf(buf, sizeof(buf), "Arithmetic fault: %s\n", reason);
        result = buf;
    }
    else if (sig == SIGBUS)
    {
        const char* reason;
        switch (info->si_code)
        {
            case BUS_ADRALN: reason = "invalid address alignment"; break;
            case BUS_ADRERR: reason = "nonexistent physical address"; break;
            case BUS_OBJERR: reason = "object-specific hardware error"; break;
            default: reason = "unknown bus error"; break;
        }
        snprintf(buf, sizeof(buf), "Bus error: %s at address 0x%lx\n", reason, (unsigned long)(uintptr_t)info->si_addr);
        result = buf;
    }

    return result;
}

static std::string FormatRegistersUnix(void* ucontextRaw)
{
    if (!ucontextRaw)
    {
        return {};
    }

    std::string result = "Registers:\n";
    char line[128];

#ifdef __APPLE__
    ucontext_t* uc = static_cast<ucontext_t*>(ucontextRaw);
#if defined(__x86_64__)
    auto* mc = uc->uc_mcontext;
    snprintf(line, sizeof(line), "  RAX=%016llx  RBX=%016llx\n", mc->__ss.__rax, mc->__ss.__rbx);
    result += line;
    snprintf(line, sizeof(line), "  RCX=%016llx  RDX=%016llx\n", mc->__ss.__rcx, mc->__ss.__rdx);
    result += line;
    snprintf(line, sizeof(line), "  RSI=%016llx  RDI=%016llx\n", mc->__ss.__rsi, mc->__ss.__rdi);
    result += line;
    snprintf(line, sizeof(line), "  RBP=%016llx  RSP=%016llx\n", mc->__ss.__rbp, mc->__ss.__rsp);
    result += line;
    snprintf(line, sizeof(line), "  R8 =%016llx  R9 =%016llx\n", mc->__ss.__r8, mc->__ss.__r9);
    result += line;
    snprintf(line, sizeof(line), "  R10=%016llx  R11=%016llx\n", mc->__ss.__r10, mc->__ss.__r11);
    result += line;
    snprintf(line, sizeof(line), "  R12=%016llx  R13=%016llx\n", mc->__ss.__r12, mc->__ss.__r13);
    result += line;
    snprintf(line, sizeof(line), "  R14=%016llx  R15=%016llx\n", mc->__ss.__r14, mc->__ss.__r15);
    result += line;
    snprintf(line, sizeof(line), "  RIP=%016llx  RFLAGS=%016llx\n", mc->__ss.__rip, mc->__ss.__rflags);
    result += line;
#elif defined(__aarch64__)
    auto* mc = uc->uc_mcontext;
    for (int i = 0; i < 29; i += 2)
    {
        if (i + 1 < 29)
        {
            snprintf(line, sizeof(line), "  X%-2d=%016llx  X%-2d=%016llx\n", i, mc->__ss.__x[i], i + 1,
                     mc->__ss.__x[i + 1]);
        }
        else
        {
            snprintf(line, sizeof(line), "  X%-2d=%016llx\n", i, mc->__ss.__x[i]);
        }
        result += line;
    }
    snprintf(line, sizeof(line), "  FP =%016llx  LR =%016llx\n", mc->__ss.__fp, mc->__ss.__lr);
    result += line;
    snprintf(line, sizeof(line), "  SP =%016llx  PC =%016llx\n", mc->__ss.__sp, mc->__ss.__pc);
    result += line;
#else
    result += "  (register dump not supported on this architecture)\n";
#endif

#elif defined(__linux__)
    ucontext_t* uc = static_cast<ucontext_t*>(ucontextRaw);
#if defined(__x86_64__)
    snprintf(line, sizeof(line), "  RAX=%016llx  RBX=%016llx\n", (unsigned long long)uc->uc_mcontext.gregs[REG_RAX],
             (unsigned long long)uc->uc_mcontext.gregs[REG_RBX]);
    result += line;
    snprintf(line, sizeof(line), "  RCX=%016llx  RDX=%016llx\n", (unsigned long long)uc->uc_mcontext.gregs[REG_RCX],
             (unsigned long long)uc->uc_mcontext.gregs[REG_RDX]);
    result += line;
    snprintf(line, sizeof(line), "  RSI=%016llx  RDI=%016llx\n", (unsigned long long)uc->uc_mcontext.gregs[REG_RSI],
             (unsigned long long)uc->uc_mcontext.gregs[REG_RDI]);
    result += line;
    snprintf(line, sizeof(line), "  RBP=%016llx  RSP=%016llx\n", (unsigned long long)uc->uc_mcontext.gregs[REG_RBP],
             (unsigned long long)uc->uc_mcontext.gregs[REG_RSP]);
    result += line;
    snprintf(line, sizeof(line), "  R8 =%016llx  R9 =%016llx\n", (unsigned long long)uc->uc_mcontext.gregs[REG_R8],
             (unsigned long long)uc->uc_mcontext.gregs[REG_R9]);
    result += line;
    snprintf(line, sizeof(line), "  R10=%016llx  R11=%016llx\n", (unsigned long long)uc->uc_mcontext.gregs[REG_R10],
             (unsigned long long)uc->uc_mcontext.gregs[REG_R11]);
    result += line;
    snprintf(line, sizeof(line), "  R12=%016llx  R13=%016llx\n", (unsigned long long)uc->uc_mcontext.gregs[REG_R12],
             (unsigned long long)uc->uc_mcontext.gregs[REG_R13]);
    result += line;
    snprintf(line, sizeof(line), "  R14=%016llx  R15=%016llx\n", (unsigned long long)uc->uc_mcontext.gregs[REG_R14],
             (unsigned long long)uc->uc_mcontext.gregs[REG_R15]);
    result += line;
    snprintf(line, sizeof(line), "  RIP=%016llx  EFL=%016llx\n", (unsigned long long)uc->uc_mcontext.gregs[REG_RIP],
             (unsigned long long)uc->uc_mcontext.gregs[REG_EFL]);
    result += line;
#elif defined(__aarch64__)
    for (int i = 0; i < 31; i += 2)
    {
        if (i + 1 < 31)
        {
            snprintf(line, sizeof(line), "  X%-2d=%016llx  X%-2d=%016llx\n", i,
                     (unsigned long long)uc->uc_mcontext.regs[i], i + 1,
                     (unsigned long long)uc->uc_mcontext.regs[i + 1]);
        }
        else
        {
            snprintf(line, sizeof(line), "  X%-2d=%016llx\n", i, (unsigned long long)uc->uc_mcontext.regs[i]);
        }
        result += line;
    }
    snprintf(line, sizeof(line), "  SP =%016llx  PC =%016llx\n", (unsigned long long)uc->uc_mcontext.sp,
             (unsigned long long)uc->uc_mcontext.pc);
    result += line;
#else
    result += "  (register dump not supported on this architecture)\n";
#endif

#else
    result += "  (register dump not supported on this platform)\n";
#endif

    return result;
}

static std::string WriteCoreDump()
{
    // Fork a child process that aborts to generate a core dump.
    // The parent continues to show the crash dialog.
    std::string corePath;

    pid_t child = fork();
    if (child == 0)
    {
        signal(SIGABRT, SIG_DFL);
        abort();
        _exit(1);  // Should not reach here
    }
    else if (child > 0)
    {
        int status = 0;
        waitpid(child, &status, 0);

#ifdef __linux__
        // Read /proc/sys/kernel/core_pattern to report where the core file went
        FILE* f = fopen("/proc/sys/kernel/core_pattern", "r");
        if (f)
        {
            char pattern[256] = {};
            if (fgets(pattern, sizeof(pattern), f))
            {
                size_t len = strlen(pattern);
                if (len > 0 && pattern[len - 1] == '\n')
                {
                    pattern[len - 1] = '\0';
                }
                corePath = std::string("Core pattern: ") + pattern;
            }
            fclose(f);
        }
#endif
        if (corePath.empty())
        {
            corePath = "Core dump requested (check system core dump settings)";
        }
    }

    return corePath;
}

#ifdef __APPLE__
static std::string CaptureOtherThreadsMacOS()
{
    std::string result;
    thread_act_array_t threadList;
    mach_msg_type_number_t threadCount = 0;

    if (task_threads(mach_task_self(), &threadList, &threadCount) != KERN_SUCCESS)
    {
        return result;
    }

    mach_port_t selfThread = mach_thread_self();
    int captured = 0;

    for (mach_msg_type_number_t i = 0; i < threadCount && captured < 16; i++)
    {
        if (threadList[i] == selfThread)
        {
            continue;
        }

        // Suspend and get state
        if (thread_suspend(threadList[i]) != KERN_SUCCESS)
        {
            continue;
        }

        char header[64];
        snprintf(header, sizeof(header), "\n--- Thread %u ---\n", threadList[i]);
        result += header;

        // CaptureStackTraceUnix only works on the current thread; the core
        // dump has every thread's stack, so just note that the thread exists.
        result += "  (see core dump for full stack)\n";

        thread_resume(threadList[i]);
        captured++;
    }

    vm_deallocate(mach_task_self(), (vm_address_t)threadList, threadCount * sizeof(thread_act_t));
    mach_port_deallocate(mach_task_self(), selfThread);

    return result;
}
#elif defined(__linux__)
static std::string CaptureOtherThreadsLinux()
{
    std::string result;
    DIR* taskDir = opendir("/proc/self/task");
    if (!taskDir)
    {
        return result;
    }

    pid_t selfTid = (pid_t)syscall(SYS_gettid);
    int captured = 0;

    struct dirent* entry;
    while ((entry = readdir(taskDir)) != nullptr && captured < 16)
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }

        pid_t tid = (pid_t)atoi(entry->d_name);
        if (tid == 0 || tid == selfTid)
        {
            continue;
        }

        char header[64];
        snprintf(header, sizeof(header), "\n--- Thread %d ---\n", tid);
        result += header;
        result += "  (see core dump for full stack)\n";
        captured++;
    }

    closedir(taskDir);
    return result;
}
#endif

static void CrashSignalHandlerUnix(int sig, siginfo_t* info, void* ucontext)
{
    // Reset the signal handler to default to avoid infinite recursion
    signal(sig, SIG_DFL);

    // The user's unsaved work first. Everything below can fault again (the
    // unwinder in backtrace() can, inside libgcc_s), and a fault in a handler
    // ends the process on the spot, before any snapshot is written.
    InvokePreCrashCallback();

    std::string title = std::string("Deki Editor Crash: ") + GetSignalString(sig);

    std::string message;

    char header[256];
    snprintf(header, sizeof(header), "Signal: %d (%s)\n", sig, GetSignalString(sig));
    message += header;

    std::string faultDetails = FormatFaultDetails(sig, info);
    if (!faultDetails.empty())
    {
        message += faultDetails;
    }

    std::string coreInfo = WriteCoreDump();
    if (!coreInfo.empty())
    {
        message += coreInfo + "\n";
        s_LastDumpPath = coreInfo;
    }

    message += "\n";

    message += FormatPlayModeContext();

    message += FormatRegistersUnix(ucontext);
    message += "\n";

    message += "Crashing Thread Callstack:\n";
    message += CaptureStackTraceUnix();

#ifdef __APPLE__
    std::string otherThreads = CaptureOtherThreadsMacOS();
#elif defined(__linux__)
    std::string otherThreads = CaptureOtherThreadsLinux();
#else
    std::string otherThreads;
#endif
    if (!otherThreads.empty())
    {
        message += "\nOther Threads:";
        message += otherThreads;
    }

    ShowCrashDialog(title, message);

    _exit(1);
}

#endif  // _WIN32

// =============================================================================
// Shared: terminate handler and initialization
// =============================================================================

static void CrashTerminateHandler()
{
    std::string title = "Deki Editor Crash: Unhandled C++ Exception";
    std::string message;

    // Try to extract exception details
    std::exception_ptr eptr = std::current_exception();
    if (eptr)
    {
        try
        {
            std::rethrow_exception(eptr);
        }
        catch (const std::exception& e)
        {
            message = std::string("Exception: ") + e.what() + "\n\n";
            title = std::string("Deki Editor Crash: ") + e.what();
        }
        catch (...)
        {
            message = "An unhandled C++ exception occurred (non-std::exception type).\n\n";
        }
    }
    else
    {
        message = "An unhandled C++ exception occurred.\n\n";
    }

    // The user's unsaved work before the dump and the stack walks, which can
    // fault and end the process (see CrashSignalHandlerUnix).
    InvokePreCrashCallback();

#ifdef _WIN32
    // Write minidump (no exception info, but still captures threads + memory)
    std::string dumpPath = WriteMiniDump(nullptr);
    if (!dumpPath.empty())
    {
        message += "Minidump: " + dumpPath + "\n\n";
    }

    message += FormatPlayModeContext();

    CONTEXT ctx;
    RtlCaptureContext(&ctx);

    // Refresh symbols in case new packages were loaded since startup
    std::unique_lock<std::recursive_timed_mutex> symLock(s_SymLock, std::chrono::seconds(2));
    if (!s_SymbolsInitialized)
    {
        InitializeSymbols();
    }
    else
    {
        SymRefreshModuleList(GetCurrentProcess());
    }

    message += FormatRegistersWindows(&ctx);
    message += "\nCallstack:\n";
    message += CaptureStackTraceWindows(&ctx);

    std::string otherThreads = CaptureAllThreadStacks(GetCurrentThreadId());
    if (!otherThreads.empty())
    {
        message += "\nOther Threads:";
        message += otherThreads;
    }

    message += FormatLoadedModules();
#else
    std::string coreInfo = WriteCoreDump();
    if (!coreInfo.empty())
    {
        message += coreInfo + "\n\n";
        s_LastDumpPath = coreInfo;
    }

    message += "Callstack:\n";
    message += CaptureStackTraceUnix();
#endif

    ShowCrashDialog(title, message);

#ifdef _WIN32
    ExitProcess(1);
#else
    _exit(1);
#endif
}

#ifndef _MSC_VER
// FaultGuard's trace hook: runs inside the fault handler, on the faulting
// thread, when a guard (PlayModeGuard) asked for a stack trace. Same capture
// the fatal path uses, so a contained play-mode crash reads like a real one.
static void FaultGuardTraceHook(void* platformInfo, std::string& outTrace, std::string& outAccess)
{
#if defined(_WIN32)
    auto* ep = static_cast<EXCEPTION_POINTERS*>(platformInfo);
    if (!ep)
    {
        return;
    }
    if (ep->ExceptionRecord)
    {
        outAccess = FormatAccessViolationDetails(ep->ExceptionRecord);
    }
    if (ep->ContextRecord)
    {
        outTrace = CaptureStackTraceWindows(ep->ContextRecord);
    }
#else
    (void)platformInfo;
    outTrace = CaptureStackTraceUnix();
#endif
}
#endif

void CrashHandler::Initialize()
{
#ifndef _MSC_VER
    FaultGuardSetTraceHook(&FaultGuardTraceHook);
#endif
#ifdef _WIN32
    InitializeSymbols();
    SetUnhandledExceptionFilter(CrashExceptionFilter);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = CrashSignalHandlerUnix;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND | SA_SIGINFO;

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
#endif

    std::set_terminate(CrashTerminateHandler);
}

// ---- Public diagnostic helpers (declared in CrashHandler.h) ----

#ifdef _WIN32
// Adds the directory of every loaded package to DbgHelp's symbol search path,
// so plugin DLLs outside the editor's bin/ (such as <project>/bin/ or
// <project>/packages/_hotreload_*/) get their PDBs found.
static void ExtendSymbolSearchPathForLoadedModules()
{
    HANDLE process = GetCurrentProcess();

    char currentPath[4096] = "";
    SymGetSearchPath(process, currentPath, sizeof(currentPath));
    std::string searchPath = currentPath;

    HMODULE packages[1024];
    DWORD bytesNeeded = 0;
    if (!EnumProcessModules(process, packages, sizeof(packages), &bytesNeeded))
    {
        return;
    }

    DWORD count = bytesNeeded / sizeof(HMODULE);
    if (count > 1024)
    {
        count = 1024;
    }

    std::set<std::string> seenDirs;
    for (DWORD i = 0; i < count; ++i)
    {
        char modPath[MAX_PATH];
        if (!GetModuleFileNameA(packages[i], modPath, MAX_PATH))
        {
            continue;
        }
        char* lastSep = strrchr(modPath, '\\');
        if (!lastSep)
        {
            lastSep = strrchr(modPath, '/');
        }
        if (!lastSep)
        {
            continue;
        }
        *lastSep = '\0';
        std::string dir(modPath);
        if (dir.empty())
        {
            continue;
        }
        if (seenDirs.insert(dir).second)
        {
            if (searchPath.find(dir) == std::string::npos)
            {
                if (!searchPath.empty())
                {
                    searchPath += ";";
                }
                searchPath += dir;
            }
        }
    }

    SymSetSearchPath(process, searchPath.c_str());
}
#endif

std::string CrashHandler::CaptureStackTrace(void* contextPtr)
{
#ifdef _WIN32
    if (!contextPtr)
    {
        return {};
    }
    std::lock_guard<std::recursive_timed_mutex> lock(s_SymLock);
    if (!s_SymbolsInitialized)
    {
        InitializeSymbols();
    }
    // Cover plugin/project DLLs that live outside the editor's bin/.
    ExtendSymbolSearchPathForLoadedModules();
    // Pick up packages loaded after Initialize() (plugin DLLs, hot-reload
    // copies). Without their unwind info, StackWalk64 gives garbage frames on x64.
    SymRefreshModuleList(GetCurrentProcess());
    // StackWalk64 changes the CONTEXT, so work on a copy and leave the caller's
    // EXCEPTION_POINTERS->ContextRecord untouched.
    CONTEXT ctxCopy = *static_cast<CONTEXT*>(contextPtr);
    return CaptureStackTraceWindows(&ctxCopy);
#else
    (void)contextPtr;
    return {};
#endif
}

std::string CrashHandler::FormatAccessViolationDetails(void* recordPtr)
{
#ifdef _WIN32
    if (!recordPtr)
    {
        return {};
    }
    return ::DekiEditor::FormatAccessViolationDetails(static_cast<EXCEPTION_RECORD*>(recordPtr));
#else
    (void)recordPtr;
    return {};
#endif
}

std::string CrashHandler::WriteMiniDumpFor(void* exceptionPointersPtr)
{
#ifdef _WIN32
    if (!exceptionPointersPtr)
    {
        return {};
    }
    return WriteMiniDump(static_cast<EXCEPTION_POINTERS*>(exceptionPointersPtr));
#else
    (void)exceptionPointersPtr;
    return {};
#endif
}

}  // namespace DekiEditor
