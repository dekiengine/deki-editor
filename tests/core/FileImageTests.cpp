// The read-only file image a device's game data is flashed as: what is
// written is found again byte for byte, and a damaged image is refused rather
// than read past its end.

#include <gtest/gtest.h>
#include <deki/FileImage.h>

#include <cstring>
#include <string>
#include <vector>

using namespace Deki;

namespace
{

FileImage::File MakeFile(const std::string& name, size_t size, uint8_t seed)
{
    FileImage::File f;
    f.name = name;
    f.bytes.resize(size);
    for (size_t i = 0; i < size; i++)
    {
        f.bytes[i] = static_cast<uint8_t>(seed + i * 7);
    }
    return f;
}

std::vector<FileImage::File> SomeFiles()
{
    return { MakeFile("boot.scene", 4217, 1),
             MakeFile("project_data.bin", 178, 2),
             MakeFile("assets/5a7800eb-4d28-4c21-8718-7cbc9940c378", 49805, 3),
             MakeFile("assets/08048540-cd9e-4465-af41-a52190c0f245", 9045, 4),
             MakeFile("assets/asset_table.bin", 0, 5),
             MakeFile("assets/b", 1, 6) };
}

std::vector<uint8_t> Written(const std::vector<FileImage::File>& files)
{
    std::vector<uint8_t> image;
    std::string error;
    EXPECT_TRUE(FileImage::Write(files, image, error)) << error;
    return image;
}

}  // namespace

TEST(FileImage, FindsEveryFileItWasGiven)
{
    const std::vector<FileImage::File> files = SomeFiles();
    const std::vector<uint8_t> image = Written(files);
    FileImage reader;
    ASSERT_TRUE(reader.Open(image.data(), image.size()));
    EXPECT_EQ(reader.GetFileCount(), files.size());
    for (const FileImage::File& f : files)
    {
        const uint8_t* data = nullptr;
        size_t size = 0;
        ASSERT_TRUE(reader.Find(f.name.data(), f.name.size(), data, size)) << f.name;
        ASSERT_EQ(size, f.bytes.size()) << f.name;
        EXPECT_EQ(0, std::memcmp(data, f.bytes.data(), size)) << f.name;
        EXPECT_EQ(0u, static_cast<size_t>(data - image.data()) % FileImage::kDataAlignment) << f.name;
    }
}

TEST(FileImage, MissesWhatIsNotThere)
{
    const std::vector<uint8_t> image = Written(SomeFiles());
    FileImage reader;
    ASSERT_TRUE(reader.Open(image.data(), image.size()));
    const uint8_t* data = nullptr;
    size_t size = 0;
    for (const char* name : { "", "a", "assets", "assets/", "assets/bb", "boot.scen", "boot.scenes", "zzz" })
    {
        EXPECT_FALSE(reader.Find(name, std::strlen(name), data, size)) << name;
    }
}

TEST(FileImage, SizeComesFromTheHeader)
{
    const std::vector<uint8_t> image = Written(SomeFiles());
    EXPECT_EQ(FileImage::SizeFromHeader(image.data(), image.size()), image.size());
    EXPECT_EQ(FileImage::SizeFromHeader(image.data(), FileImage::kHeaderSize - 1), 0u);
    const std::vector<uint8_t> erased(64, 0xFF);  // flash never written
    EXPECT_EQ(FileImage::SizeFromHeader(erased.data(), erased.size()), 0u);
}

TEST(FileImage, AnEmptyImageOpens)
{
    const std::vector<uint8_t> image = Written({});
    FileImage reader;
    ASSERT_TRUE(reader.Open(image.data(), image.size()));
    EXPECT_EQ(reader.GetFileCount(), 0u);
    const uint8_t* data = nullptr;
    size_t size = 0;
    EXPECT_FALSE(reader.Find("boot.scene", 10, data, size));
}

TEST(FileImage, RefusesSharedAndEmptyNames)
{
    std::vector<uint8_t> image;
    std::string error;
    EXPECT_FALSE(FileImage::Write({ MakeFile("a", 1, 0), MakeFile("a", 2, 0) }, image, error));
    EXPECT_FALSE(error.empty());
    error.clear();
    EXPECT_FALSE(FileImage::Write({ MakeFile("", 1, 0) }, image, error));
    EXPECT_FALSE(error.empty());
}

TEST(FileImage, RefusesADamagedImage)
{
    const std::vector<uint8_t> good = Written(SomeFiles());
    FileImage reader;

    // Cut short: the header says more than there is.
    EXPECT_FALSE(reader.Open(good.data(), good.size() - 1));

    // Another version.
    std::vector<uint8_t> bad = good;
    bad[4] = 2;
    EXPECT_FALSE(reader.Open(bad.data(), bad.size()));

    // An entry pointing past the end.
    bad = good;
    bad[FileImage::kHeaderSize + 12] = 0xFF;
    bad[FileImage::kHeaderSize + 13] = 0xFF;
    bad[FileImage::kHeaderSize + 14] = 0xFF;
    EXPECT_FALSE(reader.Open(bad.data(), bad.size()));

    // Names out of order: Find would miss files.
    bad = good;
    const size_t firstName = FileImage::kHeaderSize + 6 * FileImage::kEntrySize;
    bad[firstName] = 'z';
    EXPECT_FALSE(reader.Open(bad.data(), bad.size()));
    EXPECT_FALSE(reader.IsOpen());
}
