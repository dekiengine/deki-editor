// The rule that stops an older editor from opening a project a newer one
// saved, which would rewrite its scenes in the older format and drop whatever
// the newer editor stored.
//
// It reads deki.json's "lastSavedBy". These tests exist because the first cut
// of this gate read "engineVersion" instead, and that key cannot carry the
// rule: before 0.16.0 it was written once at scaffold time and never updated,
// and the scaffold's missing-macro fallback wrote a literal "1.0.0". Since
// 1.0.0 outranks every version the project has actually shipped, that gate
// refused every existing project, including deki-demo. No threshold fixes it,
// because a stale 1.0.0 and a genuine future 1.0.0 are the same string. Hence
// a separate key, absent on every project written before it existed.

#include <gtest/gtest.h>

#include "core/Project.h"

using DekiEditor::ProjectNeedsNewerEditor;

TEST(ProjectVersionGate, AProjectWithNoRecordIsAllowed)
{
    // Every project written before 0.16.0. Absence means older, never newer.
    EXPECT_FALSE(ProjectNeedsNewerEditor("", "0.16.0"));
}

TEST(ProjectVersionGate, AProjectFromAnOlderEditorIsAllowed)
{
    EXPECT_FALSE(ProjectNeedsNewerEditor("0.15.0", "0.16.0"));
    EXPECT_FALSE(ProjectNeedsNewerEditor("0.16.0", "0.17.0"));
    EXPECT_FALSE(ProjectNeedsNewerEditor("0.9.0", "1.0.0"));
}

TEST(ProjectVersionGate, AProjectFromThisEditorIsAllowed)
{
    EXPECT_FALSE(ProjectNeedsNewerEditor("0.16.0", "0.16.0"));
}

TEST(ProjectVersionGate, AProjectFromANewerEditorIsRefused)
{
    EXPECT_TRUE(ProjectNeedsNewerEditor("0.17.0", "0.16.0"));
    EXPECT_TRUE(ProjectNeedsNewerEditor("1.0.0", "0.16.0"));
    EXPECT_TRUE(ProjectNeedsNewerEditor("0.16.1", "0.16.0"));
}

TEST(ProjectVersionGate, TheStaleScaffoldValueDoesNotReachTheGate)
{
    // The exact shape of the bug: deki-demo carried engineVersion "1.0.0" from
    // a scaffold fallback while running 0.15.0. Read as lastSavedBy it would
    // refuse the project, so the gate must not read that key at all - which is
    // enforced by the key being absent, i.e. the empty case above. This test
    // documents why "1.0.0" being refused (the case just above) is correct for
    // lastSavedBy and wrong for engineVersion.
    EXPECT_TRUE(ProjectNeedsNewerEditor("1.0.0", "0.16.0"));
    EXPECT_FALSE(ProjectNeedsNewerEditor("", "0.16.0"));
}
