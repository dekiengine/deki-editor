#pragma once

// SceneMemoryEstimate: what the open scene would take on a target, worked out
// in the editor without running it. Every figure is an estimate and the panel
// says so; the measured figures come from a running game (MemoryReport).
//
// What it covers:
//  - the frame buffer or render strip, from the target's screen and the
//    project's Rendering settings, in internal RAM;
//  - each asset the scene's components name: an image at the size and format
//    it is stored in for that target, anything else at its built file's size;
//  - each object: the engine's object and its components (their size in this
//    editor, which is a 64-bit build; on a 32-bit board they are smaller), and
//    the assets it uses.
// Assets go to external RAM where the target has it, as the engine's loaders
// ask for it.

#include <cstdint>
#include <string>
#include <vector>

namespace Deki
{
class Scene;
class Object;
}  // namespace Deki

namespace DekiEditor
{

class AssetPipeline;
struct PlatformConfig;

/// One thing that takes memory.
struct MemoryEstimateItem
{
    std::string name;    ///< an asset's path, or what a rendering buffer is (English)
    std::string kind;    ///< an asset's type ("Sprite", "BitmapFont"), or a rendering kind (English)
    std::string detail;  ///< how it was worked out: "320 x 240 RGB565", "file size"
    uint64_t bytes = 0;
    bool external = false;  ///< in the External region on this target
    bool fileSize = false;  ///< `bytes` is the built file's size, not worked out from the content
    std::string guid;       ///< the asset, empty for anything else
    int users = 0;          ///< objects that use it
};

/// One object and what it brings in.
struct ObjectMemoryEstimate
{
    std::string name;
    std::string path;                     ///< "Parent/Child", to tell same-named objects apart
    Deki::Object* object;                 ///< valid until the scene changes
    uint64_t ownBytes = 0;                ///< the object and its components
    uint64_t assetBytes = 0;              ///< the assets it uses, each counted in full
    std::vector<std::string> components;  ///< display names
    std::vector<std::string> assetGuids;
};

struct SceneMemoryEstimate
{
    std::string targetId;  ///< empty for the editor's own desktop
    std::string targetName;

    /// Each region's size on the target, 0 when not known. External comes
    /// from the target's description; internal only from a board that was
    /// asked (MemoryReport::internalTotal), since no target description has it.
    uint64_t internalBudget = 0;
    uint64_t externalBudget = 0;
    bool internalBudgetMeasured = false;
    bool hasExternal = false;

    uint64_t internalBytes = 0;
    uint64_t externalBytes = 0;

    std::vector<MemoryEstimateItem> rendering;
    std::vector<MemoryEstimateItem> assets;  ///< biggest first
    std::vector<ObjectMemoryEstimate> objects;
    uint64_t objectBytes = 0;  ///< every object and component, in internal RAM

    /// How the frame reaches the screen on this target: "whole frames",
    /// "strips of 40 rows", or "Auto" with what decides it. English.
    std::string frameModeNote;
};

/// Works out the estimate for `scene` on `platform` (null: the editor's own
/// desktop, which has no budget). `measuredInternalTotal` and
/// `measuredInternalFree` come from a board's report, -1 when there is none;
/// with the free figure, Auto's choice of whole frames or strips is made the
/// way the board makes it.
SceneMemoryEstimate EstimateSceneMemory(const std::string& projectPath, AssetPipeline* pipeline, Deki::Scene* scene,
                                        const PlatformConfig* platform, int64_t measuredInternalTotal = -1,
                                        int64_t measuredInternalFree = -1);

/// The bytes an image takes once loaded on a target: its size after Max Size,
/// in the format it is stored in there. False when `cachePath` is not a
/// compiled image.
bool EstimateImageBytes(const std::string& assetPath, const std::string& cachePath, const PlatformConfig* platform,
                        uint64_t& bytes, std::string& detail);

}  // namespace DekiEditor
