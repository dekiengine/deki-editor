#include "EditorSelection.h"

#include <unordered_set>

namespace DekiEditor
{

bool EditorSelection::IsSelected(Deki::Object* obj) const
{
    if (!obj) return false;
    return std::find(m_SelectedObjects.begin(), m_SelectedObjects.end(), obj) != m_SelectedObjects.end();
}

void EditorSelection::SetSelectedObject(Deki::Object* obj)
{
    m_SelectedObjects.clear();
    if (obj)
        m_SelectedObjects.push_back(obj);
    ClearAssetSelection();
    NotifyChanged();
}

void EditorSelection::SetSelectedObjects(const std::vector<Deki::Object*>& objects)
{
    m_SelectedObjects = objects;
    ClearAssetSelection();
    NotifyChanged();
}

void EditorSelection::AddToSelection(Deki::Object* obj)
{
    if (!obj) return;
    if (!IsSelected(obj))
    {
        m_SelectedObjects.push_back(obj);
        ClearAssetSelection();
        NotifyChanged();
    }
}

void EditorSelection::RemoveFromSelection(Deki::Object* obj)
{
    if (!obj) return;
    auto it = std::find(m_SelectedObjects.begin(), m_SelectedObjects.end(), obj);
    if (it != m_SelectedObjects.end())
    {
        m_SelectedObjects.erase(it);
        NotifyChanged();
    }
}

void EditorSelection::RemoveObjects(const std::vector<Deki::Object*>& objs)
{
    if (objs.empty() || m_SelectedObjects.empty()) return;
    std::unordered_set<Deki::Object*> dying(objs.begin(), objs.end());
    m_SelectedObjects.erase(
        std::remove_if(m_SelectedObjects.begin(), m_SelectedObjects.end(),
                       [&dying](Deki::Object* o)
                       { return dying.count(o) > 0; }),
        m_SelectedObjects.end());
    NotifyChanged();
}

void EditorSelection::ToggleSelection(Deki::Object* obj)
{
    if (!obj) return;
    if (IsSelected(obj))
        RemoveFromSelection(obj);
    else
        AddToSelection(obj);
    ClearAssetSelection();
    NotifyChanged();
}

void EditorSelection::ClearObjectSelection()
{
    m_SelectedObjects.clear();
    NotifyChanged();
}

bool EditorSelection::HasAssetSelection() const
{
    if (!m_SelectedAssetPath.empty())
        return true;
    // Baked cache-only assets have no path but have cache data
    return m_SelectedAssetPath.empty() && m_SelectedAssetHasCached && !m_SelectedAssetCachePath.empty();
}

void EditorSelection::SetSelectedAsset(const std::string& path, const std::string& cachePath,
                                       bool hasCached, const std::string& guid)
{
    m_SelectedAssetPath = path;
    m_SelectedAssetCachePath = cachePath;
    m_SelectedAssetHasCached = hasCached;
    m_SelectedAssetGuid = guid;
    m_SelectedObjects.clear();
    NotifyChanged();
}

void EditorSelection::NotifyChanged() const
{
    if (m_OnChanged)
        m_OnChanged(GetSelectedObject());
}

void EditorSelection::ClearAssetSelection()
{
    m_SelectedAssetPath.clear();
    m_SelectedAssetCachePath.clear();
    m_SelectedAssetHasCached = false;
    m_SelectedAssetGuid.clear();
}

}  // namespace DekiEditor
