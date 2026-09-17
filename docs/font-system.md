# Font System

## Overview

Fonts go through a compilation pipeline: TTF source file -> baked BitmapFont (.dfont) + texture atlas. Each font size produces a **separate pair of files** with deterministic GUIDs.

```
assets/fonts/MyFont.ttf (source)
  -> cache/{variantGuid}  (compiled .dfont for 16px)
  -> cache/{atlasGuid}    (texture atlas for 16px)
  -> cache/{variantGuid2} (compiled .dfont for 24px)
  -> cache/{atlasGuid2}   (texture atlas for 24px)
```

## GUID Generation

Font variant GUIDs are **deterministic** from the source GUID + font size:

```
variantGuid = GenerateDeterministicGuid(sourceGuid + ":" + fontSize)
atlasGuid   = GenerateDeterministicGuid(sourceGuid + ":" + fontSize + ":atlas")
```

This means:
- Same source + size always produces the same GUID
- Cache can be rebuilt from scratch without breaking references
- Different sizes never collide

## .data Sidecar File

Each font has a `.data` JSON sidecar (e.g., `MyFont.ttf.data`):

```json
{
  "guid": "random-uuid-for-ttf-file",
  "fontSettings": {
    "sizes": [12, 16, 24],
    "firstChar": 32,
    "lastChar": 126
  },
  "variants": {
    "12": { "guid": "deterministic-variant-guid", "atlasGuid": "deterministic-atlas-guid" },
    "16": { "guid": "...", "atlasGuid": "..." },
    "24": { "guid": "...", "atlasGuid": "..." }
  }
}
```

- `fontSettings.sizes`: Which sizes are configured for this font
- `variants`: Maps each size to its variant + atlas GUIDs (always regenerated deterministically)

## Baking Authority: FontSyncHandler

**File:** `../../modules/deki-2d/editor/FontSyncHandler.cpp`

FontSyncHandler is the **single authority** for font baking. All baking requests go through it:

```
EnsureFontSizeBaked(sourceGuid, fontSize)
  -> Updates .data file with the requested size
  -> Calls pipeline->RefreshAsset() to trigger HandleFontSync
  -> HandleFontSync reads .data, compiles TTF for each size
  -> Writes cache/{variantGuid} (dfont) and cache/{atlasGuid} (atlas)
  -> Registers variant GUID with AssetManager
```

### Who calls EnsureFontSizeBaked:
- `TextComponentEditor.cpp` - "Save and Use" button in font preview
- `AssetPropertyEditor.cpp` - "Save & Use" button in asset property editor

### Registration
After baking, FontSyncHandler registers the variant with AssetManager:
```cpp
AssetManager::Get()->RegisterGuid(variantGuid, variantGuid);
```
This enables `LookupPath(variantGuid)` -> `cacheDir/variantGuid` in the editor.

## AssetRef Fields for Fonts

On a TextComponent:
```
font.guid   = variant GUID (baked .dfont file for a specific size)
font.source = TTF source GUID (the original font file)
```

`font.Get()` loads via `font.guid` -> `AssetManager::LoadByGuidAndType` ->
`BitmapFont::Load`.

### How font.guid stays in sync

In `TextComponent::RenderContent` (editor mode):
```cpp
std::string expectedGuid = GenerateDeterministicGuid(font.source + ":" + std::to_string(fontSize));
if (font.guid != expectedGuid) {
    font.guid = expectedGuid;
    font.ptr = nullptr;
    font.loadAttempted = false;
}
```

This ensures that when the user changes `fontSize` in the inspector, the next render frame loads the correct variant.

## Preview Mode (Editor Only)

The editor supports previewing fonts at arbitrary sizes without baking to disk:

1. User enables preview in TextComponentEditor
2. `GetEditorFontVariant()` compiles TTF on-demand to an in-memory BitmapFont
3. Single preview slot - only one preview exists at a time
4. Not persisted to cache - purely for testing
5. "Save and Use" button bakes the previewed size via `EnsureFontSizeBaked`

## Export Pipeline

During `AssetExporter::ExportToStorage()`:

```
1. CollectUsedAssets()
   -> Scans all scenes for TextComponents
   -> CollectFontUsage() extracts font.source + fontSize
   -> Creates FontUsageInfo {sourceGuid, fontSize}

2. BakeFontsForExport(fontUsages)
   -> For each usage, generates deterministic variant + atlas GUIDs
   -> Checks cache; bakes if missing
   -> Returns map of key -> GUID for both variants and atlases
   -> Adds ALL GUIDs (variant + atlas) to usedGuids

3. CopyUsedAssetsToOutput()
   -> Copies cache/{guid} -> storage/{guid} for all used GUIDs

4. ExportAssetLookupTable()
   -> Adds variant + atlas GUIDs to binary lookup table
   -> Runtime resolves: hash(variantGuid) -> variantGuid -> storage/{variantGuid}
```

## BitmapFont File Format (.dfont)

```
[Header]
  magic: "DFNT" (4 bytes)
  version: uint8_t (currently 1)
  first_char, last_char: uint8_t
  line_height, baseline: uint8_t
  glyph_count: uint16_t
  atlas_path_len: uint16_t

[Glyph Data]
  glyph_count x GlyphInfo structs

[Atlas Path]
  Relative path to atlas file (e.g., just the atlasGuid string)
```

The atlas path stored inside the dfont is relative to the dfont file's directory. Since both files live in the same cache/ or storage/ directory, it's just the atlasGuid string.

## Key Files

| File | Purpose |
|------|---------|
| `../../modules/deki-2d/TextComponent.h` | fontSize, previewEnabled, previewSize fields |
| `../../modules/deki-2d/TextComponent.cpp` | RenderContent font loading, preview compilation |
| `../../modules/deki-2d/BitmapFont.cpp` | Font loading, glyph rendering |
| `../../modules/deki-2d/editor/FontSyncHandler.cpp` | Baking authority |
| `../../modules/deki-2d/editor/FontCompiler.cpp` | TTF -> BitmapFont compilation |
| `../../modules/deki-2d/editor/TextComponentEditor.cpp` | Inspector UI, font size dropdown |
| `src/services/AssetExporter.cpp` | Export-time baking and copying |
