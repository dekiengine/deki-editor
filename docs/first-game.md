# Your first game

A tiny farm. Walk around the island, climb the steps to the house, plant turnips, watch them grow and pick them when they are ripe. It plays with a keyboard, a handheld's buttons or taps, and shows the controls for whichever the player uses.

![The finished game](img/first-game.webp)

It starts from a new project: see [Getting started](getting-started.md). The Inspector shows sizes in pixels and code uses meters, with 16 pixels to the meter by default: see [Units](units.md).

## Get the art and the island

The art is [Simple Cozy - Farm Asset Pack](https://aibyzz.itch.io/sc-farm-pack) by Aibyzz. Download the free version and unpack it. Deki does not include it, because its licence does not allow sharing the files. You can use it in your own games, commercial ones too.

The island is a [Tiled](https://www.mapeditor.org) map, ready to use, in <a href="first-game-files.zip">first-game-files.zip</a>. The zip also holds the map's four tilesets, the HUD's icons and the [monogram](https://datagoblin.itch.io/monogram) pixel font. The ground is painted on tile layers, steps included, and everything that stands on it, from the fences to the house, is a tile object on the `scenery` layer.

Two rectangles on that layer are for the game, not to be seen: `Steps` marks where the steps climb, and the house's `Wall` keeps the farmer out of the house and the ground behind it. Tiled has no levels, so whatever stands on the raised ground has a custom property, `level`, set to 1.

1. Open **View → Package Manager** and install **Tiled Map Editor Integration**.
2. Unpack the zip into your project's `assets` folder. It makes the folders `tilemaps`, `hud` and `fonts` there.
3. From the art pack, copy `Farm/Free ver.png`, `Farm/Objects.png` and `Farm/House.png` into `assets/tilemaps`. The tilesets look for them there.
4. Drag `Player/FPlayer 1 idle.png` and `Player/FPlayer 1 walking.png` onto the Asset Browser.
5. Select `monogram` in the Asset Browser and set its **Hinting Mode** to **Mono (1-bit, no AA)**. A pixel font then draws crisp, with no smoothed edges, like the art around it. It is drawn for size 16: one of its pixels to one on the screen.

## The scene

The island is bigger than the screen, so the camera follows the farmer.

1. Delete **Sky**.
2. On **Camera**, set **Clear Color** to `#BDE8F0`.
3. Make an empty object called **Map** at 0, 8. Add a **Tilemap Component** and set its **Tilemap** to `farm`. Then add a **Tilemap Object Spawner**, set its **Tilemap** to `farm`, **Sort Base** to 1000 and **Sort Per Meter** to 16, one order for each pixel row. The spawner is what turns the `scenery` layer into sprites when the game starts.
4. Make an empty object called **Tap Area**, drag it onto **Camera** and add an **Input Collider** with **Width** 320 and **Height** 240. It covers the screen, so every tap or click lands on it.
5. Make two empty objects, **Farm** and **Farmer**. Set the Farmer's position to 40, -16 and add a **Sprite Component** to it. Leave its **Sprite** empty: the code sets it.

## The farm

The map holds the island, so this script is only the game: where the soil is, how the turnips grow, the score, and where the farmer can walk. Create a script called `Farm`.

`src/Farm.h`:

```cpp
#pragma once

#include <deki/Engine.h>
#include <deki/assets/AssetRef.h>
#include <deki/reflection/ObjectRef.h>

#include <cmath>

#include "SpriteComponent.h"
#include "Tilemap.h"
#include "TextComponent.h"

DEKI_CATEGORY("Game")
DEKI_DESCRIPTION("Grows the turnips, keeps the score and says where the farmer can walk.")
class Farm : public Deki::Component
{
public:
    DEKI_EXPORT
    Deki::AssetRef<Deki2D::Sprite> farmSheet;

    DEKI_EXPORT
    Deki::AssetRef<DekiTiledMap::Tilemap> map;

    DEKI_EXPORT
    Deki::ObjectRef<Deki2D::TextComponent> scoreText;

    DEKI_EXPORT
    float secondsPerStage = 2.0f;

    void Start() override;
    void Update() override;

    // How high the ground is at this point, given as if it were on the lower
    // ground: 0 there, 1 on the raised ground and in between on the steps.
    // -1 where no one can stand.
    float GroundHeight(float x, float y) const;

    // Plants on the empty patch nearest this point, or picks a ripe turnip.
    void UseSoil(float x, float y);

    // Whether this point is on a patch of soil.
    bool IsSoil(float x, float y) const;

    // What stands nearer the bottom of the screen draws on top, one order
    // per pixel row, as the map's spawner sorts the scenery.
    static int DrawOrder(float y) { return 1000 - static_cast<int>(std::floor(y * 16.0f)); }

private:
    struct Patch
    {
        float x = 0.0f;
        float y = 0.0f;
        Deki2D::SpriteComponent* soil = nullptr;
        Deki2D::SpriteComponent* crop = nullptr;
        int stage = -1;  // -1: nothing planted
        float age = 0.0f;
    };

    Deki2D::SpriteComponent* Place(Deki2D::Sprite* sheet, int left, int top, int width, int height, float x, float y,
                                   int order);
    void Show(Patch& patch);
    void ShowScore();

    Patch m_Patches[20];
    int m_Score = 0;
};
```

`src/Farm.cpp`:

```cpp
#include "Farm.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
const int kIslandSize = 24;
const float kIslandTop = 12.0f;

// The first column and last row of the raised ground, where the house stands.
const float kRaisedColumn = 11.6f;
const float kRaisedRow = 9.6f;

// How close to a fence, rock or tree the farmer can get, in squares.
const float kSolidRadius = 0.5f;

// Tiled stores positions at 16 per square, and the map's grid starts 2 squares
// before the island.
const float kMapUnitsPerSquare = 16.0f;
const float kMapMargin = 2.0f;

// The middle of a square on the lower ground. A square is 2 meters wide and
// 1 tall, and the raised ground is 1 meter higher.
float WorldX(float column, float row)
{
    return column - row;
}

float WorldY(float column, float row)
{
    return kIslandTop - (column + row) * 0.5f;
}

bool IsA(const DekiTiledMap::DTilemapObject& object, const char* type)
{
    return std::strncmp(object.type, type, sizeof(object.type)) == 0;
}

// Where a map object is, in columns and rows of the ground it stands on.
struct MapPlace
{
    float column = 0.0f;
    float row = 0.0f;
    float columns = 0.0f;  // an area's size; a tile object's is its picture's
    float rows = 0.0f;
    int level = 0;

    bool Holds(float c, float r) const { return c >= column && c <= column + columns && r >= row && r <= row + rows; }
};

MapPlace PlaceOf(const DekiTiledMap::Tilemap& tilemap, const DekiTiledMap::DTilemapObject& object)
{
    // Tiled has no levels, so whatever stands on the raised ground has a
    // "level" property of 1. Tiled shows it where it would be on the lower
    // ground: a square further up and to the left.
    const DekiTiledMap::DTilemapProperty* level = tilemap.FindProperty(object, "level");
    MapPlace place;
    place.level = level ? level->intValue : 0;
    place.column = object.x / kMapUnitsPerSquare - kMapMargin + place.level;
    place.row = object.y / kMapUnitsPerSquare - kMapMargin + place.level;
    place.columns = object.width / kMapUnitsPerSquare;
    place.rows = object.height / kMapUnitsPerSquare;
    return place;
}
}  // namespace

Deki2D::SpriteComponent* Farm::Place(Deki2D::Sprite* sheet, int left, int top, int width, int height, float x, float y,
                                     int order)
{
    Deki::Object* object = GetOwner()->GetOwnerScene()->CreateDekiObject("Piece");
    object->SetWorldPosition(x, y);

    auto* sprite = object->AddComponent<Deki2D::SpriteComponent>();
    sprite->sprite = sheet;
    sprite->SetFrameRect(left, top, width, height);
    sprite->sortingOrder = order;
    return sprite;
}

void Farm::Start()
{
    Deki2D::Sprite* farm = farmSheet.Get();

    // Two long beds of soil, with a path to the gate between them.
    int next = 0;
    for (int row : { 13, 14, 16, 17 })
    {
        for (int column = 4; column <= 8; ++column)
        {
            Patch& patch = m_Patches[next++];
            patch.x = WorldX(column, row);
            patch.y = WorldY(column, row);
            patch.soil = Place(farm, 128, 0, 32, 32, patch.x, patch.y + 0.5f, 100);
            patch.crop = Place(farm, 0, 96, 32, 32, patch.x, patch.y + 0.5f, DrawOrder(patch.y - 0.5f));
            patch.crop->GetOwner()->SetActive(false);
        }
    }

    ShowScore();
}

void Farm::Update()
{
    const float seconds = Deki::Time::GetDeltaTimeF() * 0.001f;
    for (Patch& patch : m_Patches)
    {
        if (patch.stage < 0 || patch.stage == 3)
        {
            continue;
        }

        patch.age += seconds;
        if (patch.age >= secondsPerStage)
        {
            patch.age = 0.0f;
            ++patch.stage;
            Show(patch);
        }
    }
}

float Farm::GroundHeight(float x, float y) const
{
    const float sum = (kIslandTop - y) * 2.0f;
    const float column = (sum + x) * 0.5f;
    const float row = (sum - x) * 0.5f;

    const float last = kIslandSize - 1;
    if (column < -0.3f || row < -0.3f || column > last + 0.3f || row > last + 0.3f)
    {
        return -1.0f;
    }

    const DekiTiledMap::Tilemap& tilemap = *map.Get();
    float height = column > kRaisedColumn && row < kRaisedRow ? 1.0f : 0.0f;

    // The map's Steps area climbs one level, from its front edge to its back.
    for (const DekiTiledMap::DTilemapObject& object : tilemap.Objects())
    {
        const MapPlace place = PlaceOf(tilemap, object);
        if (IsA(object, "Steps") && place.Holds(column, row))
        {
            height = (place.row + place.rows - row) / place.rows;
        }
    }

    // Everything else on the map is in the way of whoever is on its level: a
    // Wall over its whole area, anything else around its point.
    const int level = height < 0.5f ? 0 : 1;
    for (const DekiTiledMap::DTilemapObject& object : tilemap.Objects())
    {
        const MapPlace place = PlaceOf(tilemap, object);
        if (IsA(object, "Steps") || place.level != level)
        {
            continue;
        }
        const bool inTheWay = IsA(object, "Wall")
                                  ? place.Holds(column, row)
                                  : std::hypot(place.column - column, place.row - row) < kSolidRadius;
        if (inTheWay)
        {
            return -1.0f;
        }
    }
    return height;
}

bool Farm::IsSoil(float x, float y) const
{
    for (const Patch& patch : m_Patches)
    {
        if (std::hypot(patch.x - x, (patch.y - y) * 2.0f) < 1.0f)
        {
            return true;
        }
    }
    return false;
}

void Farm::UseSoil(float x, float y)
{
    Patch* nearest = nullptr;
    float best = 1.4f;
    for (Patch& patch : m_Patches)
    {
        const float distance = std::hypot(patch.x - x, (patch.y - y) * 2.0f);
        if (distance < best)
        {
            best = distance;
            nearest = &patch;
        }
    }
    if (!nearest)
    {
        return;
    }

    if (nearest->stage < 0)
    {
        nearest->stage = 0;
        nearest->age = 0.0f;
    }
    else if (nearest->stage == 3)
    {
        nearest->stage = -1;
        ++m_Score;
        ShowScore();
    }
    Show(*nearest);
}

void Farm::Show(Patch& patch)
{
    // The dark soil is the watered one, just below the light one on the sheet.
    patch.soil->SetFrameRect(128, patch.stage < 0 ? 0 : 32, 32, 32);
    patch.crop->GetOwner()->SetActive(patch.stage >= 0);
    if (patch.stage >= 0)
    {
        patch.crop->SetFrameRect(patch.stage * 32, 96, 32, 32);
    }
}

void Farm::ShowScore()
{
    char line[24];
    std::snprintf(line, sizeof(line), "%d", m_Score);
    scoreText.Get()->SetText(line);
}
```

Each `Place` call makes one object that shows a rectangle of a sheet, chosen with `SetFrameRect`, so nothing needs cutting up first.

A column and a row name a square on the island: columns run down and to the right, rows down and to the left. `WorldX` and `WorldY` turn them into a position, and `GroundHeight` does the reverse. It says how high the ground is there: 0 on the lower ground, 1 on the raised ground and in between on the steps. It keeps the farmer on the island and out of everything on the `scenery` layer that stands on her level: a `Wall` over its whole area, anything else around its point. `PlaceOf` reads where a map object is, with `FindProperty` for its `level`.

`DrawOrder` is what makes the farmer walk behind the trees and in front of the turnips: lower on the screen draws on top. It matches the spawner's **Sort Base** and **Sort Per Meter**, so the farmer and the scenery sort together. The spawner sorts a tile object by its point. The house's point is the back corner of its floor, and its tileset's **Tile Offset** draws the picture from there, so the farmer draws in front of it at the door.

## The farmer

Create a script called `Farmer`.

`src/Farmer.h`:

```cpp
#pragma once

#include <deki/Engine.h>
#include <deki/assets/AssetRef.h>
#include <deki/reflection/ObjectRef.h>

#include "CameraComponent.h"
#include "Farm.h"
#include "InputCollider.h"
#include "SpriteComponent.h"

DEKI_CATEGORY("Game")
DEKI_DESCRIPTION("Walks with the arrow keys or to a tapped spot. Space or Enter plants and picks.")
class Farmer : public Deki::Component
{
public:
    DEKI_EXPORT
    Deki::ObjectRef<Farm> farm;

    DEKI_EXPORT
    Deki::ObjectRef<DekiRendering::CameraComponent> camera;

    DEKI_EXPORT
    Deki::ObjectRef<DekiInput::InputCollider> tapArea;

    DEKI_EXPORT
    Deki::AssetRef<Deki2D::Sprite> idleSheet;

    DEKI_EXPORT
    Deki::AssetRef<Deki2D::Sprite> walkSheet;

    DEKI_EXPORT
    float speed = 5.0f;

    void Start() override;
    void Update() override;

private:
    void WalkTo(float x, float y);

    bool m_HasTarget = false;  // walking to a tapped spot
    float m_TargetX = 0.0f;
    float m_TargetGround = 0.0f;
    bool m_UseWhenThere = false;
    float m_Height = 0.0f;  // how far up she has climbed, in levels
    int m_FacingX = 1;
    int m_FacingY = -1;
    float m_Clock = 0.0f;
    bool m_WasWalking = true;
    bool m_WasUseDown = false;
};
```

`src/Farmer.cpp`:

```cpp
#include "Farmer.h"

#include "DekiInput.h"
#include "Keys.h"

#include <cmath>

namespace
{
const float kFeetBelowCentre = 1.25f;

bool Held(uint32_t key)
{
    return DekiInput::DekiInput::IsKeyPressed(key);
}

// The sheet's four diagonal rows. Each is -1 or 1; up is +1.
int RowFacing(int x, int y)
{
    if (y < 0)
    {
        return x < 0 ? 4 : 5;
    }
    return x < 0 ? 6 : 7;
}
}  // namespace

void Farmer::Start()
{
    // A tap or a click anywhere on the screen sends her there.
    tapArea.Get()->onPointerDown.push_back([this](float x, float y) { WalkTo(x, y); });
}

void Farmer::WalkTo(float x, float y)
{
    // The tap is where her feet should end up. On the raised ground that is a
    // level up, so look there first.
    Farm* field = farm.Get();
    const bool up = field->GroundHeight(x, y - 1.0f) >= 0.5f;
    m_TargetX = x;
    m_TargetGround = up ? y - 1.0f : y;
    m_HasTarget = true;
    m_UseWhenThere = field->IsSoil(x, m_TargetGround);
}

void Farmer::Update()
{
    using DekiInput::Keys::Down;
    using DekiInput::Keys::Enter;
    using DekiInput::Keys::Left;
    using DekiInput::Keys::Right;
    using DekiInput::Keys::Space;
    using DekiInput::Keys::Up;

    int x = (Held(Right) ? 1 : 0) - (Held(Left) ? 1 : 0);
    int y = (Held(Up) ? 1 : 0) - (Held(Down) ? 1 : 0);

    const float seconds = Deki::Time::GetDeltaTimeF() * 0.001f;
    Deki::Object* me = GetOwner();
    const float ground = me->GetWorldY() - kFeetBelowCentre - m_Height;

    if (x != 0 || y != 0)
    {
        m_HasTarget = false;
    }
    else if (m_HasTarget)
    {
        // Head for the tapped spot as the keys would, and use the soil there.
        const float dx = m_TargetX - me->GetWorldX();
        const float dy = m_TargetGround - ground;
        x = dx > 0.15f ? 1 : (dx < -0.15f ? -1 : 0);
        y = dy > 0.08f ? 1 : (dy < -0.08f ? -1 : 0);
        if (x == 0 && y == 0)
        {
            m_HasTarget = false;
            if (m_UseWhenThere)
            {
                farm.Get()->UseSoil(me->GetWorldX(), ground);
            }
        }
    }
    const bool walking = x != 0 || y != 0;

    if (walking)
    {
        // On this view, going up the screen covers half the distance.
        const float step = speed * seconds * (x != 0 && y != 0 ? 0.71f : 1.0f);
        // Where her feet go, as if on the lower ground: Farm says how high it is there.
        const float nextX = me->GetWorldX() + x * step;
        const float nextGround = ground + y * step * 0.5f;
        const float height = farm.Get()->GroundHeight(nextX, nextGround);
        // The cliff is too high to step up or down: only the steps go between.
        if (height >= 0.0f && std::fabs(height - m_Height) < 0.5f)
        {
            m_Height = height;
            me->SetWorldPosition(nextX, nextGround + kFeetBelowCentre + height);
        }
        else
        {
            // Walking to a tapped spot stops at whatever is in the way.
            m_HasTarget = false;
        }
        // A straight key keeps the other half of the diagonal she last faced.
        m_FacingX = x != 0 ? x : m_FacingX;
        m_FacingY = y != 0 ? y : m_FacingY;
    }

    auto* sprite = me->GetComponent<Deki2D::SpriteComponent>();
    if (walking != m_WasWalking)
    {
        sprite->sprite = walking ? walkSheet.Get() : idleSheet.Get();
        m_WasWalking = walking;
    }

    m_Clock += seconds;
    const int frame = static_cast<int>(m_Clock * 8.0f) % 4;
    sprite->SetFrameRect(frame * 48, RowFacing(m_FacingX, m_FacingY) * 48, 48, 48);

    const float feet = me->GetWorldY() - kFeetBelowCentre;
    sprite->sortingOrder = Farm::DrawOrder(feet - 0.5f);

    // Enter is also a handheld's A button.
    const bool useDown = Held(Space) || Held(Enter);
    if (useDown && !m_WasUseDown)
    {
        farm.Get()->UseSoil(me->GetWorldX(), feet - m_Height);
    }
    m_WasUseDown = useDown;

    // The camera stays on her, on whole pixels so the map doesn't shimmer.
    camera.GetObject()->SetWorldPosition(std::round(me->GetWorldX() * 16.0f) / 16.0f,
                                         std::round(me->GetWorldY() * 16.0f) / 16.0f);
}
```

Arrows and Space are the keyboard. A handheld's D-pad arrives as the arrows too, and its A button as Enter, so `Update` takes Enter as well as Space: see [Input](input.md).

A tap, a click or a touch lands on the **Tap Area**. `Start` adds a listener to its `onPointerDown`, which gets the spot in world meters, and `WalkTo` sets it as where to go. `Update` then walks there as the arrow keys would, stops at anything in the way, and uses the soil if the tap was on it.

The farmer remembers how high she has climbed. Ground more than half a level higher or lower than her is refused, so the cliff stops her and only the steps take her up.

The sheet has eight rows and four frames in each. This game uses the four diagonal rows, which suit the island's angle. `SetFrameRect` picks the frame from the clock and the row from the way the farmer is facing.

## The HUD

The HUD goes under the camera, so it stays on screen. The panels are Shape Components: a darker side, a face on top of it and a thin edge, which gives them the raised look of the keys. Positions and sizes are in pixels, as the Inspector shows them. Set every Text Component's **Font** to `monogram`, **Align** to **Left**, **Vertical Align** to **Cap Center** and **Color** to `#6B4024`. The score uses **Pixel Scale** 2, which doubles every pixel of the font and keeps it crisp. "Order" is the **Sorting Order**.

The turnip counter, each object under **Camera**. **Score** is the Hello text: rename it and drag it onto **Camera**.

| Object | Position | Add |
| --- | --- | --- |
| **Counter Side** | -122, 98 | Shape Component: 60 × 28, **Corner Radius** 6, `#E2C69C`, order 1989 |
| **Counter** | -122, 100 | Shape Component: 60 × 24, **Corner Radius** 6, `#FFEFD0`, order 1990 |
| **Counter Edge** | -122, 98 | Shape Component: 60 × 28, **Corner Radius** 6, **Line Width** 1, `#6B4024`, order 1991 |
| **Turnip Icon** | -138, 101 | Sprite Component: `turnip`, order 1992 |
| **Score** (Hello, renamed) | -109, 98 | Text Component: `0`, **Font Size** 16, **Pixel Scale** 2, 32 × 28, order 1992 |

The controls go in three panels, one for each way to play, and a script shows the one that matches. Make an empty object called **Controls** under **Camera**, at 0, 0. Build the first panel under it, then duplicate the panel twice and change the copies.

| Object | Position | Add |
| --- | --- | --- |
| **Keys Hint** | -48, -92 | Shape Component: 208 × 40, **Corner Radius** 6, `#E2C69C`, order 1989 |

Under it:

| Object | Position | Add |
| --- | --- | --- |
| **Face** | 0, 2 | Shape Component: 208 × 36, **Corner Radius** 6, `#FFEFD0`, order 1990 |
| **Edge** | 0, 0 | Shape Component: 208 × 40, **Corner Radius** 6, **Line Width** 1, `#6B4024`, order 1991 |
| **Arrow Keys** | -76, 2 | Sprite Component: `arrow-keys`, order 1992 |
| **Walk Label** | -37, 2 | Text Component: `Walk`, **Font Size** 16, 26 × 36, order 1992 |
| **Space Key** | 2, 2 | Sprite Component: `space-key`, order 1992 |
| **Use Label** | 63, 2 | Text Component: `Plant, pick`, **Font Size** 16, 66 × 36, order 1992 |

| Object | Position | Add |
| --- | --- | --- |
| **Buttons Hint** | -71, -92 | Shape Component: 162 × 40, **Corner Radius** 6, `#E2C69C`, order 1989 |

Under it:

| Object | Position | Add |
| --- | --- | --- |
| **Face** | 0, 2 | Shape Component: 162 × 36, **Corner Radius** 6, `#FFEFD0`, order 1990 |
| **Edge** | 0, 0 | Shape Component: 162 × 40, **Corner Radius** 6, **Line Width** 1, `#6B4024`, order 1991 |
| **D-Pad** | -61, 2 | Sprite Component: `d-pad`, order 1992 |
| **Walk Label** | -30, 2 | Text Component: `Walk`, **Font Size** 16, 26 × 36, order 1992 |
| **A Button** | -6, 2 | Sprite Component: `a-button`, order 1992 |
| **Use Label** | 40, 2 | Text Component: `Plant, pick`, **Font Size** 16, 66 × 36, order 1992 |

| Object | Position | Add |
| --- | --- | --- |
| **Tap Hint** | -74, -92 | Shape Component: 156 × 40, **Corner Radius** 6, `#E2C69C`, order 1989 |

Under it:

| Object | Position | Add |
| --- | --- | --- |
| **Face** | 0, 2 | Shape Component: 156 × 36, **Corner Radius** 6, `#FFEFD0`, order 1990 |
| **Edge** | 0, 0 | Shape Component: 156 × 40, **Corner Radius** 6, **Line Width** 1, `#6B4024`, order 1991 |
| **Tap** | -63, 2 | Sprite Component: `tap`, order 1992 |
| **Tap Label** | 11, 2 | Text Component: `Tap to walk and farm`, **Font Size** 16, 120 × 36, order 1992 |

Create a script called `ControlsHint`, and add the **Controls Hint** component to **Controls**.

`src/ControlsHint.h`:

```cpp
#pragma once

#include <deki/Engine.h>
#include <deki/reflection/ObjectRef.h>

#include "InputCollider.h"
#include "ShapeComponent.h"

DEKI_CATEGORY("Game")
DEKI_DESCRIPTION("Shows the controls for what the player last used: the keyboard, a handheld's buttons or taps.")
class ControlsHint : public Deki::Component
{
public:
    DEKI_EXPORT
    Deki::ObjectRef<Deki2D::ShapeComponent> keysHint;

    DEKI_EXPORT
    Deki::ObjectRef<Deki2D::ShapeComponent> buttonsHint;

    DEKI_EXPORT
    Deki::ObjectRef<Deki2D::ShapeComponent> tapHint;

    DEKI_EXPORT
    Deki::ObjectRef<DekiInput::InputCollider> tapArea;

    ~ControlsHint() override;
    void Start() override;

private:
    void Show(Deki::ObjectRef<Deki2D::ShapeComponent>& hint);

    size_t m_Listener = 0;
};
```

`src/ControlsHint.cpp`:

```cpp
#include "ControlsHint.h"

#include "DekiInput.h"

#include <cstring>

void ControlsHint::Start()
{
    // Start with what this device has: a handheld's buttons, a touchscreen, or keys.
    if (DekiInput::DekiInput::GetInput("Miyoo"))
    {
        Show(buttonsHint);
    }
    else if (DekiInput::DekiInput::GetInput("DekiDisplayTouch"))
    {
        Show(tapHint);
    }
    else
    {
        Show(keysHint);
    }

    // Then follow the player. A key says which input sent it: the Miyoo's
    // buttons arrive as keys too.
    m_Listener = DekiInput::DekiInput::RegisterEventCallback(
        [this](const DekiInput::InputEvent& event)
        {
            if (event.type == DekiInput::InputEventType::KeyDown)
            {
                const bool handheld = event.source && std::strcmp(event.source, "Miyoo") == 0;
                Show(handheld ? buttonsHint : keysHint);
            }
        });

    // A tap or a click lands on the Tap Area, which the Farmer listens to as well.
    tapArea.Get()->onPointerDown.push_back([this](float, float) { Show(tapHint); });
}

ControlsHint::~ControlsHint()
{
    DekiInput::DekiInput::UnregisterEventCallback(m_Listener);
}

void ControlsHint::Show(Deki::ObjectRef<Deki2D::ShapeComponent>& hint)
{
    for (Deki::ObjectRef<Deki2D::ShapeComponent>* each : { &keysHint, &buttonsHint, &tapHint })
    {
        each->GetObject()->SetActive(each == &hint);
    }
}
```

`Start` picks the panel from the inputs the device has: `GetInput("Miyoo")` finds the Miyoo Mini's buttons, and `DekiDisplayTouch` is a touchscreen. After that it follows the player. Every key event names the input that sent it, so a key from the `Miyoo` shows the buttons and any other key shows the keyboard. A tap reaches it through the same **Tap Area** the farmer listens to: a collider can have any number of listeners.

## Connect it

Add the **Farm** component to the Farm object and fill in its fields.

![Farm's fields in the Inspector](img/first-game-fields.png)

| Field | Set it to |
| --- | --- |
| **Farm Sheet** | `Free ver` |
| **Map** | `farm` |
| **Score Text** | The Score object |

Add the **Farmer** component to the Farmer object. Set **Farm** to the Farm object, **Camera** to the Camera object, **Tap Area** to the Tap Area object, **Idle Sheet** to `FPlayer 1 idle` and **Walk Sheet** to `FPlayer 1 walking`.

On **Controls**, set **Keys Hint**, **Buttons Hint** and **Tap Hint** to the three panels, and **Tap Area** to the Tap Area object.

Save with Ctrl+S and press **Run**.

## Take it further

**Change the pace.** **Seconds Per Stage** on the Farm is how fast turnips grow.

**Rearrange the island.** Open `farm.tmj` in [Tiled](https://www.mapeditor.org) to paint the ground or move things on the `scenery` layer. Deki picks up the map when you save it.

**Change the farmer.** The pack also has `MPlayer 1 idle` and `MPlayer 1 walking`, with the same layout.

**Build more.** The pack's paid version adds water, waterfalls and fruit trees.

**Put it on a device.** Follow the last part of [Getting started](getting-started.md). In **Project Settings → Rendering**, turn on **Redraw Changes**: the camera stays on whole pixels, so each frame moves the last one and draws only the edge it uncovers and what changed. Then use the [Speed](performance.md) tools to see what a frame costs there.

## Credits

Art: [Simple Cozy - Farm Asset Pack](https://aibyzz.itch.io/sc-farm-pack) by Aibyzz. The HUD's icons were drawn for this guide. Font: [monogram](https://datagoblin.itch.io/monogram) by Vinícius Menézio, free under CC0.
