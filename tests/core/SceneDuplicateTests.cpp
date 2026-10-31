#include <gtest/gtest.h>

#include <memory>
#include <set>

#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki-editor/CommandHistory.h>
#include "core/commands/ObjectCommands.h"

// Scene::DuplicateObject clones through the scene JSON, which carries ids.
// The duplicate and every object under it must get new ids, or a scene built
// by duplicating (Ctrl+D, or the CLI's duplicate_object) saves many objects
// under one id.

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

// copy_from_scene: the original is in a scene read on the side, gone before
// any undo or redo. The copy keeps its name and lands under the given parent.
TEST(SceneDuplicateTest, CopyFromAnotherSceneOutlivesIt)
{
    using DekiEditor::CommandHistory;
    CommandHistory::Instance().Clear();
    auto target = std::make_unique<::Deki::Scene>();
    ::Deki::Object* root = target->CreateDekiObject("Root");

    ::Deki::Object* copy = nullptr;
    {
        auto other = std::make_unique<::Deki::Scene>();
        ::Deki::Object* otherRoot = other->CreateDekiObject("Root");
        auto* buttons = new ::Deki::Object("Buttons");
        otherRoot->AddChild(buttons);
        buttons->AddChild(new ::Deki::Object("Primary"));

        auto command = std::make_unique<DekiEditor::DuplicateObjectCommand>(target.get(), buttons, root);
        command->CopyFromOtherScene();
        DekiEditor::DuplicateObjectCommand* raw = command.get();
        CommandHistory::Instance().Execute(std::move(command));
        copy = raw->GetDuplicatedObject();
    }
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->GetName(), "Buttons") << "a copy from another scene is not numbered";
    EXPECT_EQ(copy->GetParent(), root);
    ASSERT_EQ(copy->GetChildren().size(), 1u);

    // The other scene is gone: undo and redo must not reach for it.
    CommandHistory::Instance().Undo();
    EXPECT_TRUE(root->GetChildren().empty());
    CommandHistory::Instance().Redo();
    ASSERT_EQ(root->GetChildren().size(), 1u);
    EXPECT_EQ(root->GetChildren()[0], copy);

    CommandHistory::Instance().Clear();
}
