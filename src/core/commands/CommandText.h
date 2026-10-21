#pragma once

// Helpers for Command::GetDescription, which returns the description in the
// editor's current language. Descriptions are built when asked for, so a
// language switch shows at once in the Edit menu.

#include <deki-editor/Localization.h>
#include <deki/reflection/ComponentRegistry.h>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace DekiEditor
{
namespace CommandText
{

/// printf into a std::string, for a translated format whose arguments (names)
/// have no fixed length.
inline std::string Format(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    va_list measure;
    va_copy(measure, args);
    const int length = std::vsnprintf(nullptr, 0, format, measure);
    va_end(measure);
    std::string text(length > 0 ? static_cast<size_t>(length) : 0u, '\0');
    if (length > 0)
    {
        std::vsnprintf(text.data(), text.size() + 1, format, args);
    }
    va_end(args);
    return text;
}

/// `english` in the current language when the table has it, otherwise
/// `fallback`. For a name stored in one form (a field name) and translated in
/// another (the label the Inspector shows).
inline std::string TranslatedOr(const std::string& english, const std::string& fallback)
{
    const char* shown = Tr(english.c_str());
    return shown == english.c_str() ? fallback : std::string(shown);
}

/// A component type named by its serialized name ("Game::Health"): the name
/// the Inspector shows, translated, or the serialized name itself when there
/// is no translation.
inline std::string ComponentName(const std::string& typeName)
{
    const Deki::ComponentMeta* meta = Deki::ComponentRegistry::Instance().GetMeta(typeName);
    if (!meta)
    {
        return typeName;
    }
    const char* displayName = meta->GetDisplayName();
    return TranslatedOr(displayName ? displayName : meta->name, typeName);
}

}  // namespace CommandText
}  // namespace DekiEditor
