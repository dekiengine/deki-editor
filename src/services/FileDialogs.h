#pragma once

#include <string>

namespace DekiEditor
{
namespace FileDialogs
{

// Open a folder picker dialog
// Returns the selected folder path, or empty string if cancelled
std::string OpenFolder(const char* title = "Select Folder");

// Open a file picker dialog
// filter: Windows-style filter string (e.g. "Text Files\0*.txt\0All Files\0*.*\0")
// defaultPath: Optional initial directory
// Returns the selected file path, or empty string if cancelled
std::string OpenFile(const char* filter, const char* defaultPath = nullptr);

// Open a save file dialog
// filter: Windows-style filter string
// defaultExt: Default extension to append if none specified
// defaultPath: Optional initial directory
// Returns the selected file path, or empty string if cancelled
std::string SaveFile(const char* filter, const char* defaultExt, const char* defaultPath = nullptr);

}  // namespace FileDialogs
}  // namespace DekiEditor
