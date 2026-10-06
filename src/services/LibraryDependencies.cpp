#include "LibraryDependencies.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

namespace DekiEditor
{
namespace
{

namespace fs = std::filesystem;

// ELF64 values used below (see the System V gABI).
constexpr uint32_t kPtLoad = 1;
constexpr uint32_t kPtDynamic = 2;
constexpr int64_t kDtNull = 0;
constexpr int64_t kDtNeeded = 1;
constexpr int64_t kDtStrtab = 5;

template <typename T>
bool ReadAt(const std::vector<char>& file, uint64_t offset, T& out)
{
    if (offset > file.size() || file.size() - offset < sizeof(T))
    {
        return false;
    }
    std::memcpy(&out, file.data() + offset, sizeof(T));
    return true;
}

struct Segment
{
    uint64_t offset = 0;
    uint64_t vaddr = 0;
    uint64_t size = 0;
};

}  // namespace

std::vector<std::string> NeededLibraries(const fs::path& library)
{
    std::ifstream in(library, std::ios::binary);
    if (!in)
    {
        return {};
    }
    const std::vector<char> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // 64-bit, little-endian ELF only: the editor runs on x86-64 and arm64.
    if (file.size() < 64 || static_cast<unsigned char>(file[0]) != 0x7f || file[1] != 'E' || file[2] != 'L' ||
        file[3] != 'F' || file[4] != 2 || file[5] != 1)
    {
        return {};
    }

    uint64_t phoff = 0;
    uint16_t phentsize = 0, phnum = 0;
    if (!ReadAt(file, 32, phoff) || !ReadAt(file, 54, phentsize) || !ReadAt(file, 56, phnum) || phentsize < 56)
    {
        return {};
    }

    std::vector<Segment> loads;
    Segment dynamic;
    bool haveDynamic = false;
    for (uint16_t i = 0; i < phnum; ++i)
    {
        const uint64_t ph = phoff + static_cast<uint64_t>(i) * phentsize;
        uint32_t type = 0;
        Segment s;
        if (!ReadAt(file, ph, type) || !ReadAt(file, ph + 8, s.offset) || !ReadAt(file, ph + 16, s.vaddr) ||
            !ReadAt(file, ph + 32, s.size))
        {
            return {};
        }
        if (type == kPtLoad)
        {
            loads.push_back(s);
        }
        else if (type == kPtDynamic)
        {
            dynamic = s;
            haveDynamic = true;
        }
    }
    if (!haveDynamic)
    {
        return {};
    }

    std::vector<uint64_t> neededOffsets;
    uint64_t strtabVaddr = 0;
    for (uint64_t at = dynamic.offset; at + 16 <= dynamic.offset + dynamic.size; at += 16)
    {
        int64_t tag = 0;
        uint64_t value = 0;
        if (!ReadAt(file, at, tag) || !ReadAt(file, at + 8, value) || tag == kDtNull)
        {
            break;
        }
        if (tag == kDtNeeded)
        {
            neededOffsets.push_back(value);
        }
        else if (tag == kDtStrtab)
        {
            strtabVaddr = value;
        }
    }

    // The string table is given as an address; the load segment holding it
    // says where that is in the file.
    uint64_t strtab = 0;
    bool found = false;
    for (const Segment& s : loads)
    {
        if (strtabVaddr >= s.vaddr && strtabVaddr < s.vaddr + s.size)
        {
            strtab = strtabVaddr - s.vaddr + s.offset;
            found = true;
            break;
        }
    }
    if (!found)
    {
        return {};
    }

    std::vector<std::string> needed;
    for (uint64_t offset : neededOffsets)
    {
        const uint64_t start = strtab + offset;
        if (start >= file.size())
        {
            continue;
        }
        const auto begin = file.begin() + static_cast<std::ptrdiff_t>(start);
        needed.emplace_back(begin, std::find(begin, file.end(), '\0'));
    }
    return needed;
}

std::vector<fs::path> DependencyOrder(const std::vector<fs::path>& libraries,
                                      const std::function<std::vector<std::string>(const fs::path&)>& neededOf)
{
    std::map<std::string, fs::path> byName;
    for (const fs::path& library : libraries)
    {
        byName[library.filename().string()] = library;
    }

    std::vector<fs::path> ordered;
    std::set<std::string> done;
    std::set<std::string> visiting;  // a cycle stops here instead of recursing forever
    std::function<void(const std::string&)> visit = [&](const std::string& name)
    {
        if (done.count(name) || visiting.count(name))
        {
            return;
        }
        visiting.insert(name);
        std::vector<std::string> needed = neededOf(byName[name]);
        std::sort(needed.begin(), needed.end());
        for (const std::string& dep : needed)
        {
            if (byName.count(dep))
            {
                visit(dep);
            }
        }
        visiting.erase(name);
        done.insert(name);
        ordered.push_back(byName[name]);
    };

    for (const auto& [name, path] : byName)
    {
        visit(name);
    }
    return ordered;
}

}  // namespace DekiEditor
