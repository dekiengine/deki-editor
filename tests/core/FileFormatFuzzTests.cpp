// Corrupt files must fail, never crash. A scene or settings file comes from
// disk, a card or a download, so these readers see whatever is there.
//
// Each test starts from a valid file and damages it thousands of ways: bits
// flipped, bytes overwritten with sizes that overflow, ranges cut out, ends
// truncated. Whether a damaged file loads is up to the reader; what matters is
// that it returns. Seeded, so a failure repeats. DEKI_FUZZ_ITERATIONS asks for
// more: CI's sanitizer job runs far more of them.

#include <gtest/gtest.h>

#include "../mocks/MockComponent.h"
#include "ObjectInternal.h"

#include <deki/LogSystem.h>
#include <deki/Object.h>
#include <deki/ProjectSettings.h>
#include <deki/Scene.h>
#include <deki/SceneMessagePack.h>

#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace DekiEditor::Tests;

namespace
{

int Iterations(int fallback)
{
    const char* text = std::getenv("DEKI_FUZZ_ITERATIONS");
    const int asked = text ? std::atoi(text) : 0;
    return asked > 0 ? asked : fallback;
}

// The values that break size arithmetic.
const uint8_t kNasty[] = { 0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0xC1, 0xCA, 0xCF, 0xDB, 0xDD, 0xDF };

std::vector<uint8_t> Damage(std::vector<uint8_t> bytes, std::mt19937& rng)
{
    const int edits = 1 + static_cast<int>(rng() % 8);
    for (int e = 0; e < edits && !bytes.empty(); ++e)
    {
        const size_t at = rng() % bytes.size();
        switch (rng() % 6)
        {
            case 0: bytes[at] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
            case 1: bytes[at] = static_cast<uint8_t>(rng()); break;
            case 2: bytes[at] = kNasty[rng() % sizeof(kNasty)]; break;
            case 3:
                // A 32-bit size, big-endian as MessagePack's, little as ours.
                for (size_t i = at; i < bytes.size() && i < at + 4; ++i)
                {
                    bytes[i] = 0xFF;
                }
                break;
            case 4: bytes.resize(at); break;
            default:
            {
                const size_t count = std::min<size_t>(rng() % 32, bytes.size() - at);
                bytes.erase(bytes.begin() + at, bytes.begin() + at + count);
                break;
            }
        }
    }
    return bytes;
}

// The damaged files make the readers log every problem they find, which is
// right, and would be millions of lines here.
struct QuietLogs
{
    QuietLogs()
    {
        Deki::LogSystem::SetLogCallback([](Deki::LogLevel, const std::string&, const char*, int) {});
    }
    ~QuietLogs() { Deki::LogSystem::SetLogCallback(nullptr); }
};

std::vector<uint8_t> ValidMsgPackScene()
{
    MockSmallEnumComponent::Register();
    Deki::Scene scene;
    for (int i = 0; i < 4; ++i)
    {
        auto* object = new Deki::Object();
        object->SetName("Object" + std::to_string(i));
        auto* component = new MockSmallEnumComponent();
        component->width = static_cast<float>(i) * 1.5f;
        Deki::Internal::AttachComponent(*object, component);
        scene.AddObject(object);
    }
    Deki::SceneFormat::SceneMsgPackWriter writer;
    EXPECT_TRUE(writer.WriteScene(&scene));
    return std::vector<uint8_t>(writer.GetData(), writer.GetData() + writer.GetSize());
}

}  // namespace

TEST(FileFormatFuzz, MessagePackSceneNeverCrashes)
{
    const std::vector<uint8_t> valid = ValidMsgPackScene();
    ASSERT_FALSE(valid.empty());
    QuietLogs quiet;
    std::mt19937 rng(0xDEC1u);
    for (int i = 0; i < Iterations(4000); ++i)
    {
        const std::vector<uint8_t> damaged = Damage(valid, rng);
        Deki::SceneFormat::SceneMsgPackParser parser(damaged.data(), damaged.size());
        Deki::Scene scene;
        parser.ParseScene(&scene);
    }
}

TEST(FileFormatFuzz, EveryTruncationOfAMessagePackSceneFails)
{
    const std::vector<uint8_t> valid = ValidMsgPackScene();
    QuietLogs quiet;
    for (size_t length = 0; length < valid.size(); ++length)
    {
        Deki::SceneFormat::SceneMsgPackParser parser(valid.data(), length);
        Deki::Scene scene;
        EXPECT_FALSE(parser.ParseScene(&scene)) << "a scene cut at byte " << length << " loaded";
    }
}

TEST(FileFormatFuzz, JsonSceneNeverCrashes)
{
    const std::string text = R"({"formatVersion":1,"objects":[{"active":true,"children":[{"active":true,"children":[],)"
                             R"("components":[],"id":2,"name":"Child","transform":{"rotation":0.0,"scaleX":1.0,"scaleY":1.0,)"
                             R"("x":1.5,"y":2.5}}],"components":[{"properties":{"width":3.0},"type":"Nope::Missing"}],)"
                             R"("id":1,"name":"Root","transform":{"rotation":0.0,"scaleX":1.0,"scaleY":1.0,"x":0.0,"y":0.0}}]})";
    const std::vector<uint8_t> valid(text.begin(), text.end());
    QuietLogs quiet;
    {
        Deki::Scene scene;
        EXPECT_TRUE(scene.LoadFromJsonString(text)) << "the starting point must be valid";
    }
    std::mt19937 rng(0xDEC2u);
    for (int i = 0; i < Iterations(4000); ++i)
    {
        const std::vector<uint8_t> damaged = Damage(valid, rng);
        Deki::Scene scene;
        scene.LoadFromJsonString(std::string(damaged.begin(), damaged.end()));
    }
}

TEST(FileFormatFuzz, ProjectSettingsNeverCrash)
{
    const char* passes[] = { "Pass A", "Pass B" };
    const std::vector<uint8_t> valid = Deki::ProjectSettings::Write(Deki::Storage::Internal, 60, "scenes/main.scene",
                                                                    "Renderer", passes, 2, 5.5f);
    ASSERT_FALSE(valid.empty());
    QuietLogs quiet;
    EXPECT_TRUE(Deki::ProjectSettings::Load(valid.data(), valid.size())) << "the starting point must be valid";
    std::mt19937 rng(0xDEC3u);
    for (int i = 0; i < Iterations(8000); ++i)
    {
        const std::vector<uint8_t> damaged = Damage(valid, rng);
        Deki::ProjectSettings::Load(damaged.data(), damaged.size());
    }
    Deki::ProjectSettings::Load(valid.data(), valid.size());
}
