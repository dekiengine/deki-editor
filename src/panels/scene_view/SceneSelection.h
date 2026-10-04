#pragma once

#include "SceneViewTypes.h"
#include <vector>

namespace Deki
{
class Object;
}

namespace DekiEditor
{

class SceneObjectMeasurer;
class SceneTextureCache;

class SceneSelection
{
public:
    SceneSelection(SceneObjectMeasurer* measurer, SceneTextureCache* textureCache);

    /// True if the object is hit at a world point (meters). `pixelsPerMeter`
    /// converts the object's drawn size in pixels to meters.
    bool HitTestObject(Deki::Object* obj, float worldX, float worldY, float pixelsPerMeter);

    /// The object a click picks: the outermost (backmost) one under the point,
    /// or, when `currentSelection` is one of the hits, the next one inward,
    /// wrapping around. nullptr if nothing is hit.
    Deki::Object* HandleClick(const std::vector<Deki::Object*>& objects, Deki::Object* currentSelection, float worldX,
                              float worldY, float pixelsPerMeter);

    /// The visible objects drawn wholly inside the world rectangle (meters,
    /// corners in any order). Wholly, not touching, so a box around some
    /// sprites does not also pick the background they sit on, or the camera.
    std::vector<Deki::Object*> ObjectsInRect(const std::vector<Deki::Object*>& objects, float x0, float y0, float x1,
                                             float y1, float pixelsPerMeter);

private:
    SceneObjectMeasurer* m_Measurer;
    SceneTextureCache* m_TextureCache;
};

}  // namespace DekiEditor
