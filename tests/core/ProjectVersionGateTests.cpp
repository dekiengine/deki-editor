// The rule that stops an older editor from opening a project a newer one
// saved, which would rewrite its scenes in the older format and drop whatever
// the newer editor stored.
//
// It reads deki.json's "lastSavedBy", never "engineVersion". Older projects
// wrote engineVersion once at scaffold time and never updated it, and the
// scaffold's fallback wrote a literal "1.0.0", which outranks every real
// version. A stale 1.0.0 and a real future 1.0.0 are the same string, so no
// threshold can fix that; hence a separate key, absent on every project
// written before it existed.

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
    // deki-demo carried engineVersion "1.0.0" from a scaffold fallback while
    // running 0.15.0. Refusing "1.0.0" is right for lastSavedBy, and is why
    // the gate must never read engineVersion.
    EXPECT_TRUE(ProjectNeedsNewerEditor("1.0.0", "0.16.0"));
    EXPECT_FALSE(ProjectNeedsNewerEditor("", "0.16.0"));
}
