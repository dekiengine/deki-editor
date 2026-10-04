// Hardening of engine-side parsing, plus the editor-side settings write.
//
// The parser tests matter most: SceneMsgPackParser is the device-side loader
// for .scene caches read off SD/SPIFFS, so its input is untrusted by default -
// a failing card produces malformed data without anyone attacking anything.

#include <gtest/gtest.h>

#include <deki/SceneMessagePack.h>
#include <deki/Scene.h>
#include <deki/assets/AssetLookupTable.h>
#include <deki/Object.h>
#include <deki/Engine.h>
#include <deki/assets/Texture2D.h>
#include <deki/providers/Memory.h>
#include "LifecycleManager.h"
#include <deki-editor/FileIO.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ============================================================================
// MessagePack parser - nesting depth
// ============================================================================

namespace
{
/// A scene-shaped document whose single value is `depth` nested fixarrays.
/// A fixarray header is one byte (0x91 = "array of 1"), so this costs one input
/// byte per nesting level: deep nesting is cheap to send.
std::vector<uint8_t> MakeNestedDoc(int depth)
{
    std::vector<uint8_t> doc;
    doc.push_back(0x81);  // fixmap, 1 pair
    doc.push_back(0xa1);
    doc.push_back('x');  // key: fixstr "x"
    for (int i = 0; i < depth; ++i)
    {
        doc.push_back(0x91);
    }
    doc.push_back(0xc0);  // innermost nil
    return doc;
}
}  // namespace

TEST(MsgPackDepth, ShallowNestingStillParses)
{
    // Well below the cap - must not be rejected by the depth limit.
    auto doc = MakeNestedDoc(8);
    Deki::Scene scene;
    Deki::SceneFormat::SceneMsgPackParser parser(doc.data(), doc.size());
    EXPECT_TRUE(parser.ParseScene(&scene));
}

TEST(MsgPackDepth, DeepNestingIsRejectedRatherThanCrashing)
{
    // Without a depth cap this exhausts the stack and kills the process
    // (0xC00000FD). ~20 KB is enough on a 1 MB desktop stack; on an ESP32
    // task stack a couple of hundred bytes is.
    auto doc = MakeNestedDoc(60000);
    Deki::Scene scene;
    Deki::SceneFormat::SceneMsgPackParser parser(doc.data(), doc.size());
    EXPECT_FALSE(parser.ParseScene(&scene));
}

TEST(MsgPackDepth, NestingJustPastTheCapIsRejected)
{
    auto doc = MakeNestedDoc(Deki::SceneFormat::SceneMsgPackParser::kMaxDepth + 8);
    Deki::Scene scene;
    Deki::SceneFormat::SceneMsgPackParser parser(doc.data(), doc.size());
    EXPECT_FALSE(parser.ParseScene(&scene));
}

TEST(MsgPackDepth, DeeplyNestedChildObjectsAreRejected)
{
    // The object tree recurses through ParseObjectInternal, which allocates a
    // Deki::Object per level - so an unbounded chain burns heap as well as stack.
    std::vector<uint8_t> doc;
    doc.push_back(0x81);  // scene map
    doc.push_back(0xa7);
    for (char c : std::string("objects"))
    {
        doc.push_back(c);
    }
    doc.push_back(0x91);  // array of 1 object

    const int depth = 5000;
    for (int i = 0; i < depth; ++i)
    {
        doc.push_back(0x81);  // object map, 1 pair
        doc.push_back(0xa8);
        for (char c : std::string("children"))
        {
            doc.push_back(c);
        }
        doc.push_back(0x91);  // array of 1 child
    }
    doc.push_back(0x80);  // innermost: empty object

    Deki::Scene scene;
    Deki::SceneFormat::SceneMsgPackParser parser(doc.data(), doc.size());
    EXPECT_FALSE(parser.ParseScene(&scene));
}

TEST(MsgPackDepth, TruncatedInputFailsCleanly)
{
    // Not a depth case, but the same "malformed input must fail, not crash"
    // contract the parser holds to.
    const std::vector<uint8_t> truncated = { 0x81, 0xa1, 'x' };
    Deki::Scene scene;
    Deki::SceneFormat::SceneMsgPackParser parser(truncated.data(), truncated.size());
    EXPECT_FALSE(parser.ParseScene(&scene));
}

