// A component's identity is its qualified class name (deki/TypeId.h). These
// pin the spelling the compiler-derived name has, since scene files store it
// and the reflection codegen must agree with it.

#include <gtest/gtest.h>

#include <deki/TypeId.h>
#include <deki/Behaviour.h>
#include <deki/Object.h>
#include <deki/reflection/ComponentFactory.h>

#include <cstring>
#include <string>

namespace TypeIdTestNs
{
class Health : public Deki::Component
{
public:
    int hp = 10;
};
class Runner : public Deki::Behaviour
{
public:
    void Update() override { ++ticks; }
    int ticks = 0;
};
}  // namespace TypeIdTestNs

class GlobalWidget : public Deki::Component
{
};

TEST(TypeId, QualifiedNameIsTheIdentity)
{
    EXPECT_EQ(std::string(Deki::TypeNameOf<TypeIdTestNs::Health>()), "TypeIdTestNs::Health");
    EXPECT_EQ(std::string(Deki::TypeNameOf<GlobalWidget>()), "GlobalWidget");
    EXPECT_EQ(std::string(Deki::TypeNameOf<Deki::Behaviour>()), "Deki::Behaviour");

    EXPECT_EQ(std::string(Deki::TypeShortNameOf<TypeIdTestNs::Health>()), "Health");
    EXPECT_EQ(std::string(Deki::TypeShortNameOf<GlobalWidget>()), "GlobalWidget");
}

TEST(TypeId, HashMatchesTheRuntimeHashOfTheName)
{
    // The scene loader hashes the "type" string it reads; that must land on
    // the compile-time id.
    EXPECT_EQ(Deki::TypeId<TypeIdTestNs::Health>(), Deki::HashString("TypeIdTestNs::Health"));
    EXPECT_EQ(Deki::TypeId<TypeIdTestNs::Health>(), Deki::HashString(Deki::TypeName<TypeIdTestNs::Health>()));
    EXPECT_EQ(Deki::TypeId<Deki::Behaviour>(), Deki::HashString("Deki::Behaviour"));
    EXPECT_NE(Deki::TypeId<TypeIdTestNs::Health>(), Deki::TypeId<GlobalWidget>());

    // Null-terminated storage, usable as a ComponentMeta string
    EXPECT_STREQ(Deki::TypeName<GlobalWidget>(), "GlobalWidget");
    EXPECT_STREQ(Deki::TypeShortName<TypeIdTestNs::Health>(), "Health");
}

TEST(TypeId, AddComponentAttachesAnEntryEvenWithoutCodegen)
{
    Deki::Object obj("o");
    auto* health = obj.AddComponent<TypeIdTestNs::Health>();
    auto* runner = obj.AddComponent<TypeIdTestNs::Runner>();

    ASSERT_NE(health->GetTypeInfo(), nullptr);
    EXPECT_EQ(health->GetType(), Deki::TypeId<TypeIdTestNs::Health>());
    EXPECT_EQ(health->GetBaseType(), Deki::TypeId<Deki::Component>());
    EXPECT_FALSE(health->DeclaresUpdate());

    EXPECT_EQ(runner->GetType(), Deki::TypeId<TypeIdTestNs::Runner>());
    EXPECT_EQ(runner->GetBaseType(), Deki::TypeId<Deki::Behaviour>());
    EXPECT_TRUE(runner->DeclaresUpdate());  // a hand-written behaviour is allowed to tick

    EXPECT_EQ(obj.GetComponent<TypeIdTestNs::Health>(), health);
    EXPECT_EQ(obj.GetComponent<Deki::Behaviour>(), runner);  // base-type match
    EXPECT_EQ(obj.GetComponent<GlobalWidget>(), nullptr);
}

namespace TypeIdTestNs
{
class Spawnable : public Deki::Component
{
public:
    static const Deki::ComponentTypeInfo kTypeInfo;
    Spawnable() { SetTypeInfo(&kTypeInfo); }
    int value = 7;
};
}  // namespace TypeIdTestNs

namespace
{
Deki::ComponentTypeInfo MakeSpawnable()
{
    Deki::ComponentTypeInfo info = Deki::MakeHandWrittenTypeInfo<TypeIdTestNs::Spawnable>(false);
    info.create = []() -> Deki::Component* { return new TypeIdTestNs::Spawnable(); };
    return info;
}
}  // namespace
const Deki::ComponentTypeInfo TypeIdTestNs::Spawnable::kTypeInfo = MakeSpawnable();

TEST(TypeId, ObjectAddsRegisteredComponentsByIdAndByName)
{
    auto& factory = Deki::ComponentFactory::Instance();
    factory.Register(&TypeIdTestNs::Spawnable::kTypeInfo);

    Deki::Object obj("o");
    Deki::Component* byId = obj.AddComponent(Deki::TypeId<TypeIdTestNs::Spawnable>());
    ASSERT_NE(byId, nullptr);
    EXPECT_EQ(byId->GetType(), Deki::TypeId<TypeIdTestNs::Spawnable>());
    EXPECT_EQ(byId->GetOwner(), &obj);
    EXPECT_EQ(obj.GetComponent<TypeIdTestNs::Spawnable>(), byId);

    // The id of a class nobody registered
    EXPECT_EQ(obj.AddComponent(Deki::HashString("Nowhere::Nothing")), nullptr);

    factory.Unregister(Deki::TypeId<TypeIdTestNs::Spawnable>());
}

TEST(TypeId, ARegisteredEntryWinsOverTheFallback)
{
    static const Deki::ComponentTypeInfo info = Deki::MakeHandWrittenTypeInfo<GlobalWidget>(/*declaresUpdate=*/false);
    auto& factory = Deki::ComponentFactory::Instance();
    factory.Register(&info);

    Deki::Object obj("o");
    auto* w = obj.AddComponent<GlobalWidget>();
    EXPECT_EQ(w->GetTypeInfo(), &info);
    EXPECT_EQ(factory.Find(Deki::TypeId<GlobalWidget>()), &info);
    EXPECT_EQ(factory.Create(Deki::TypeId<GlobalWidget>()), nullptr);  // no create() on a hand-written entry

    factory.Unregister(Deki::TypeId<GlobalWidget>());
    EXPECT_EQ(factory.Find(Deki::TypeId<GlobalWidget>()), nullptr);
}
