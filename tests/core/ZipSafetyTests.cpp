// Zip-slip: an archive whose entries name "../x" or an absolute path must be
// refused before anything is written. The archives are built here byte by
// byte (stored, no compression) so no fixture file and no zip library are
// needed: a local file header per entry, the central directory, the end
// record, and a CRC-32 table.

#include <gtest/gtest.h>

#include <deki-editor/EditorHttpUtils.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using DekiEditor::EditorHttpUtils;

namespace
{

uint32_t Crc32(const std::string& data)
{
    static uint32_t s_Table[256];
    static bool s_Init = false;
    if (!s_Init)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
            {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            s_Table[i] = c;
        }
        s_Init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char b : data)
    {
        crc = s_Table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void Put16(std::string& out, uint32_t v)
{
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void Put32(std::string& out, uint32_t v)
{
    Put16(out, v & 0xFFFF);
    Put16(out, (v >> 16) & 0xFFFF);
}

struct Entry
{
    std::string name;
    std::string data;
};

// A minimal stored zip. Entry names are written verbatim, which is the point:
// a real archiver refuses "../", an attacker's does not.
std::string BuildZip(const std::vector<Entry>& entries)
{
    std::string out;
    std::string central;
    for (const Entry& e : entries)
    {
        const uint32_t offset = static_cast<uint32_t>(out.size());
        const uint32_t crc = Crc32(e.data);
        const uint32_t size = static_cast<uint32_t>(e.data.size());
        // local file header
        Put32(out, 0x04034b50);
        Put16(out, 20);    // version needed
        Put16(out, 0);     // flags
        Put16(out, 0);     // method: stored
        Put16(out, 0);     // time
        Put16(out, 0x21);  // date
        Put32(out, crc);
        Put32(out, size);
        Put32(out, size);
        Put16(out, static_cast<uint32_t>(e.name.size()));
        Put16(out, 0);
        out += e.name;
        out += e.data;
        // central directory record
        Put32(central, 0x02014b50);
        Put16(central, 20);
        Put16(central, 20);
        Put16(central, 0);
        Put16(central, 0);
        Put16(central, 0);
        Put16(central, 0x21);
        Put32(central, crc);
        Put32(central, size);
        Put32(central, size);
        Put16(central, static_cast<uint32_t>(e.name.size()));
        Put16(central, 0);
        Put16(central, 0);
        Put16(central, 0);
        Put16(central, 0);
        Put32(central, 0);
        Put32(central, offset);
        central += e.name;
    }
    const uint32_t centralOffset = static_cast<uint32_t>(out.size());
    out += central;
    Put32(out, 0x06054b50);
    Put16(out, 0);
    Put16(out, 0);
    Put16(out, static_cast<uint32_t>(entries.size()));
    Put16(out, static_cast<uint32_t>(entries.size()));
    Put32(out, static_cast<uint32_t>(central.size()));
    Put32(out, centralOffset);
    Put16(out, 0);
    return out;
}

struct Scratch
{
    fs::path dir;
    Scratch(const char* name)
    {
        dir = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
    }
    ~Scratch()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path Write(const char* file, const std::string& bytes) const
    {
        const fs::path p = dir / file;
        std::ofstream out(p, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return p;
    }
};

}  // namespace

TEST(ZipSafety, EntryNamesThatEscapeAreRecognised)
{
    EXPECT_TRUE(EditorHttpUtils::IsSafeArchiveEntry("ok/file.txt"));
    EXPECT_TRUE(EditorHttpUtils::IsSafeArchiveEntry("dir/"));
    EXPECT_TRUE(EditorHttpUtils::IsSafeArchiveEntry("a..b/c"));
    EXPECT_FALSE(EditorHttpUtils::IsSafeArchiveEntry("../evil.txt"));
    EXPECT_FALSE(EditorHttpUtils::IsSafeArchiveEntry("ok/../../evil.txt"));
    EXPECT_FALSE(EditorHttpUtils::IsSafeArchiveEntry("ok\\..\\evil.txt"));
    EXPECT_FALSE(EditorHttpUtils::IsSafeArchiveEntry("/etc/passwd"));
    EXPECT_FALSE(EditorHttpUtils::IsSafeArchiveEntry("C:\\Windows\\evil.exe"));
    EXPECT_FALSE(EditorHttpUtils::IsSafeArchiveEntry("\\\\server\\share\\x"));
    EXPECT_FALSE(EditorHttpUtils::IsSafeArchiveEntry(""));
    EXPECT_FALSE(EditorHttpUtils::IsSafeArchiveEntry(std::string("a\nb")));
}

TEST(ZipSafety, SlipArchiveIsRefusedAndWritesNothing)
{
    Scratch s("deki_zipslip");
    const fs::path zip = s.Write("slip.zip", BuildZip({ { "ok/file.txt", "fine" }, { "../evil.txt", "escaped" } }));
    const fs::path dest = s.dir / "dest";

    std::string reason;
    EXPECT_FALSE(EditorHttpUtils::ZipEntriesAreSafe(zip.string(), reason));
    EXPECT_NE(reason.find("evil"), std::string::npos) << reason;

    std::string log;
    EXPECT_FALSE(
        EditorHttpUtils::ExtractZip(zip.string(), dest.string(), [&](const std::string& m) { log += m + "\n"; }));
    EXPECT_FALSE(fs::exists(s.dir / "evil.txt"));
    EXPECT_FALSE(fs::exists(dest / "ok" / "file.txt")) << "nothing may be written when one entry escapes";
}

TEST(ZipSafety, PlainArchiveExtracts)
{
    Scratch s("deki_zipplain");
    const fs::path zip = s.Write("plain.zip", BuildZip({ { "ok/", "" }, { "ok/file.txt", "hello" } }));
    const fs::path dest = s.dir / "dest";

    std::string reason;
    EXPECT_TRUE(EditorHttpUtils::ZipEntriesAreSafe(zip.string(), reason)) << reason;
    std::string log;
    ASSERT_TRUE(
        EditorHttpUtils::ExtractZip(zip.string(), dest.string(), [&](const std::string& m) { log += m + "\n"; }))
        << log;
    std::ifstream in(dest / "ok" / "file.txt", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "hello");
}
