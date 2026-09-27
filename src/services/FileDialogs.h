#pragma once

#include <functional>
#include <string>

struct SDL_Window;

namespace DekiEditor
{
namespace FileDialogs
{

// Called with the chosen path, or with an empty string if the user cancelled
// or the dialog failed. Always runs on the main thread, from PumpResults(),
// never inside an ImGui frame -- so a handler may open a project, save a
// scene, or do anything else that would be unsafe mid-frame.
using ResultCallback = std::function<void(const std::string& path)>;

// Parent window for the dialogs. Wired once at boot, like ProgressModal's.
void SetWindow(SDL_Window* window);

// Every one of these returns immediately. The callback arrives on a later
// frame; on Linux that is however long the desktop portal takes to answer.
//
// filter is the Win32 filter string ("Scene Files (*.scene)\0*.scene\0"),
// parsed by FileDialogFilter for the platforms that want a different shape.

// defaultPath, when given, is the folder the picker starts in.
void OpenFolder(const char* title, ResultCallback onResult, const char* defaultPath = nullptr);

void OpenFile(const char* filter, const char* defaultPath, ResultCallback onResult);

// defaultExt is appended when the chosen name carries no extension of its own.
void SaveFile(const char* filter, const char* defaultExt, const char* defaultPath,
              ResultCallback onResult);

// True between a request and its callback. Disable whatever opened the dialog
// while this holds, which is how stacking is avoided.
//
// Making a request anyway is allowed and supersedes the outstanding one: its
// handler is dropped and a late answer for it is ignored. That is deliberate.
// A platform dialog helper can die without ever answering, and a state that
// only the answer could clear would leave the editor unable to open any
// dialog again until restart.
bool IsPending();

// Delivers the callbacks of finished dialogs. Called once a frame from
// EditorApp::ProcessDeferredTasks, before the ImGui frame opens.
void PumpResults();

}  // namespace FileDialogs
}  // namespace DekiEditor
