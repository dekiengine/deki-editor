#include <gtest/gtest.h>

#include <memory>
#include <set>

#include <deki/Object.h>
#include <deki/Scene.h>

// Scene::DuplicateObject clones through the scene JSON, which carries ids.
// Every duplicate used to keep its original's id (and each object under it
// its original's), so a scene built by duplicating - Ctrl+D, or the CLI's
// duplicate_object - saved many objects under one id.

namespace
{
void CollectIds(const ::Deki::Object* obj, std::multiset<uint32_t>& ids)
{
    ids.insert(obj->GetId());
    for (const ::Deki::Object* child : obj->GetChildren())
    {
        CollectIds(child, ids);
    }
}
}  // namespace

TEST(SceneDuplicateTest, DuplicateGetsFreshIds)
{
    auto scene = std::make_unique<::Deki::Scene>();
    ::Deki::Object* original = scene->CreateDekiObject("Original");
    // Not CreateDekiObject: that makes it a root of the scene as well.
    auto* child = new ::Deki::Object("Child");
    original->AddChild(child);

    ::Deki::Object* copy = scene->DuplicateObject(original);
    ASSERT_NE(copy, nullptr);
    ASSERT_EQ(copy->GetChildren().size(), 1u);

    EXPECT_NE(copy->GetId(), original->GetId());
    EXPECT_NE(copy->GetChildren()[0]->GetId(), child->GetId());

    std::multiset<uint32_t> ids;
    CollectIds(original, ids);
    CollectIds(copy, ids);
    EXPECT_EQ(std::set<uint32_t>(ids.begin(), ids.end()).size(), ids.size()) << "an id is shared";

    delete copy;  // detached: DuplicateObject hands back an object in no scene
}

TEST(SceneDuplicateTest, RepeatedDuplicatesAreAllDistinct)
{
    auto scene = std::make_unique<::Deki::Scene>();
    ::Deki::Object* original = scene->CreateDekiObject("Tile");

    std::set<uint32_t> ids{ original->GetId() };
    std::vector<std::unique_ptr<::Deki::Object>> copies;
    for (int i = 0; i < 8; ++i)
    {
        copies.emplace_back(scene->DuplicateObject(original));
        ASSERT_NE(copies.back(), nullptr);
        EXPECT_TRUE(ids.insert(copies.back()->GetId()).second) << "copy " << i << " reused an id";
    }
}
