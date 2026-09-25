# Sprite System

## Overview

Spritesheets go through an import pipeline: PNG source file -> compiled .tex file with embedded frame metadata. All frames live **inside the parent .tex file** as regions - no separate files are generated per frame.

```
assets/sprites/player.png (source)
  -> cache/{parentGuid}  (compiled .tex with FrameList chunk)
```

Unlike fonts (where each size produces a separate file), sprite frames are sub-regions of a single texture. The parent .tex file contains both the pixel data and a FrameList metadata chunk listing all frame coordinates.

## GUID Generation

Frame GUIDs are **deterministic** from the parent GUID + frame index:

```
frameGuid = GenerateDeterministicGuid(parentGuid + ":" + frameIndex)
```

This means:
- Same parent + index always produces the same frame GUID
- Cache can be rebuilt from scratch without breaking references
- Different frames never collide

## .tex File Format (2DTX)

```
[Header - 32 bytes]
  magic: "2DTX" (4 bytes)
  version: uint32_t
  width, height: uint32_t
  format: uint32_t (RGB565, RGB565A8, RGBA8888, ALPHA8)
  dataSize: uint32_t
  metadataSize: uint32_t
  flags: uint32_t (HasTransparency, HasAlpha, IsSprite)

[Pixel Data]
  Raw pixel data in the specified format

[Metadata Chunks]
  numChunks: uint32_t

  For each chunk:
    chunkType: uint32_t (1=Sprite, 2=FrameList, 3=ChromaKey, 4=SourceSize)
    chunkSize: uint32_t
    ...chunk data...
```

### Sprite Chunk (type 1)

Grid-based spritesheets store default frame dimensions:

```
frameWidth:  int32_t
frameHeight: int32_t
```

### FrameList Chunk (type 2)

All frames with their GUIDs and coordinates:

```
frameCount: uint16_t

For each frame (52 bytes):
  guid:   36 bytes (no null terminator)
  x:      int32_t
  y:      int32_t
  width:  int32_t
  height: int32_t
```

### SourceSize Chunk (type 4)

Present only when the texture is stored smaller than its image (Max Size):

```
width:  uint32_t   the image's size
height: uint32_t
```

The Sprite and FrameList chunks, nine-slice borders and tilesets stay in the
image's pixels. `Sprite::ApplySourceSize` brings frames, borders and
`pixelsPerMeter` to the stored pixels at load, so a sprite keeps its size in
the world; a tileset maps its tile rects with `Sprite::SourceToStoredX/Y`.
Both use round(v x stored / image) on rect edges, the rule the encoder
resampled each frame with, so frames that touch still touch and none bleeds
into another.

## .data Sidecar File

Each spritesheet can have a `.data` JSON sidecar (e.g., `player.png.data`):

```json
{
  "guid": "random-uuid-for-png-file",
  "slicing": {
    "mode": "grid",
    "frameWidth": 32,
    "frameHeight": 32
  }
}
```

For atlas mode (custom frame definitions):

```json
{
  "guid": "random-uuid-for-png-file",
  "slicing": {
    "mode": "atlas",
    "frames": [
      { "x": 0, "y": 0, "width": 32, "height": 48 },
      { "x": 32, "y": 0, "width": 24, "height": 48 }
    ]
  }
}
```

## Import Pipeline: TextureImporter

**File:** `deki-editor/src/TextureImporter.cpp`

TextureImporter handles PNG -> .tex conversion with frame metadata:

### Two-Pass Import

**Pass 1: Discovery**
- Scan all image files in assets/
- Generate deterministic SubAsset GUIDs for each frame
- Register frame coordinates in `s_FrameDataRegistry`

**Pass 2: Compilation**
- Load source image via stb_image
- Convert to target format (RGB565, etc.)
- Write .tex file with header + pixel data + metadata chunks
- FrameList chunk contains all frame GUIDs + coordinates

### SubAsset Generation

```cpp
// Grid mode
for (row = 0; row < rows; row++) {
    for (col = 0; col < cols; col++) {
        int index = row * cols + col;
        subAsset.guid = GenerateDeterministicGuid(parentGuid + ":" + std::to_string(index));
        subAsset.parentGuid = parentGuid;
        subAsset.subAssetIndex = index;
    }
}
```

### Frame Data Registry

In-memory cache for quick frame coordinate lookups in the editor:

```cpp
static std::unordered_map<std::string, SpriteFrameData> s_FrameDataRegistry;

// API:
TextureImporter::RegisterFrameData(frameGuid, data);
TextureImporter::GetFrameData(frameGuid);
TextureImporter::ClearFrameData();
```

## AssetRef Fields for Sprites

On a SpriteComponent:

```
sprite.guid   = parent sprite GUID (the .tex file to load)
sprite.source = frame GUID (which sub-region to render)
```

### Why guid = parent, source = frame

Sprite frames are **regions within one file**, not separate files. The parent .tex contains all frames embedded in its FrameList chunk. So:

- `sprite.guid` must point to the parent .tex (the actual file to load from disk)
- `sprite.source` identifies which frame region within that file

This differs from fonts where each variant is a separate file (`font.guid` = variant file, `font.source` = TTF source).

### For plain sprites (no frames)

