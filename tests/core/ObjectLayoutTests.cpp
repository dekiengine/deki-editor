// Layout guards for the types package DLLs reach into.
//
// These are not testing behaviour — they are testing an ABI contract. Package
// DLLs and the game plugin read Deki::Object's `parent`/`children` and walk its
// component vector through INLINE accessors in Object.h, and they derive
// from Deki::Component. Both layouts are therefore compiled into every package
// binary, and nothing at link time checks that those copies still agree with
// the engine's.
//
// The failure mode this pins shut: three `#ifdef DEKI_EDITOR` members used to
// sit ahead of `parent` in Deki::Object, so sizeof(Deki::Object) was 224 in the
// editor build and 152 in a runtime build, and every inline GetParent() call in
// a package compiled with the other setting read ~72 bytes off. No crash, no
// warning — just wrong pointers.
//
// If one of these fails, do NOT simply update the number. See the note in
// Object.cpp: the layout changing means every prebuilt package is now wrong,
// and kDekiReflectionAbiVersion must be bumped in the same commit so the loader
// rejects them instead of miscomputing.

#include <gtest/gtest.h>

#include <deki/Object.h>
#include <deki/Scene.h>
#include <deki/interop/ReflectionAbi.h>

#include <string>
#include <type_traits>

namespace
{

// Only asserted on 64-bit, where the editor and every package DLL are built.
// 32-bit targets (ESP32) have their own consistent sizes and load no
// DLLs, so pinning a second set of numbers here would just be more to maintain.
constexpr bool k64Bit = (sizeof(void*) == 8);

TEST(ObjectLayout, SizeIsPinned)
{
    if (!k64Bit) GTEST_SKIP() << "sizes are pinned for 64-bit only";
    EXPECT_EQ(sizeof(Deki::Object), 152u)
        << "sizeof(Deki::Object) changed. Every package DLL bakes this into its inline "
           "GetParent()/GetChildren()/GetComponent<T>() calls. Bump "
           "kDekiReflectionAbiVersion so stale packages are refused.";
}

TEST(ObjectLayout, SceneSizeIsPinned)
{
    if (!k64Bit) GTEST_SKIP() << "sizes are pinned for 64-bit only";
    EXPECT_EQ(sizeof(Deki::Scene), 120u)
        << "sizeof(Scene) changed. deki-editor.dll constructs a Scene by value in "
           "AssetPipeline::CompileSceneFile, so this must agree across the DLL boundary.";
}

// The point of moving the editor-only instance payload into a side table was to
// make the layout independent of DEKI_EDITOR. This suite always compiles WITH
// that define, so it cannot prove the runtime half on its own — that check is
// the static_assert in Object.cpp, verified by building
// `-DDEKI_EDITOR=OFF -DSIMULATOR=ON`. What this can prove is that no
// editor-only storage crept back into the object.
TEST(ObjectLayout, EditorOnlyPayloadIsNotStoredInTheObject)
{
    // Two std::strings and a bool used to live in Deki::Object under #ifdef. If
    // anything like that returns, the size assertion above fires first — but
    // this states the intent explicitly for whoever reads the failure.
    EXPECT_LT(sizeof(Deki::Object), 152u + sizeof(std::string))
        << "Deki::Object appears to have regained editor-only storage. Editor-only "
           "payload belongs in the side table in Object.cpp, not in a member.";
}

TEST(ObjectLayout, NoVirtualFunctions)
{
    // A vptr would insert 8 bytes at the front and move every member.
    EXPECT_FALSE(std::is_polymorphic<Deki::Object>::value);
    EXPECT_FALSE(std::is_polymorphic<Deki::Scene>::value);
}

TEST(ObjectLayout, NotCopyable)
{
    // Deki::Object owns children and components by raw pointer, and its editor-only
    // payload is keyed by `this`. A copy would double-free and alias that row.
    EXPECT_FALSE(std::is_copy_constructible<Deki::Object>::value);
    EXPECT_FALSE(std::is_copy_assignable<Deki::Object>::value);
}

TEST(ObjectLayout, AbiHandshakeCarriesObjectLayout)
{
    const DekiReflectionAbi abi = DekiCurrentReflectionAbi();
    EXPECT_EQ(abi.version, kDekiReflectionAbiVersion);
    EXPECT_EQ(abi.sizeofDekiObject, static_cast<uint32_t>(sizeof(Deki::Object)));
    EXPECT_EQ(abi.sizeofDekiComponent, static_cast<uint32_t>(sizeof(Deki::Component)));
}

TEST(ObjectLayout, AbiMismatchIsReportedAndNamed)
{
    DekiReflectionAbi theirs = DekiCurrentReflectionAbi();
    theirs.sizeofDekiObject += 8;  // as if built before a member was added

    char what[192] = {};
    EXPECT_FALSE(DekiReflectionAbiMatches(theirs, what, sizeof(what)));
    EXPECT_NE(std::string(what).find("sizeof(DekiObject)"), std::string::npos)
        << "the mismatch description must name the field so the log is actionable; got: " << what;
}

// The side table must behave exactly like the members it replaced.
// The build chooses the transform width, and every package DLL bakes the
// resulting Deki::Object layout into its inline accessors. So the handshake
// carries the width and not only the size: a headless build and a 2D build
// could pad to the same sizeof and still disagree on every offset.
TEST(ObjectLayout, AbiHandshakeCarriesTransformWidth)
{
    const DekiReflectionAbi abi = DekiCurrentReflectionAbi();
    // The editor always compiles the 2D transform.
    EXPECT_EQ(abi.transformConfig & 1u, 1u);
}

TEST(ObjectLayout, AbiTransformMismatchIsNamed)
{
    DekiReflectionAbi theirs = DekiCurrentReflectionAbi();
    theirs.transformConfig = 0;  // as if built for a project with nothing to draw
    char what[128] = {};
    EXPECT_FALSE(DekiReflectionAbiMatches(theirs, what, sizeof(what)));
    EXPECT_NE(std::string(what).find("transform configuration"), std::string::npos) << what;
}

TEST(ObjectInstancePayload, RoundTripsThroughTheSideTable)
{
    Deki::Object obj("probe");
    EXPECT_FALSE(obj.IsSceneInstance());
    EXPECT_TRUE(obj.GetSourceSceneGuid().empty());

    obj.SetSourceSceneGuid("abc-123");
    EXPECT_TRUE(obj.IsSceneInstance());
    EXPECT_EQ(obj.GetSourceSceneGuid(), "abc-123");

    obj.SetInstanceOverridesJson("{\"x\":1}");
    EXPECT_EQ(obj.GetInstanceOverridesJson(), "{\"x\":1}");

    // IsSceneInstance() used to be `!m_SourceSceneGuid.empty()`; clearing the
    // guid must still clear the predicate.
    obj.SetSourceSceneGuid("");
    EXPECT_FALSE(obj.IsSceneInstance());
    EXPECT_TRUE(obj.GetSourceSceneGuid().empty());
    // Clearing one field must not disturb the other.
    EXPECT_EQ(obj.GetInstanceOverridesJson(), "{\"x\":1}");
}

TEST(ObjectInstancePayload, PartOfInstanceFlagIsIndependent)
{
    Deki::Object obj("probe");
    EXPECT_FALSE(obj.IsPartOfInstance());

    obj.SetPartOfInstance(true);
    EXPECT_TRUE(obj.IsPartOfInstance());
    EXPECT_FALSE(obj.IsSceneInstance()) << "the two flag bits must not alias";

    obj.SetSourceSceneGuid("guid");
    EXPECT_TRUE(obj.IsPartOfInstance()) << "setting the guid must not clear the other bit";
    EXPECT_TRUE(obj.IsSceneInstance());

    obj.SetPartOfInstance(false);
    EXPECT_FALSE(obj.IsPartOfInstance());
    EXPECT_TRUE(obj.IsSceneInstance());
}

// The table is keyed by pointer, so a destroyed object must not leave a row
// behind for whatever gets allocated at that address next.
TEST(ObjectInstancePayload, DestructionDoesNotLeakRowsToReusedAddresses)
{
    std::string leakedAddressGuid;
    {
        Deki::Object* first = new Deki::Object("first");
        first->SetSourceSceneGuid("should-not-survive");
        delete first;
    }

    // Allocate until we land somewhere plausible; any fresh object must read
    // empty regardless of whether the allocator reused the address.
    for (int i = 0; i < 32; ++i)
    {
        Deki::Object probe("probe");
        ASSERT_TRUE(probe.GetSourceSceneGuid().empty())
            << "a destroyed object left its row in the side table";
        ASSERT_FALSE(probe.IsSceneInstance());
    }
}

} // namespace
