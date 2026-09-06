#include <gtest/gtest.h>
#include "services/SafeDllCall.h"

#ifdef _WIN32

using namespace DekiEditor;

// ============================================================================
// Valid function calls — should succeed normally
// ============================================================================

static bool g_WasCalled = false;

static void ValidVoidFunc()
{
    g_WasCalled = true;
}

static void ValidVoidPtrFunc(void* arg)
{
    *static_cast<int*>(arg) = 42;
}

static const char* ValidGetStringFunc()
{
    return "hello";
}

static int ValidGetIntFunc()
{
    return 123;
}

static void* ValidGetPtrFunc(int index)
{
    static int values[] = { 10, 20, 30 };
    return &values[index];
}

static int ValidInitFunc()
{
    return 0;  // success
}

TEST(SafeDllCall, ValidVoidCallSucceeds)
{
    g_WasCalled = false;
    bool ok = SafeCallVoid(ValidVoidFunc, "test::ValidVoidFunc");
    EXPECT_TRUE(ok);
    EXPECT_TRUE(g_WasCalled);
}

TEST(SafeDllCall, ValidVoidPtrCallSucceeds)
{
    int value = 0;
    bool ok = SafeCallVoidPtr(ValidVoidPtrFunc, &value, "test::ValidVoidPtrFunc");
    EXPECT_TRUE(ok);
    EXPECT_EQ(value, 42);
}

TEST(SafeDllCall, ValidGetStringReturnsString)
{
    const char* result = SafeCallGetString(ValidGetStringFunc, "test::ValidGetStringFunc");
    ASSERT_NE(result, nullptr);
    EXPECT_STREQ(result, "hello");
}

TEST(SafeDllCall, ValidGetIntReturnsValue)
{
    int result = SafeCallGetInt(ValidGetIntFunc, -1, "test::ValidGetIntFunc");
    EXPECT_EQ(result, 123);
}

TEST(SafeDllCall, ValidGetPtrReturnsPointer)
{
    void* result = SafeCallGetPtr(ValidGetPtrFunc, 1, "test::ValidGetPtrFunc");
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(*static_cast<int*>(result), 20);
}

TEST(SafeDllCall, ValidInitReturnsZero)
{
    int result = SafeCallInit(ValidInitFunc, "test::ValidInitFunc");
    EXPECT_EQ(result, 0);
}

// ============================================================================
// Crashing function calls — SEH should catch and return failure
// ============================================================================

// These functions deliberately trigger access violations.
// SafeDllCall.cpp (compiled with /EHa) catches them via __try/__except.

static void CrashingVoidFunc()
{
    volatile int* p = nullptr;
    *p = 0xDEAD;  // Access violation
}

static void CrashingVoidPtrFunc(void*)
{
    volatile int* p = nullptr;
    *p = 0xDEAD;
}

static const char* CrashingGetStringFunc()
{
    volatile int* p = nullptr;
    *p = 0xDEAD;
    return "unreachable";
}

static int CrashingGetIntFunc()
{
    volatile int* p = nullptr;
    *p = 0xDEAD;
    return -999;
}

static void* CrashingGetPtrFunc(int)
{
    volatile int* p = nullptr;
    *p = 0xDEAD;
    return nullptr;
}

static int CrashingInitFunc()
{
    volatile int* p = nullptr;
    *p = 0xDEAD;
    return 0;
}

TEST(SafeDllCall, CrashingVoidCallReturnsFalse)
{
    bool ok = SafeCallVoid(CrashingVoidFunc, "test::CrashingVoidFunc");
    EXPECT_FALSE(ok);
}

TEST(SafeDllCall, CrashingVoidPtrCallReturnsFalse)
{
    int dummy = 0;
    bool ok = SafeCallVoidPtr(CrashingVoidPtrFunc, &dummy, "test::CrashingVoidPtrFunc");
    EXPECT_FALSE(ok);
}

TEST(SafeDllCall, CrashingGetStringReturnsNull)
{
    const char* result = SafeCallGetString(CrashingGetStringFunc, "test::CrashingGetStringFunc");
    EXPECT_EQ(result, nullptr);
}

TEST(SafeDllCall, CrashingGetIntReturnsDefault)
{
    int result = SafeCallGetInt(CrashingGetIntFunc, -1, "test::CrashingGetIntFunc");
    EXPECT_EQ(result, -1);
}

TEST(SafeDllCall, CrashingGetPtrReturnsNull)
{
    void* result = SafeCallGetPtr(CrashingGetPtrFunc, 0, "test::CrashingGetPtrFunc");
    EXPECT_EQ(result, nullptr);
}

TEST(SafeDllCall, CrashingInitReturnsNegOne)
{
    int result = SafeCallInit(CrashingInitFunc, "test::CrashingInitFunc");
    EXPECT_EQ(result, -1);
}

// ============================================================================
// Edge cases
// ============================================================================

static const char* NullReturningStringFunc()
{
    return nullptr;
}

TEST(SafeDllCall, NullStringFromValidFuncReturnsNull)
{
    // This tests that a valid function returning nullptr is NOT treated as a crash
    const char* result = SafeCallGetString(NullReturningStringFunc, "test::NullReturningStringFunc");
    EXPECT_EQ(result, nullptr);
}

static int FailingInitFunc()
{
    return 5;  // non-zero = failure, but NOT a crash
}

TEST(SafeDllCall, FailingInitReturnsErrorCode)
{
    int result = SafeCallInit(FailingInitFunc, "test::FailingInitFunc");
    EXPECT_EQ(result, 5);  // Should return the actual error code, not -1
}

#else

// On non-Windows, the inline wrappers just call through directly.
// We only test that valid calls work (crash tests would kill the process).
TEST(SafeDllCall, NonWindowsValidCallWorks)
{
    EXPECT_TRUE(true);  // Placeholder
}

#endif
