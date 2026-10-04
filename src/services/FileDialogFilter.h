#pragma once

#include <string>
#include <vector>

namespace DekiEditor
{
namespace FileDialogs
{

/// One entry of a file-type filter, in the form the portable dialogs want: a
/// readable name and a semicolon-separated list of bare extensions.
struct DialogFilter
{
    std::string name;     // "Scene Files (*.scene)"
    std::string pattern;  // "scene", or "png;jpg", or "*" for everything
};

/// Parses a Win32 filter string (NUL-separated pairs of name and spec, ended by
/// a double NUL) into the portable form:
///
///   "Scene Files (*.scene)\0*.scene\0"  ->  { {"Scene Files (*.scene)", "scene"} }
///
/// A spec of "*.*" or "*" becomes "*", which the dialogs read as "any file".
/// A null or empty filter gives an empty list, which means the same.
std::vector<DialogFilter> ParseWin32Filter(const char* filter);

}  // namespace FileDialogs
}  // namespace DekiEditor
