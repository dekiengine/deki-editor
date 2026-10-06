// set_property takes a colour as [r, g, b, a] or as the "#RRGGBBAA" text
// scene files use.

#include <gtest/gtest.h>

#include "core/commands/PropertyTypeHandlers.h"

#include <deki/Color.h>
#include <deki/Component.h>

#include <cstddef>

namespace
{

struct Tinted : Deki::Component
{
    Deki::Color color = Deki::Color(1, 2, 3, 4);
};

void Apply(Tinted& t, const nlohmann::json& value)
{
    DekiEditor::ApplyColor(&t, offsetof(Tinted, color), value);
}

}  // namespace

TEST(PropertyColor, TakesAnArray)
{
    Tinted t;
    Apply(t, nlohmann::json::array({ 10, 20, 30, 40 }));
    EXPECT_EQ(t.color.r, 10);
    EXPECT_EQ(t.color.a, 40);
}

TEST(PropertyColor, TakesHexText)
{
    Tinted t;
    Apply(t, "#3A5BA0FF");
    EXPECT_EQ(t.color.r, 0x3A);
    EXPECT_EQ(t.color.g, 0x5B);
    EXPECT_EQ(t.color.b, 0xA0);
    EXPECT_EQ(t.color.a, 0xFF);
    Apply(t, "#102030");
    EXPECT_EQ(t.color.b, 0x30);
    EXPECT_EQ(t.color.a, 0xFF);
}

TEST(PropertyColor, IgnoresTextThatIsNotAColour)
{
    Tinted t;
    Apply(t, "red");
    Apply(t, "#12345G");
    EXPECT_EQ(t.color.r, 1);
    EXPECT_EQ(t.color.a, 4);
}
