// A colour is read the one way every property is (Deki::Scene::PropertyFromJson):
// the "#RRGGBBAA" text scene files hold, "#RRGGBB" for opaque, or [r, g, b, a]
// as code builds it. Anything else leaves it as it was and says so.

#include <gtest/gtest.h>

#include <deki/Color.h>
#include <deki/Component.h>
#include <deki/Scene.h>
#include <deki/reflection/Property.h>
#include <nlohmann/json.hpp>

#include <cstddef>

namespace
{

struct Tinted : Deki::Component
{
    Deki::Color color = Deki::Color(1, 2, 3, 4);
};

Deki::PropertyInfo ColorProperty()
{
    Deki::PropertyInfo prop = {};
    prop.name = "color";
    prop.type = Deki::PropertyType::Color;
    prop.offset = offsetof(Tinted, color);
    return prop;
}

bool Apply(Tinted& t, const nlohmann::json& value)
{
    return Deki::Scene::PropertyFromJson(&t, ColorProperty(), value);
}

}  // namespace

TEST(PropertyColor, TakesAnArray)
{
    Tinted t;
    EXPECT_TRUE(Apply(t, nlohmann::json::array({ 10, 20, 30, 40 })));
    EXPECT_EQ(t.color.r, 10);
    EXPECT_EQ(t.color.a, 40);
}

TEST(PropertyColor, TakesHexText)
{
    Tinted t;
    EXPECT_TRUE(Apply(t, "#3A5BA0FF"));
    EXPECT_EQ(t.color.r, 0x3A);
    EXPECT_EQ(t.color.g, 0x5B);
    EXPECT_EQ(t.color.b, 0xA0);
    EXPECT_EQ(t.color.a, 0xFF);
    EXPECT_TRUE(Apply(t, "#102030"));
    EXPECT_EQ(t.color.b, 0x30);
    EXPECT_EQ(t.color.a, 0xFF);
}

TEST(PropertyColor, IgnoresWhatIsNotAColour)
{
    Tinted t;
    EXPECT_FALSE(Apply(t, "red"));
    EXPECT_FALSE(Apply(t, "#12345G"));
    EXPECT_FALSE(Apply(t, nlohmann::json::array({ 10, 20, 300, 40 })));
    EXPECT_FALSE(Apply(t, 42));
    EXPECT_EQ(t.color.r, 1);
    EXPECT_EQ(t.color.a, 4);
}

TEST(PropertyColor, WritesTheTextItReads)
{
    Tinted t;
    t.color = Deki::Color(0x3A, 0x5B, 0xA0, 0x80);
    const nlohmann::json written = Deki::Scene::PropertyToJson(&t, ColorProperty());
    EXPECT_EQ(written, "#3A5BA080");
    Tinted back;
    EXPECT_TRUE(Apply(back, written));
    EXPECT_EQ(back.color.a, 0x80);
}