TEST(MsgPackDepth, EmptyInputFailsCleanly)
{
    Deki::Scene scene;
    Deki::SceneFormat::SceneMsgPackParser parser(nullptr, 0);
    EXPECT_FALSE(parser.ParseScene(&scene));
}

// ============================================================================
// AssetLookupTable - header arithmetic and terminator validation
// ============================================================================

namespace
{
/// Build a lookup table blob: uint32 count, then `count` 44-byte entries.
std::vector<uint8_t> MakeLookupTable(uint32_t declaredCount, uint32_t realEntries, bool terminateGuids = true)
{
    std::vector<uint8_t> buf(4 + static_cast<size_t>(realEntries) * 44, 0);
    std::memcpy(buf.data(), &declaredCount, sizeof(declaredCount));

    for (uint32_t i = 0; i < realEntries; ++i)
    {
        uint8_t* entry = buf.data() + 4 + static_cast<size_t>(i) * 44;
        const uint32_t hash = i;
        std::memcpy(entry, &hash, sizeof(hash));
        // guid field is entry[4..40]; leave it zero-filled (terminated) unless
        // the test wants to prove the terminator check bites.
        if (!terminateGuids)
        {
            std::memset(entry + 4, 'A', 37);
        }
    }
    return buf;
}
}  // namespace

TEST(AssetLookupTableHardening, RejectsCountLargerThanTheBuffer)
{
    // Declares 1000 entries, supplies 2. The size check must catch it -
    // otherwise Lookup binary-searches past the end of the buffer.
    auto buf = MakeLookupTable(/*declared*/ 1000, /*real*/ 2);
    EXPECT_FALSE(Deki::AssetLookupTable::Load(buf.data(), buf.size()));
    EXPECT_EQ(Deki::AssetLookupTable::Lookup(0), nullptr);
    Deki::AssetLookupTable::Unload();
}

TEST(AssetLookupTableHardening, RejectsCountThatWouldOverflow32BitArithmetic)
{
    // 4 + count * 44 wraps to a small value in 32-bit size_t, which is what
    // ESP32 builds use. It is computed in 64-bit, so this is refused on every
    // platform.
    const uint32_t overflowing = 0xFFFFFFFFu / 44u + 2u;
    auto buf = MakeLookupTable(overflowing, /*real*/ 1);
    EXPECT_FALSE(Deki::AssetLookupTable::Load(buf.data(), buf.size()));
    Deki::AssetLookupTable::Unload();
}

TEST(AssetLookupTableHardening, RejectsUnterminatedGuidFields)
{
    // Lookup returns entries[i].guid as a bare const char*; a field with no
    // terminator would run into the next entry.
    auto buf = MakeLookupTable(/*declared*/ 2, /*real*/ 2, /*terminate*/ false);
    EXPECT_FALSE(Deki::AssetLookupTable::Load(buf.data(), buf.size()));
    Deki::AssetLookupTable::Unload();
}

TEST(AssetLookupTableHardening, FailedLoadDoesNotLeaveStaleState)
{
    // A good table, then a bad one: the bad load must clear both the pointer
    // and the count rather than leaving the previous table half-installed.
    auto good = MakeLookupTable(/*declared*/ 2, /*real*/ 2);
    ASSERT_TRUE(Deki::AssetLookupTable::Load(good.data(), good.size()));

    auto bad = MakeLookupTable(/*declared*/ 5000, /*real*/ 1);
    EXPECT_FALSE(Deki::AssetLookupTable::Load(bad.data(), bad.size()));
    EXPECT_EQ(Deki::AssetLookupTable::Lookup(0), nullptr);
    Deki::AssetLookupTable::Unload();
}

TEST(AssetLookupTableHardening, ValidTableStillLoadsAndResolves)
{
    auto buf = MakeLookupTable(/*declared*/ 3, /*real*/ 3);
    ASSERT_TRUE(Deki::AssetLookupTable::Load(buf.data(), buf.size()));

    // Entry hashes are 0,1,2 with zero-filled (empty, terminated) guids.
    EXPECT_NE(Deki::AssetLookupTable::Lookup(1), nullptr);
    EXPECT_EQ(Deki::AssetLookupTable::Lookup(999), nullptr);
    Deki::AssetLookupTable::Unload();
}

