# Assets

An asset is a file your game uses: an image, a font, a scene, an animation.

## Add one

Drag the file onto the Asset Browser, or copy it into the project's `assets` folder. The editor imports it on its own.

| File | Becomes |
| --- | --- |
| `.png` | An image. See [Sprites](sprites.md) |
| `.ttf`, `.bdf` | A font. See [Text](text.md) |
| `.scene` | A scene |
| `.anim` | An animation. See [Animation](animation.md) |

## Keep the .data file with it

Beside each asset is a file of the same name ending in `.data`. It holds the asset's settings and its id, and fields point at that id. Move and rename assets in the Asset Browser, so the two stay together and nothing that uses them breaks.

## Use one

Choose it in the Inspector. A field like **Sprite** or **Font** offers the assets of that kind.

For your own components, add a field:

```cpp
DEKI_EXPORT
Deki::AssetRef<Deki2D::Sprite> hurtSprite;
```

Use it like a pointer. The asset loads the first time your code needs it.

```cpp
if (hurtSprite)
{
    sprite->sprite = hurtSprite.Get();
}
```

## Different settings per device

An image's settings can differ per device, such as a smaller **Max Size** where memory is short: see [Sprites](sprites.md). Each device's build gets its own copy, so your other targets are not affected.

The **Memory** panel shows what a scene will take on a device before you run it.

How assets are stored and found is covered under **Working on Deki**, in [Asset System](asset-system.md).
