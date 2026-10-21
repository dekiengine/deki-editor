#pragma once

#include <functional>
#include <string>

struct SDL_Window;

namespace DekiEditor
{
namespace FileDialogs
{

/// Called with the chosen path, or an empty string if the user cancelled or
/// the dialog failed. Always runs on the main thread from PumpResults(),
/// outside any ImGui frame, so a handler may open a project, save a scene, or
/// do anything else that is unsafe mid-frame.
using ResultCallback = std::function<void(const std::string& path)>;

/// Sets the parent window for the dialogs. Called once at boot.
void SetWindow(SDL_Window* window);

// Each of these returns at once. The callback arrives on a later frame; on
// Linux, whenever the desktop portal answers.
//
// `filter` is the Win32 filter string ("Scene Files (*.scene)\0*.scene\0"),
// which FileDialogFilter converts for the other platforms.
//
// Titles and the file type names in `filter` are English and are shown in the
// editor's language, so callers pass them without Tr.

/// Asks for a folder. `defaultPath`, when given, is where the picker starts.
void OpenFolder(const char* title, ResultCallback onResult, const char* defaultPath = nullptr);

void OpenFile(const char* filter, const char* defaultPath, ResultCallback onResult);

/// Asks for a file to save. `defaultExt` is added when the chosen name has no
/// extension.
void SaveFile(const char* filter, const char* defaultExt, const char* defaultPath, ResultCallback onResult);

/// True between a request and its callback. Disable whatever opened the dialog
/// while this holds, so dialogs do not stack.
///
/// A new request anyway replaces the outstanding one: its handler is dropped
/// and a late answer for it is ignored. This is on purpose: a platform dialog
/// helper can die without answering, and the editor must still be able to
/// open dialogs afterwards.
bool IsPending();

/// Delivers the callbacks of finished dialogs. Called once a frame from
/// EditorApp::ProcessDeferredTasks, before the ImGui frame opens.
void PumpResults();

}  // namespace FileDialogs
}  // namespace DekiEditor