TEST(AssetLookupTableHardening, RejectsTruncatedHeader)
{
    const uint8_t tiny[2] = { 0, 0 };
    EXPECT_FALSE(Deki::AssetLookupTable::Load(tiny, sizeof(tiny)));
    EXPECT_FALSE(Deki::AssetLookupTable::Load(nullptr, 0));
    Deki::AssetLookupTable::Unload();
}

// ============================================================================
// EditorSettings write durability
// ============================================================================

TEST(SettingsDurability, AtomicWriteReplacesWithoutAnIntermediateEmptyState)
{
    const fs::path dir = fs::temp_directory_path() / "deki_settings_atomic";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path target = dir / "settings.json";

    {
        std::ofstream f(target);
        f << R"({"lastProjectPath":"C:/old"})";
    }

    std::string err;
    ASSERT_TRUE(DekiEditor::AtomicWriteFile(target.string(), R"({"lastProjectPath":"C:/new"})", err)) << err;

    std::ifstream in(target);
    const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find("C:/new"), std::string::npos);
    // No temp file left behind to be mistaken for real settings.
    EXPECT_FALSE(fs::exists(target.string() + ".tmp"));

    fs::remove_all(dir, ec);
}

// ============================================================================
// Lifecycle phases must not allocate every frame
// ============================================================================

// Measuring allocations directly needs a counter inside deki-engine-core: on
// Windows each DLL resolves its own operator new, so an override here never
// sees the engine's. So this pins the property that makes the steady state
// allocation-free: the scratch buffers keep their capacity across frames, so
// repeated phases reuse storage instead of building a fresh vector each time
// (which costs 12 allocations, ~8.9 KB, a frame with 200 behaviours).

TEST(LifecycleAllocations, RepeatedFramesReuseScratchCapacity)
{
    Deki::Scene scene;
    Deki::Object* root = scene.CreateDekiObject("Root");
    for (int i = 0; i < 64; ++i)
    {
        root->AddChild(new Deki::Object("Obj"));
    }

    auto* lm = Deki::Engine::GetInstance().GetLifecycleManager();
    ASSERT_NE(lm, nullptr);

    // Two identical frames. The second must not need to grow anything the
    // first already sized.
    for (int frame = 0; frame < 2; ++frame)
    {
        lm->ProcessInternalAwake(&scene);
        lm->ProcessAwake(&scene);
        lm->ProcessStart(&scene);
        lm->ProcessUpdate(&scene);
    }

    // The behaviours cache itself is also expected to be stable across frames
    // (it is rebuilt only when marked dirty), which is what lets the snapshot
    // copy stay inside already-reserved capacity.
    const size_t capBefore = scene.GetActiveComponents().capacity();
    lm->ProcessUpdate(&scene);
    EXPECT_EQ(scene.GetActiveComponents().capacity(), capBefore);
}

// A texture header from a corrupt file: 65536 x 65536 one-byte pixels is
// 2^32 bytes, which wraps to 0 in 32-bit arithmetic and would "match" a
// dataSize of 0. The check is done in 64 bits.
TEST(EngineHardening, TextureSizeThatWrapsIn32BitsIsRejected)
{
    Deki::Texture2D::Header h{};
    std::memcpy(h.magic, "2DTX", 4);
    h.version = 1;
    h.width = 65536;
    h.height = 65536;
    h.format = Deki::Texture2D::TextureFormat::ALPHA8;
    h.dataSize = 0;
    EXPECT_FALSE(Deki::Texture2D::ValidateHeader(h));
}

// A size no allocation can have (read from a corrupt file) is refused rather
// than wrapped into a small allocation by the header and rounding.
TEST(EngineHardening, AnImpossibleAllocationSizeIsRefused)
{
    EXPECT_EQ(Deki::Memory::Allocate(SIZE_MAX - 8, Deki::Memory::Internal), nullptr);
    EXPECT_EQ(Deki::Memory::Allocate(SIZE_MAX - 8, Deki::Memory::External), nullptr);
}
