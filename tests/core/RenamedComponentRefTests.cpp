// Renaming a component has to carry the references to it, not just the
// component itself.
//
// A scene stores a component's identity three ways. The "type" key is a name,
// and DEKI_FORMER_NAME resolves an old one - that path is covered by
// RenamedComponentTests. The other two store no name at all:
//
//   ObjectRef   -> {"id": <object>, "typeId": <hash of the class name>}
//   PropertyRef -> {"object": ..., "component": "<class name>", "field": ...}
//
// A hash cannot be turned back into the string it came from, so the name
// fallback cannot reach an ObjectRef. Before this was fixed, 0.16.0 moving every
// package component into a namespace changed every one of those hashes: the
// component still loaded, the object still resolved, and the reference silently
// pointed at nothing. Worse, the dead id was written straight back out on save,
// so the file never healed.
//
// These tests pin both directions of the repair.

#include <gtest/gtest.h>

#include <deki/TypeId.h>
#include <deki/reflection/ComponentRegistry.h>
#include <deki/reflection/PropertyRef.h>
#include "reflection/FormerTypeIds.h"
#include "../mocks/MockComponent.h"

using DekiEditor::Tests::MockComponent;

class RenamedComponentRef : public ::testing::Test
{
protected:
    void SetUp() override { MockComponent::Register(); }
    void TearDown() override { MockComponent::Unregister(); }

    // The mock declares DEKI_FORMER_NAME("OldMockComponent") and
    // ("AncientMockComponent"); this is the id a scene saved under the first.
    static uint32_t OldId() { return Deki::HashString("OldMockComponent"); }
    static uint32_t AncientId() { return Deki::HashString("AncientMockComponent"); }
    static uint32_t CurrentId() { return MockComponent::kStaticType; }
};

TEST_F(RenamedComponentRef, AnIdSavedUnderAFormerNameResolvesToTheClassThatHasItNow)
{
    EXPECT_NE(OldId(), CurrentId()) << "the rename must actually change the id";
    EXPECT_EQ(Deki::LegacyComponentTypeId(OldId()), CurrentId());
}

TEST_F(RenamedComponentRef, ARenameChainOfTwoStepsStillResolves)
{
    EXPECT_EQ(Deki::LegacyComponentTypeId(AncientId()), CurrentId());
}

TEST_F(RenamedComponentRef, ACurrentIdIsLeftAlone)
{
    EXPECT_EQ(Deki::LegacyComponentTypeId(CurrentId()), CurrentId());
}

TEST_F(RenamedComponentRef, AnIdNobodyClaimsIsLeftAlone)
{
    // Not translated to something wrong: an unresolvable reference must stay
    // unresolvable rather than silently latch onto an unrelated component.
    const uint32_t stranger = Deki::HashString("NoSuchComponentAnywhere");
    EXPECT_EQ(Deki::LegacyComponentTypeId(stranger), stranger);
}

TEST_F(RenamedComponentRef, ZeroMeansNoReferenceAndStaysZero)
{
    // ObjectRef treats 0 as "any component of the template type", so it must
    // never be mapped onto a real id.
    EXPECT_EQ(Deki::LegacyComponentTypeId(0u), 0u);
}

TEST_F(RenamedComponentRef, TheNameAndIdIndexesAgree)
{
    // Two indexes over the same DEKI_FORMER_NAME list: one keyed by the string,
    // for the "type" key, one by its hash, for the references that store no
    // name. They are built together and must not drift apart, or a scene would
    // resolve its components and lose its references to them.
    const Deki::ComponentRegistry& reg = Deki::ComponentRegistry::Instance();
    for (const char* former : { "OldMockComponent", "AncientMockComponent" })
    {
        const Deki::ComponentMeta* byName = reg.GetMeta(std::string(former));
        const Deki::ComponentMeta* byId = reg.GetMetaByFormerTypeId(Deki::HashString(former));
        ASSERT_NE(byName, nullptr) << former;
        EXPECT_EQ(byName, byId) << former;
    }
}

TEST_F(RenamedComponentRef, AnIdThatNoComponentEverUsedIsNotInTheIndex)
{
    EXPECT_EQ(Deki::ComponentRegistry::Instance().GetMetaByFormerTypeId(Deki::HashString("NoSuchComponentAnywhere")),
              nullptr);
    // Zero is "no component named", never a key.
    EXPECT_EQ(Deki::ComponentRegistry::Instance().GetMetaByFormerTypeId(0u), nullptr);
}

TEST_F(RenamedComponentRef, APropertyRefAuthoredUnderAFormerNameRebindsAndMigrates)
{
    Deki::PropertyRef ref;
    ref.object = "Player";
    ref.component = "OldMockComponent";
    ref.field = "intValue";
    ref.Rehash();

    // Rewritten to the current name, so the next save migrates the file...
    EXPECT_EQ(ref.component, std::string(Deki::TypeName<MockComponent>()));
    // ...and the derived id is the one the live component answers to.
    EXPECT_EQ(ref.componentTypeId, CurrentId());
}

TEST_F(RenamedComponentRef, APropertyRefOnTheCurrentNameIsUnchanged)
{
    Deki::PropertyRef ref;
    ref.component = Deki::TypeName<MockComponent>();
    ref.field = "intValue";
    ref.Rehash();

    EXPECT_EQ(ref.component, std::string(Deki::TypeName<MockComponent>()));
    EXPECT_EQ(ref.componentTypeId, CurrentId());
}

TEST_F(RenamedComponentRef, APropertyRefNamingNothingKnownKeepsWhatWasAuthored)
{
    // An unresolved binding must keep the author's text so the inspector can
    // show what was meant, rather than being blanked or bent onto another class.
    Deki::PropertyRef ref;
    ref.component = "NoSuchComponentAnywhere";
    ref.field = "intValue";
    ref.Rehash();

    EXPECT_EQ(ref.component, std::string("NoSuchComponentAnywhere"));
    EXPECT_EQ(ref.componentTypeId, Deki::HashString("NoSuchComponentAnywhere"));
}

TEST_F(RenamedComponentRef, AnEmptyPropertyRefHashesToZero)
{
    Deki::PropertyRef ref;
    ref.Rehash();
    EXPECT_EQ(ref.componentTypeId, 0u);
    EXPECT_EQ(ref.fieldHash, 0u);
}
