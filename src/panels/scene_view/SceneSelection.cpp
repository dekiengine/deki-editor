#include "SceneSelection.h"
#include "SceneObjectMeasurer.h"
#include "SceneTextureCache.h"
#include <deki-editor/EditorExtension.h>
#include <deki-editor/CustomEditor.h>
#include "../../services/SafeDispatch.h"
#include <deki/Object.h>
#include <deki/reflection/ComponentRegistry.h>
#include <algorithm>
#include <cmath>

namespace DekiEditor
{

SceneSelection::SceneSelection(SceneObjectMeasurer* measurer, SceneTextureCache* textureCache)
    : m_Measurer(measurer), m_TextureCache(textureCache)
{
}

bool SceneSelection::HitTestObject(Deki::Object* obj, float worldX, float worldY, float pixelsPerMeter)
{
    // Get actual display dimensions
    float displayWidth, displayHeight;
    if (!m_Measurer->GetDisplaySize(obj, displayWidth, displayHeight))
    {
        return false;  // Non-visual objects can only be selected via hierarchy panel
    }

    if (displayWidth <= 0 || displayHeight <= 0)
    {
        return false;
    }

    // GetDisplaySize returns framebuffer pixels, but the click point and object
    // transforms are in world meters (meters-internal). Convert the pixel extent
    // to meters via 1/PPM so the bounds check is unit-consistent (matches the
    // gizmo and snap math). Without this the box is off by a factor of PPM and
    // nearly every object registers a hit.
    const float ppm = (pixelsPerMeter > 0.0f) ? pixelsPerMeter : 1.0f;
    const float invPPM = 1.0f / ppm;

    // Use world transforms for hierarchy support (in meters)
    float halfW = displayWidth * obj->GetWorldScaleX() * 0.5f * invPPM;
    float halfH = displayHeight * obj->GetWorldScaleY() * 0.5f * invPPM;

    // Transform click point into object's local space (accounting for world rotation).
    // world rotation is in radians (engine convention); negate for the inverse rotation.
    float rotRad = -obj->GetWorldRotation();
    float cosR = std::cos(rotRad);
    float sinR = std::sin(rotRad);

    // Translate to object center (use world position)
    float localX = worldX - obj->GetWorldX();
    float localY = worldY - obj->GetWorldY();

    // Apply inverse rotation (transpose): swap sign of sin terms
    float rotatedX = localX * cosR + localY * sinR;
    float rotatedY = -localX * sinR + localY * cosR;

    // First check bounding box (scaled)
    if (rotatedX < -halfW || rotatedX > halfW || rotatedY < -halfH || rotatedY > halfH)
    {
        return false;
    }

    // Check component editors for pixel-precise hit testing
    const auto& components = obj->GetComponents();
    auto& registry = Deki::ComponentRegistry::Instance();

    // Component HitTest and texture-alpha sampling work in unscaled framebuffer
    // pixels (same units as displayWidth/displayHeight). rotatedX/Y are world
    // meters, so convert: meters * PPM / scale -> unscaled local pixels.
    float unscaledX = rotatedX * ppm / obj->GetWorldScaleX();
    float unscaledY = rotatedY * ppm / obj->GetWorldScaleY();

    for (Deki::Component* comp : components)
    {
        const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());
        if (!meta || !meta->name)
            continue;

        CustomEditor* editor = CustomEditorRegistry::Instance().GetEditorForComponent(meta->name, comp);
        if (!editor)
            continue;

        // First check component-specific hit test (e.g., SpriteCSSComponent rounded corners).
        // Guarded so a faulty editor falls through to default-hit (treat as opaque).
        bool componentHit = true;
        Guard(meta->name, [&]
              { componentHit = editor->HitTest(comp, unscaledX, unscaledY, displayWidth, displayHeight); });
        if (!componentHit)
        {
            return false;  // Component says it's transparent at this point
        }

        // Check texture alpha if component provides a texture
        std::string textureGuid;
        Guard(meta->name, [&]
              { textureGuid = editor->GetTextureAssetGuid(comp); });
        if (!textureGuid.empty())
        {
            CachedTexture* tex = m_TextureCache->GetOrLoad(textureGuid);
            if (tex && !tex->pixelData.empty())
            {
                // Convert local coords to UV (0-1 range)
                // U: left (-halfW) = 0, right (+halfW) = 1
                float u = (unscaledX + displayWidth * 0.5f) / displayWidth;
                // V: world Y-up means top (+halfH) = texture V=0, bottom (-halfH) = V=1
                float v = (displayHeight * 0.5f - unscaledY) / displayHeight;

                // Sample alpha - if transparent, don't hit
                uint8_t alpha = tex->SampleAlpha(u, v);
                if (alpha < 32)  // Threshold for "transparent enough"
                {
                    return false;
                }
            }
        }

        // Found a visible component at this point
        return true;
    }

    // No component editor found - use bounding box (assume opaque)
    return true;
}

Deki::Object* SceneSelection::HandleClick(const std::vector<Deki::Object*>& objects,
                                        Deki::Object* currentSelection,
                                        float worldX, float worldY, float pixelsPerMeter)
{
    // Collect every object under the click point, outermost first (render order:
    // back-to-front). Containers/parents render behind their content, so iterating
    // forward yields the outermost object first and drills inward on repeated clicks.
    std::vector<Deki::Object*> hits;
    for (auto* obj : objects)
    {
        if (!obj->IsActive())
            continue;

        if (HitTestObject(obj, worldX, worldY, pixelsPerMeter))
        {
            hits.push_back(obj);
        }
    }

    if (hits.empty())
        return nullptr;

    // If the currently-selected object is one of the hits, advance to the next
    // (drilling inward: outermost -> innermost -> wrap back to outermost).
    if (currentSelection)
    {
        auto it = std::find(hits.begin(), hits.end(), currentSelection);
        if (it != hits.end())
        {
            size_t idx = static_cast<size_t>(it - hits.begin());
            return hits[(idx + 1) % hits.size()];
        }
    }

    return hits.front();
}

}  // namespace DekiEditor
