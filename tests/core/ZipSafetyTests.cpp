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
#include <iterator>
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
    // Unix mode, as an archiver on Unix stores it (0xA1FF a symbolic link,
    // whose data is its target); 0 for none.
    uint32_t mode = 0;
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
        Put16(central, e.mode ? 0x031E : 20);  // version made by: Unix when a mode is stored
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
        Put32(central, e.mode << 16);  // external attributes
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

// -----------------------------------------------------------------------------
// Symbolic links. ESP-IDF's archive has some, and tar.exe cannot create a link
// without administrator rights or Developer Mode: it failed the whole extract
// on them. A link is now put in as a copy of what it points to on Windows (on
// Linux and macOS unzip makes the link), so what it points to reads the same
// either way.
// -----------------------------------------------------------------------------

namespace
{
std::string ReadAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}  // namespace

TEST(ZipLinks, ParsedFromTheTwoListings)
{
    const std::string names = "top/\ntop/dir/\ntop/dir/run.sh\ntop/dir/run copy.sh\ntop/linkdir\n";
    const std::string verbose = "drwxr-xr-x  0 0      0           0 Jan 01  1980 top/\r\n"
                                "drwxr-xr-x  0 0      0           0 Jan 01  1980 top/dir/\r\n"
                                "-rwxr-xr-x  0 0      0          20 Jan 01  1980 top/dir/run.sh\r\n"
                                "lrwxrwxrwx  0 0      0           0 Jan 01  1980 top/dir/run copy.sh -> run.sh\r\n"
                                "lrwxrwxrwx  0 0      0           0 Jan 01  1980 top/linkdir -> dir\r\n";
    std::vector<EditorHttpUtils::ArchiveLink> links;
    std::string error;
    ASSERT_TRUE(EditorHttpUtils::ParseArchiveLinks(names, verbose, links, error)) << error;
    ASSERT_EQ(links.size(), 2u);
    EXPECT_EQ(links[0].path, "top/dir/run copy.sh");
    EXPECT_EQ(links[0].target, "run.sh");
    EXPECT_EQ(links[1].path, "top/linkdir");
    EXPECT_EQ(links[1].target, "dir");

    EXPECT_FALSE(EditorHttpUtils::ParseArchiveLinks("a\nb\n", "-rw a\n", links, error)) << "listings out of step";
}

TEST(ZipLinks, CopiedInPlaceAndKeptInside)
{
    Scratch s("deki_ziplinks_copy");
    fs::create_directories(s.dir / "top" / "dir");
    std::ofstream(s.dir / "top" / "dir" / "run.sh", std::ios::binary) << "#!/bin/sh\n";
    std::ofstream(s.dir / "top" / "dir" / "data.txt", std::ios::binary) << "data";

    std::string error;
    // A link to a link and a link into a directory that holds a link: each
    // gets the finished copy whatever the order. A link to nothing is left out.
    ASSERT_TRUE(EditorHttpUtils::MaterializeArchiveLinks(s.dir.string(),
                                                         { { "top/again.sh", "dir/run-esp32.sh" },
                                                           { "top/linkdir", "dir" },
                                                           { "top/dir/run-esp32.sh", "run.sh" },
                                                           { "top/gone.sh", "missing.sh" } },
                                                         error))
        << error;
    EXPECT_EQ(ReadAll(s.dir / "top" / "dir" / "run-esp32.sh"), "#!/bin/sh\n");
    EXPECT_EQ(ReadAll(s.dir / "top" / "again.sh"), "#!/bin/sh\n");
    EXPECT_EQ(ReadAll(s.dir / "top" / "linkdir" / "data.txt"), "data");
    EXPECT_EQ(ReadAll(s.dir / "top" / "linkdir" / "run-esp32.sh"), "#!/bin/sh\n");
    EXPECT_FALSE(fs::exists(s.dir / "top" / "gone.sh"));

    EXPECT_FALSE(EditorHttpUtils::MaterializeArchiveLinks(s.dir.string(), { { "top/evil", "../../outside.txt" } },
                                                          error))
        << "a link out of the archive is refused";
    EXPECT_FALSE(
        EditorHttpUtils::MaterializeArchiveLinks(s.dir.string(), { { "top/evil", "/etc/passwd" } }, error));
}

TEST(ZipLinks, ArchiveWithLinksExtracts)
{
    Scratch s("deki_ziplinks_extract");
    const fs::path zip = s.Write("links.zip", BuildZip({ { "top/", "", 040755 },
                                                         { "top/dir/", "", 040755 },
                                                         { "top/dir/run.sh", "#!/bin/sh\necho hi\n", 0100755 },
                                                         { "top/dir/run copy.sh", "run.sh", 0120777 },
                                                         { "top/linkdir", "dir", 0120777 },
                                                         { "top/dir/data.txt", "hello", 0100644 } }));
    const fs::path dest = s.dir / "dest";

    std::string log, error;
    ASSERT_TRUE(EditorHttpUtils::ExtractZip(zip.string(), dest.string(),
                                            [&](const std::string& m) { log += m + "\n"; }, &error))
        << error << "\n"
        << log;
    EXPECT_EQ(ReadAll(dest / "top" / "dir" / "run copy.sh"), "#!/bin/sh\necho hi\n");
    EXPECT_EQ(ReadAll(dest / "top" / "linkdir" / "data.txt"), "hello");
}
