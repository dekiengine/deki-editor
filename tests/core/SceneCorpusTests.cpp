// The rename gate.
//
// Reflected member names ARE the scene-file format. reflection_codegen emits
// each C++ identifier as a Deki::HashString key with no opt-in gate, so renaming
// a reflected member silently changes the on-disk key: the parser's switch
// falls through and the property reverts to its default. No error, no log line.
// The value is not destroyed -- unknown keys are re-emitted on save -- but the
// behaviour is gone, and it is gone in whichever scene happened to use it.
//
// Scene::CollectUnknownProperties reports exactly that condition. This test
// loads every scene in a real project and asserts it comes back empty, so a
// missed rename names the component and the key instead of shipping quietly.
//
// Point DEKI_SCENE_CORPUS at a project's assets directory to run it. Without
// that it skips: the editor repo has no scenes of its own, and a gate that
// silently passes on an empty corpus would be worse than no gate.

#include <gtest/gtest.h>

#include <deki/Scene.h>
#include <deki/Object.h>
#include "MissingComponent.h"

#include <cstdlib>
#include <functional>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

std::string CorpusDir()
{
    if (const char* env = std::getenv("DEKI_SCENE_CORPUS"))
    {
        return env;
    }
    return {};
}

TEST(SceneCorpus, EveryPropertyKeyIsRecognised)
{
    const std::string dir = CorpusDir();
    if (dir.empty())
    {
        GTEST_SKIP() << "set DEKI_SCENE_CORPUS to a project's assets dir to run the rename gate";
    }

    std::error_code ec;
    ASSERT_TRUE(fs::exists(dir, ec)) << "DEKI_SCENE_CORPUS does not exist: " << dir;

    Deki::Scene::ClearUnknownProperties();

    int loaded = 0;
    int totalComponents = 0;
    int missingComponents = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec))
    {
        if (ec)
        {
            break;
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".scene")
        {
            continue;
        }

        Deki::Scene scene;
        // A scene that fails to load is a different problem from a scene with
        // an unrecognised key; report it, but keep going so one bad file does
        // not hide the rename damage in the rest.
        if (!scene.LoadFromFile(entry.path().string()))
        {
            ADD_FAILURE() << "could not load " << entry.path().string();
            continue;
        }
        ++loaded;

        // Walk the whole tree, not just roots: nested objects carry components too.
        std::function<void(const Deki::Object*)> visit = [&](const Deki::Object* obj)
        {
            if (!obj)
            {
                return;
            }
            for (const Deki::Component* c : obj->GetComponents())
            {
                ++totalComponents;
                if (c && c->GetType() == Deki::MissingComponent::kStaticType)
                {
                    ++missingComponents;
                }
            }
            for (const Deki::Object* child : obj->GetChildren())
            {
                visit(child);
            }
        };
        for (const Deki::Object* root : scene.GetObjects())
        {
            visit(root);
        }
    }

    ASSERT_GT(loaded, 0) << "no .scene files under " << dir << " -- the gate would pass vacuously";

    // The check only sees keys a component's metadata does NOT recognise, so a
    // component with no metadata at all contributes nothing. If the packages
    // owning these components are not loaded, every component deserialises as
    // MissingComponent and the gate reports success having examined nothing.
    // That false green is more dangerous than no gate, so refuse to give it.
    ASSERT_GT(totalComponents, 0) << "scenes loaded but contained no components";
    if (missingComponents == totalComponents)
    {
        GTEST_SKIP() << "no component types are registered in this binary (" << totalComponents
                     << " components, all unresolved), so unknown "
                     << "property keys cannot be detected. Run this where the project's "
                     << "packages are loaded.";
    }
    EXPECT_EQ(missingComponents, 0) << missingComponents << " of " << totalComponents << " components did not resolve; "
                                    << "the gate can only vouch for the ones that did";

    const auto unknown = Deki::Scene::CollectUnknownProperties();
    if (!unknown.empty())
    {
        std::string detail;
        for (const auto& [component, key] : unknown)
        {
            detail += "\n  " + component + "." + key;
        }
        FAIL() << "loaded " << loaded << " scene(s); " << unknown.size()
               << " property key(s) are not recognised by any component's current metadata."
               << "\nEach one is a value that will silently revert to its default."
               << "\nIf this appeared after a rename, the scene files still carry the old key:" << detail;
    }

    SUCCEED() << "loaded " << loaded << " scene(s), " << totalComponents
              << " components, every property key recognised";
}

}  // namespace
