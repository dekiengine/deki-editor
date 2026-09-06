#include "SafeNames.h"

#include <system_error>

namespace fs = std::filesystem;

namespace DekiEditor::SafeNames
{

bool IsSafeName(const std::string& name, std::string& reason)
{
    if (name.empty())
    {
        reason = "empty name";
        return false;
    }
    if (name.size() > 100)
    {
        reason = "name is longer than 100 characters";
        return false;
    }
    if (name == "." || name == "..")
    {
        reason = "name may not be '.' or '..'";
        return false;
    }
    // A leading dot hides the directory and reaches ".git"; a leading dash
    // becomes an option for any tool the name is later passed to.
    if (name.front() == '.' || name.front() == '-')
    {
        reason = "name may not start with '.' or '-'";
        return false;
    }
    for (unsigned char c : name)
    {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok)
        {
            reason = "name contains a character outside A-Z, a-z, 0-9, '.', '_', '-'";
            return false;
        }
    }
    return true;
}

bool IsSafeRepoSlug(const std::string& slug, std::string& reason)
{
    const size_t slash = slug.find('/');
    if (slash == std::string::npos)
        return IsSafeName(slug, reason);
    if (slug.find('/', slash + 1) != std::string::npos)
    {
        reason = "repository must be 'owner/name'";
        return false;
    }
    return IsSafeName(slug.substr(0, slash), reason) && IsSafeName(slug.substr(slash + 1), reason);
}

bool IsSafeSerialPort(const std::string& port, std::string& reason)
{
    if (port.empty())
    {
        reason = "empty serial port";
        return false;
    }
    if (port.size() > 64)
    {
        reason = "serial port name is too long";
        return false;
    }
    auto safeChars = [](const std::string& s, size_t from) {
        for (size_t i = from; i < s.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            if (!ok) return false;
        }
        return true;
    };
    if (port.rfind("COM", 0) == 0 && port.size() > 3)
    {
        for (size_t i = 3; i < port.size(); ++i)
            if (port[i] < '0' || port[i] > '9')
            {
                reason = "a COM port is COM followed by digits";
                return false;
            }
        return true;
    }
    if (port.rfind("/dev/", 0) == 0 && port.size() > 5 && safeChars(port, 5))
        return true;
    reason = "serial port must be COM<n> or /dev/<name>";
    return false;
}

bool IsSafeDefine(const std::string& define, std::string& reason)
{
    if (define.empty())
    {
        reason = "empty define";
        return false;
    }
    const size_t eq = define.find('=');
    const std::string name = define.substr(0, eq);
    if (name.empty() || !((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_'))
    {
        reason = "define must start with a letter or '_'";
        return false;
    }
    for (unsigned char c : name)
    {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok)
        {
            reason = "define name contains a character outside A-Z, a-z, 0-9, '_'";
            return false;
        }
    }
    if (eq != std::string::npos)
    {
        for (size_t i = eq + 1; i < define.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(define[i]);
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
            if (!ok)
            {
                reason = "define value contains a character outside A-Z, a-z, 0-9, '_', '.'";
                return false;
            }
        }
    }
    return true;
}

bool IsInsideDirectory(const fs::path& child, const fs::path& parent)
{
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(child, ec);
    if (ec) return false;
    const fs::path p = fs::weakly_canonical(parent, ec);
    if (ec) return false;

    // Component-wise prefix test: a string prefix would let "packages-evil"
    // pass for "packages".
    auto ci = c.begin();
    for (auto pi = p.begin(); pi != p.end(); ++pi, ++ci)
    {
        if (pi->empty()) continue;  // trailing separator
        if (ci == c.end() || *ci != *pi)
            return false;
    }
    return true;
}

}  // namespace DekiEditor::SafeNames
