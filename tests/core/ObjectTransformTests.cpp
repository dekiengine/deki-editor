/**
 * @file ObjectTransformTests.cpp
 * @brief World-transform propagation (the rotation-0 fast path must equal the
 *        trig path), the one-shot GetWorldTransform read, the iterative
 *        active-hierarchy walk, and the interface-adapter version counter.
 */

#include <gtest/gtest.h>
#include <cmath>

#include <deki/Object.h>
#include <deki/ComponentInterfaceAdapters.h>

TEST(ObjectTransform, ChildOfUnrotatedParentUsesPlainOffsets)
{
    auto* parent = new Deki::Object("parent");
    parent->SetX(1.5f);
    parent->SetY(-2.0f);
    parent->SetScale(2.0f, 3.0f);
    auto* child = new Deki::Object("child");
    child->SetX(0.25f);
    child->SetY(-0.5f);
    parent->AddChild(child);

    // Exactly what the trig path yields for rotation 0 (cos 0 = 1, sin 0 = 0).
    EXPECT_EQ(child->GetWorldX(), 1.5f + 0.25f * 2.0f);
    EXPECT_EQ(child->GetWorldY(), -2.0f + (-0.5f * 3.0f));
    EXPECT_EQ(child->GetWorldScaleX(), 2.0f);
    EXPECT_EQ(child->GetWorldScaleY(), 3.0f);
    EXPECT_EQ(child->GetWorldRotation(), 0.0f);
    delete parent;  // deletes the child
}

TEST(ObjectTransform, RotatedParentStillRotatesChildren)
{
    auto* parent = new Deki::Object("parent");
    parent->SetRotation(1.0f);
    auto* child = new Deki::Object("child");
    child->SetX(1.0f);
    parent->AddChild(child);

    EXPECT_NEAR(child->GetWorldX(), std::cos(1.0f), 1e-6f);
    EXPECT_NEAR(child->GetWorldY(), std::sin(1.0f), 1e-6f);
    EXPECT_EQ(child->GetWorldRotation(), 1.0f);
    delete parent;
}

TEST(ObjectTransform, GetWorldTransformMatchesTheFiveGetters)
{
    auto* parent = new Deki::Object("parent");
    parent->SetX(3.0f);
    parent->SetRotation(0.3f);
    parent->SetScale(0.5f, 4.0f);
    auto* child = new Deki::Object("child");
    child->SetX(1.0f);
    child->SetY(2.0f);
    child->SetRotation(0.2f);
    child->SetScale(2.0f, 0.25f);
    parent->AddChild(child);

    const Deki::WorldTransform t = child->GetWorldTransform();
    EXPECT_EQ(t.x, child->GetWorldX());
    EXPECT_EQ(t.y, child->GetWorldY());
    EXPECT_EQ(t.rotation, child->GetWorldRotation());
    EXPECT_EQ(t.scaleX, child->GetWorldScaleX());
    EXPECT_EQ(t.scaleY, child->GetWorldScaleY());
    delete parent;
}

TEST(ObjectTransform, MovingTheParentInvalidatesTheChild)
{
    auto* parent = new Deki::Object("parent");
    auto* child = new Deki::Object("child");
    child->SetX(1.0f);
    parent->AddChild(child);
    EXPECT_EQ(child->GetWorldX(), 1.0f);
    parent->SetX(10.0f);
    EXPECT_EQ(child->GetWorldX(), 11.0f);
    delete parent;
}

TEST(ObjectActive, InactiveAncestorMakesTheWholeSubtreeInactive)
{
    auto* root = new Deki::Object("root");
    auto* mid = new Deki::Object("mid");
    auto* leaf = new Deki::Object("leaf");
    root->AddChild(mid);
    mid->AddChild(leaf);
    EXPECT_TRUE(leaf->IsActiveInHierarchy());
    mid->SetActive(false);
    EXPECT_TRUE(leaf->IsActive());
    EXPECT_FALSE(leaf->IsActiveInHierarchy());
    EXPECT_TRUE(root->IsActiveInHierarchy());
    delete root;
}

TEST(ComponentInterfaceAdaptersTests, VersionIncrementsOnRegister)
{
    const uint32_t before = Deki::ComponentInterfaceAdapters::Version();
    Deki::ComponentInterfaceAdapters::Register(0x54455354u /* "TEST" */, 0x12345678u,
                                               [](Deki::Component* c) -> void* { return c; });
    EXPECT_GT(Deki::ComponentInterfaceAdapters::Version(), before);
    const uint32_t after = Deki::ComponentInterfaceAdapters::Version();
    Deki::ComponentInterfaceAdapters::Register(0x54455354u, 0x12345678u, nullptr);  // ignored
    EXPECT_EQ(Deki::ComponentInterfaceAdapters::Version(), after);
}

TEST(ComponentInterfaceAdaptersTests, FindReturnsTheAdapterQueryWouldUse)
{
    static auto s_Adapter = [](Deki::Component* c) -> void* { return c; };
    const uint32_t iface = 0x46494e44u;  // "FIND"
    const Deki::ComponentType baseType = 0x0000aa01u, derivedType = 0x0000aa02u, otherType = 0x0000aa03u;
    Deki::ComponentInterfaceAdapters::Register(iface, baseType, s_Adapter);

    EXPECT_EQ(Deki::ComponentInterfaceAdapters::Find(iface, baseType, 0), s_Adapter);
    EXPECT_EQ(Deki::ComponentInterfaceAdapters::Find(iface, derivedType, baseType), s_Adapter);  // via the base
    EXPECT_EQ(Deki::ComponentInterfaceAdapters::Find(iface, otherType, 0), nullptr);
    EXPECT_EQ(Deki::ComponentInterfaceAdapters::Find(iface + 1, baseType, 0), nullptr);
}