```
sprite.guid   = sprite GUID (the .tex file)
sprite.source = (empty)
```

## Frame Resolution Flow

```
1. Scene loaded from MessagePack
   -> SpriteComponent.Deserialize()
   -> sprite.guid = "parent-sprite-guid"
   -> sprite.source = "frame-guid" (or empty)

2. SpriteComponent::ResolveObjectRefs()
   -> sprite.Get() loads parent .tex via AssetManager
   -> Sprite::Load() parses FrameList chunk -> populates frames vector

3. Choose the meaningful GUID:
   -> If sprite.source is empty: use sprite.guid (full texture)
   -> If sprite.source is set:  use sprite.source (specific frame)
   -> Call OnAssetRefResolved(propertyName, loadedSprite, frameGuid)

4. SpriteComponent::OnAssetRefResolved()
   -> Sprite::FindFrame(frameGuid) -> linear search in frames vector
   -> SetFrameRect(frame->x, frame->y, frame->width, frame->height)

5. SpriteComponent::RenderContent()
   -> Uses frame_x, frame_y, frame_width, frame_height for QuadBlit source rect
```

### Generated ResolveObjectRefs

The code generator produces this pattern:

```cpp
void SpriteComponent::ResolveObjectRefs(Scene* scene) {
    if (auto* _loaded_sprite = sprite.Get()) {
        const char* _frameGuid = sprite.source.empty()
            ? sprite.guid.c_str()
            : sprite.source.c_str();
        OnAssetRefResolved("sprite", _loaded_sprite, _frameGuid);
    }
}
```

This ensures `OnAssetRefResolved` receives the **meaningful identifier** (frame GUID for spritesheets, sprite GUID for plain sprites).

### FindFrame

```cpp
const SpriteFrame* Sprite::FindFrame(const std::string& guid) const {
    for (size_t i = 0; i < frames.size(); i++) {
        if (guid == frames[i].guid) return &frames[i];
    }
    return nullptr;
}
```

Linear search - frame counts are small (typically < 100).

## Assignment in Editor

**File:** `src/panels/inspector/AssetPropertyEditor.cpp`

When a user drops a frame onto a SpriteComponent in the inspector:

```cpp
// Frame dropped (SubAsset)
assetRefBase.guid = subAsset->parentGuid;   // Parent for loading
assetRefBase.source = droppedGuid;          // Frame for coordinate lookup

// Plain sprite dropped
assetRefBase.guid = droppedGuid;            // Sprite for loading
assetRefBase.source = "";                   // No frame
```

## GUID Registration (Editor)

SubAsset GUIDs are registered with AssetManager so that `LookupPath` works:

```cpp
// EditorApp.cpp
AssetManager::Get()->RegisterGuid(subAsset.guid, info.guid);
// Maps: frameGuid -> parentGuid
// LookupPath(frameGuid) -> cacheDir/parentGuid -> loads parent .tex
```

## Export Pipeline

During `AssetExporter::ExportToStorage()`:

```
1. CollectUsedAssets()
   -> Scans all scenes for SpriteComponents
   -> Collects parent GUIDs (sprite.guid)

2. CopyUsedAssetsToOutput()
   -> Copies cache/{parentGuid} -> storage/{parentGuid}
   -> Only the parent .tex is copied (frames are embedded)

3. ExportAssetLookupTable()
   -> Adds parent GUID to binary lookup table
   -> Frame GUIDs are NOT in the lookup table
   -> At runtime, frames are found via FindFrame() after loading parent
```

**Key difference from fonts:** Font variants need separate entries in the lookup table because they are separate files. Sprite frames do not - they are found via `FindFrame()` after the parent is loaded.

## Comparison: Fonts vs Sprites

| Aspect | Fonts | Sprites |
|--------|-------|---------|
| Virtual assets | Separate files (variant + atlas) | Regions within one file |
| `guid` field | Variant GUID (baked file) | Parent GUID (.tex file) |
| `source` field | TTF source GUID | Frame GUID |
| Deterministic seed | `source + ":" + size` | `parent + ":" + index` |
| Exported files | One variant + one atlas per size | One .tex per spritesheet |
| Lookup table entries | Variant + atlas GUIDs | Parent GUID only |
| Runtime resolution | `LookupPath(variantGuid)` -> file | `LookupPath(parentGuid)` -> file, then `FindFrame(frameGuid)` |

## Key Files

| File | Purpose |
|------|---------|
| `deki-editor/src/TextureImporter.cpp` | PNG -> .tex conversion, SubAsset generation |
| `deki-editor/include/deki-editor/TextureImporter.h` | TextureImporter interface, SpriteFrameData |
| `../../modules/deki-2d/Sprite.h` | Sprite class, SpriteFrame struct, FindFrame |
| `../../modules/deki-2d/Sprite.cpp` | Sprite loading, FrameList parsing |
| `../../modules/deki-2d/SpriteComponent.h` | SpriteComponent with frame rect fields |
| `../../modules/deki-2d/generated/Deki2DReflection0.gen.cpp` | Generated ResolveObjectRefs with source pattern (one file per package, not per component) |
| `src/panels/inspector/AssetPropertyEditor.cpp` | Frame assignment UI |
| `src/services/AssetExporter.cpp` | Export pipeline |
