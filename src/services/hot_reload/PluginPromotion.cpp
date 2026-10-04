#include "PluginPromotion.h"

#include "../SharedLibNaming.h"

#include <filesystem>
#include <system_error>

namespace DekiEditor
{
namespace PluginPromotion
{

namespace fs = std::filesystem;

Result PromoteStaged(const std::string& runtimeDir)
{
    Result result;
    if (runtimeDir.empty())
    {
        return result;
    }

    const fs::path binDir(runtimeDir);
    const fs::path tempDir = binDir / ".build_temp";

    std::error_code ec;
    if (!fs::exists(tempDir, ec))
    {
        return result;  // no staged build; nothing to do
    }

    // Collect first: renaming entries out from under an open directory iterator
    // is undefined.
    std::vector<fs::path> staged;
    for (const auto& entry : fs::directory_iterator(tempDir, ec))
    {
        const auto ext = entry.path().extension();
        // PDBs move too, or the debugger keeps the old one locked. (MSVC only -
        // ELF/Mach-O carry their debug info inside the library.)
        if (ext == kSharedLibExt || ext == ".pdb")
        {
            staged.push_back(entry.path());
        }
    }

    for (const fs::path& src : staged)
    {
        const fs::path dest = binDir / src.filename();
        std::error_code moveEc;
        fs::remove(dest, moveEc);  // replace whatever is there
        fs::rename(src, dest, moveEc);
        if (!moveEc)
        {
            ++result.promoted;
            result.promotedFiles.push_back(dest);
            continue;
        }
        result.failures.push_back(src.filename().string() + ": " + moveEc.message());
    }
    return result;
}

}  // namespace PluginPromotion
}  // namespace DekiEditor
