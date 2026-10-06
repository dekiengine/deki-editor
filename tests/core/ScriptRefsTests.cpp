// A --script step can use what an earlier step returned ("as" + "${name.key}").

#include <gtest/gtest.h>

#include "core/ScriptRefs.h"

#include <map>
#include <string>

using DekiEditor::ResolveScriptRefs;
using json = nlohmann::json;

namespace
{

const std::map<std::string, json> kSaved = {
    { "panel", json{ { "id", 42 }, { "value", { { "guid", "abc" } } } } },
};

}  // namespace

TEST(ScriptRefs, AWholeReferenceKeepsItsType)
{
    json args = { { "object_id", "${panel.id}" } };
    std::string error;
    ASSERT_TRUE(ResolveScriptRefs(args, kSaved, error)) << error;
    EXPECT_EQ(args["object_id"], 42);
}

TEST(ScriptRefs, NestedKeysAndReferencesInsideText)
{
    json args = { { "value", "${panel.value.guid}" }, { "name", "Item ${panel.id}" }, { "list", { "${panel.id}" } } };
    std::string error;
    ASSERT_TRUE(ResolveScriptRefs(args, kSaved, error)) << error;
    EXPECT_EQ(args["value"], "abc");
    EXPECT_EQ(args["name"], "Item 42");
    EXPECT_EQ(args["list"][0], 42);
}

TEST(ScriptRefs, AnUnknownNameOrKeyIsAnError)
{
    std::string error;
    json unknownName = { { "id", "${nope.id}" } };
    EXPECT_FALSE(ResolveScriptRefs(unknownName, kSaved, error));
    json unknownKey = { { "id", "${panel.missing}" } };
    EXPECT_FALSE(ResolveScriptRefs(unknownKey, kSaved, error));
}

TEST(ScriptRefs, TextWithoutReferencesIsUnchanged)
{
    json args = { { "text", "costs $5 {not a ref}" }, { "n", 3 } };
    std::string error;
    ASSERT_TRUE(ResolveScriptRefs(args, kSaved, error));
    EXPECT_EQ(args["text"], "costs $5 {not a ref}");
    EXPECT_EQ(args["n"], 3);
}
