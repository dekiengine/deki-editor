#pragma once

#include "SceneViewTypes.h"
#include <vector>

namespace Deki { class Object; }

namespace DekiEditor
{

class SceneObjectMeasurer;
class SceneTextureCache;

class SceneSelection
{
public:
    SceneSelection(SceneObjectMeasurer* measurer, SceneTextureCache* textureCache);

    // Hit test a single object at world coordinates (meters). pixelsPerMeter
    // converts the object's framebuffer-pixel display size into world meters.
    bool HitTestObject(Deki::Object* obj, float worldX, float worldY, float pixelsPerMeter);

    // Handle click with cycling support. Returns the outermost (backmost)
    // object under the click point; if currentSelection is already one of the
    // hits, returns the next one inward (wrapping). nullptr if nothing is hit.
    Deki::Object* HandleClick(const std::vector<Deki::Object*>& objects,
                            Deki::Object* currentSelection,
                            float worldX, float worldY, float pixelsPerMeter);

private:
    SceneObjectMeasurer* m_Measurer;
    SceneTextureCache* m_TextureCache;
};

}  // namespace DekiEditor
