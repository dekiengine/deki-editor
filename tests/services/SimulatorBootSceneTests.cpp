// The desktop simulator's boot scene, which every new project is created with.
//
// It names its two setup components twice: once as a "type" string, and once as
// a numeric typeId inside the PlatformSetupComponent's setupComponents list. The
// number is the hash of the name, but it is a literal in a string, so nothing
// makes the two agree — and in 0.16.0 they did not. Moving the SDL3 components
// into the DekiSdl3 namespace changed the hashes and left the template writing
// the old ones, so a brand new project's display and input setup resolved to
// nothing and the window never opened.
//
// This checks the scene against itself: every typeId must be the hash of the
// type name written on the component it points at. That holds through any future
// rename, without the test needing the package types, which are not linked here.

#include <gtest/gtest.h>

#include <deki/TypeId.h>
#include "services/SimulatorBootScene.h"

#include <nlohmann/json.hpp>

#include <string>

using nlohmann::json;

namespace
{
// The single component on an object in this scene.
std::string SoleComponentType(const json& object)
{
    const auto& components = object.at("components");
    EXPECT_EQ(components.size(), 1u) << "the boot scene's setup objects hold one component each";
    return components.at(0).value("type", std::string());
}

const json& ChildWithId(const json& parent, uint32_t id)
{
    for (const auto& child : parent.at("children"))
        if (child.value("id", 0u) == id)
            return child;
    ADD_FAILURE() << "no child object with id " << id;
    return parent;
}
}  // namespace

TEST(SimulatorBootScene, ItParses)
{
    EXPECT_NO_THROW(json::parse(DekiEditor::SimulatorBootSceneJson()));
}

TEST(SimulatorBootScene, EveryTypeIdIsTheHashOfTheNameItPointsAt)
{
    const json scene = json::parse(DekiEditor::SimulatorBootSceneJson());
    const json& setup = scene.at("objects").at(0);

    const json& refs = setup.at("components").at(0).at("properties").at("setupComponents");
    ASSERT_FALSE(refs.empty()) << "the simulator boots nothing at all";

    for (const auto& ref : refs)
    {
        const uint32_t id = ref.at("id").get<uint32_t>();
        const uint32_t typeId = ref.at("typeId").get<uint32_t>();
        const std::string type = SoleComponentType(ChildWithId(setup, id));

        ASSERT_FALSE(type.empty()) << "object " << id << " has a component with no type";
        EXPECT_EQ(typeId, Deki::HashString(type.c_str()))
            << "setupComponents entry for object " << id << " carries " << typeId
            << ", but '" << type << "' hashes to " << Deki::HashString(type.c_str())
            << ". An ObjectRef whose typeId matches no live component resolves to null, so "
               "this setup step would silently never run. Update the typeId to match the name.";
    }
}

TEST(SimulatorBootScene, TheSetupComponentTypesAreQualified)
{
    // Bare names still load, through DEKI_FORMER_NAME, but a template should
    // write what the current release writes, not what an older one did.
    const json scene = json::parse(DekiEditor::SimulatorBootSceneJson());
    const json& setup = scene.at("objects").at(0);

    for (const auto& child : setup.at("children"))
    {
        const std::string type = SoleComponentType(child);
        EXPECT_NE(type.find("::"), std::string::npos)
            << "'" << type << "' is a bare class name; scene files store the qualified one";
    }

    EXPECT_NE(setup.at("components").at(0).value("type", std::string()).find("::"),
              std::string::npos);
}
