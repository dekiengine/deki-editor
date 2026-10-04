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
    : m_Measurer(measurer),
      m_TextureCache(textureCache)
{
}

bool SceneSelection::HitTestObject(Deki::Object* obj, float worldX, float worldY, float pixelsPerMeter)
{
    float displayWidth, displayHeight;
    if (!m_Measurer->GetDisplaySize(obj, displayWidth, displayHeight))
    {
        return false;  // objects with no visible size are selected in the Hierarchy
    }

    if (displayWidth <= 0 || displayHeight <= 0)
    {
        return false;
    }

    // GetDisplaySize returns framebuffer pixels, but the click point and the
    // transforms are in world meters, so the size is converted with 1/PPM, as
    // in the gizmo and snap math.
    const float ppm = (pixelsPerMeter > 0.0f) ? pixelsPerMeter : 1.0f;
    const float invPPM = 1.0f / ppm;

    // World transforms, so parents count (meters).
    float halfW = displayWidth * obj->GetWorldScaleX() * 0.5f * invPPM;
    float halfH = displayHeight * obj->GetWorldScaleY() * 0.5f * invPPM;

    // The click point in the object's local space. World rotation is in
    // radians; negated for the inverse rotation.
    float rotRad = -obj->GetWorldRotation();
    float cosR = std::cos(rotRad);
    float sinR = std::sin(rotRad);

    float localX = worldX - obj->GetWorldX();
    float localY = worldY - obj->GetWorldY();

    // Inverse rotation (the transpose): the sin terms change sign.
    float rotatedX = localX * cosR + localY * sinR;
    float rotatedY = -localX * sinR + localY * cosR;

    // The scaled bounding box first.
    if (rotatedX < -halfW || rotatedX > halfW || rotatedY < -halfH || rotatedY > halfH)
    {
        return false;
    }

    // Then the component editors, for a pixel-precise test.
    const auto& components = obj->GetComponents();
    auto& registry = Deki::ComponentRegistry::Instance();

    // Component HitTest and texture alpha work in unscaled framebuffer pixels,
    // like displayWidth/Height. rotatedX/Y are meters: pixels = meters * PPM /
    // scale.
    float unscaledX = rotatedX * ppm / obj->GetWorldScaleX();
    float unscaledY = rotatedY * ppm / obj->GetWorldScaleY();

    for (Deki::Component* comp : components)
    {
        const Deki::ComponentMeta* meta = registry.GetMeta(comp->GetType());
        if (!meta || !meta->name)
        {
            continue;
        }

        CustomEditor* editor = CustomEditorRegistry::Instance().GetEditorForComponent(meta->name, comp);
        if (!editor)
        {
            continue;
        }

        // The component's own hit test first (SpriteCSSComponent's rounded
        // corners, say). Guarded, so a faulty editor counts as opaque.
        bool componentHit = true;
        Guard(meta->name,
              [&] { componentHit = editor->HitTest(comp, unscaledX, unscaledY, displayWidth, displayHeight); });
        if (!componentHit)
        {
            return false;  // transparent at this point
        }

        // Then the texture's alpha, if the component has a texture.
        std::string textureGuid;
        Guard(meta->name, [&] { textureGuid = editor->GetTextureAssetGuid(comp); });
        if (!textureGuid.empty())
        {
            CachedTexture* tex = m_TextureCache->GetOrLoad(textureGuid);
            if (tex && !tex->pixelData.empty())
            {
                // Local position to UV: left edge U=0, right edge U=1.
                float u = (unscaledX + displayWidth * 0.5f) / displayWidth;
                // World Y points up, so the top edge is V=0 and the bottom V=1.
                float v = (displayHeight * 0.5f - unscaledY) / displayHeight;

                uint8_t alpha = tex->SampleAlpha(u, v);
                if (alpha < 32)  // transparent enough to click through
                {
                    return false;
                }
            }
        }

        return true;
    }

    // No component editor: the bounding box decides, as if opaque.
    return true;
}

std::vector<Deki::Object*> SceneSelection::ObjectsInRect(const std::vector<Deki::Object*>& objects, float x0, float y0,
                                                         float x1, float y1, float pixelsPerMeter)
{
    const float minX = std::min(x0, x1), maxX = std::max(x0, x1);
    const float minY = std::min(y0, y1), maxY = std::max(y0, y1);
    const float invPPM = 1.0f / (pixelsPerMeter > 0.0f ? pixelsPerMeter : 1.0f);
    std::vector<Deki::Object*> out;
    for (Deki::Object* obj : objects)
    {
        if (!obj->IsActive())
        {
            continue;
        }
        float w = 0.0f, h = 0.0f;
        if (!m_Measurer->GetDisplaySize(obj, w, h) || w <= 0.0f || h <= 0.0f)
        {
            continue;
        }
        // The drawn box as HitTestObject sees it (centered, scaled, rotated),
        // reduced to its world-aligned bounds.
        const float halfW = std::fabs(w * obj->GetWorldScaleX()) * 0.5f * invPPM;
        const float halfH = std::fabs(h * obj->GetWorldScaleY()) * 0.5f * invPPM;
        const float c = std::fabs(std::cos(obj->GetWorldRotation()));
        const float s = std::fabs(std::sin(obj->GetWorldRotation()));
        const float extX = halfW * c + halfH * s;
        const float extY = halfW * s + halfH * c;
        const float x = obj->GetWorldX(), y = obj->GetWorldY();
        if (x - extX >= minX && x + extX <= maxX && y - extY >= minY && y + extY <= maxY)
        {
            out.push_back(obj);
        }
    }
    return out;
}

Deki::Object* SceneSelection::HandleClick(const std::vector<Deki::Object*>& objects, Deki::Object* currentSelection,
                                          float worldX, float worldY, float pixelsPerMeter)
{
    // Every object under the point, outermost first. The list is in render
    // order, back to front, and parents draw behind their content, so repeated
    // clicks drill inward.
    std::vector<Deki::Object*> hits;
    for (auto* obj : objects)
    {
        if (!obj->IsActive())
        {
            continue;
        }

        if (HitTestObject(obj, worldX, worldY, pixelsPerMeter))
        {
            hits.push_back(obj);
        }
    }

    if (hits.empty())
    {
        return nullptr;
    }

    // If the selected object is one of the hits, take the next one inward,
    // wrapping back to the outermost.
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
