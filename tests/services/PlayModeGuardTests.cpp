// PlayModeGuard: a fault inside game code during play mode is reported, not
// fatal. This used to hold only for MSVC builds (SEH); MinGW, which is the
// supported Windows toolchain, and Linux ran the game unguarded. The guard now
// rests on FaultGuard on every non-MSVC platform, so these tests are the same
// on Windows and Linux.

#include <gtest/gtest.h>

#include "services/PlayModeGuard.h"

#include <stdexcept>
#include <string>

using namespace DekiEditor;

namespace
{
// Deliberate faults. `volatile` keeps the compiler from proving the access is
// undefined and deleting it.
void NullRead()
{
    volatile int* p = nullptr;
    volatile int v = *p;
    (void)v;
}
void NullWrite()
{
    volatile int* p = nullptr;
    *p = 42;
}
}  // namespace

TEST(PlayModeGuard, NormalCallSucceeds)
{
    PlayModeGuard::CrashInfo info;
    int ran = 0;
    EXPECT_TRUE(PlayModeGuard::Invoke("Update", [&] { ++ran; }, info));
    EXPECT_EQ(ran, 1);
    EXPECT_TRUE(info.message.empty());
}

TEST(PlayModeGuard, NullReadIsContainedAndDescribed)
{
    PlayModeGuard::CrashInfo info;
    EXPECT_FALSE(PlayModeGuard::Invoke("Update", [] { NullRead(); }, info));
    EXPECT_STREQ(info.phase, "Update");
    EXPECT_NE(info.exceptionCode, 0u);
#if defined(_WIN32)
    EXPECT_NE(info.message.find("EXCEPTION_ACCESS_VIOLATION"), std::string::npos) << info.message;
    EXPECT_NE(info.accessDetails.find("READ"), std::string::npos) << info.accessDetails;
#else
    EXPECT_NE(info.message.find("SIGSEGV"), std::string::npos) << info.message;
#endif
    EXPECT_NE(info.accessDetails.find("null pointer"), std::string::npos) << info.accessDetails;
}

TEST(PlayModeGuard, NullWriteIsContained)
{
    PlayModeGuard::CrashInfo info;
    EXPECT_FALSE(PlayModeGuard::Invoke("Start", [] { NullWrite(); }, info));
#if defined(_WIN32)
    EXPECT_NE(info.accessDetails.find("WRITE"), std::string::npos) << info.accessDetails;
#endif
    EXPECT_FALSE(info.message.empty());
}

TEST(PlayModeGuard, CppExceptionsAreReportedByWhat)
{
    PlayModeGuard::CrashInfo info;
    EXPECT_FALSE(PlayModeGuard::Invoke("Awake", [] { throw std::runtime_error("boom"); }, info));
    EXPECT_EQ(info.message, "C++ exception: boom");
    EXPECT_EQ(info.exceptionCode, 0u);
}

TEST(PlayModeGuard, TheThreadKeepsWorkingAfterAFault)
{
    // The second fault is the one that used to be lost when the signal stayed
    // blocked, and the normal call proves the guard was popped cleanly.
    PlayModeGuard::CrashInfo info;
    EXPECT_FALSE(PlayModeGuard::Invoke("Update", [] { NullRead(); }, info));
    EXPECT_FALSE(PlayModeGuard::Invoke("Update", [] { NullWrite(); }, info));
    int ran = 0;
    EXPECT_TRUE(PlayModeGuard::Invoke("Update", [&] { ++ran; }, info));
    EXPECT_EQ(ran, 1);
}

TEST(PlayModeGuard, GuardsNest)
{
    PlayModeGuard::CrashInfo outer, inner;
    bool afterInner = false;
    EXPECT_TRUE(PlayModeGuard::Invoke("Outer", [&] {
        EXPECT_FALSE(PlayModeGuard::Invoke("Inner", [] { NullRead(); }, inner));
        afterInner = true;
    }, outer));
    EXPECT_TRUE(afterInner);
    EXPECT_STREQ(inner.phase, "Inner");
}
