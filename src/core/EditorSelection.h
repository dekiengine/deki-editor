#pragma once

#include <functional>
#include <string>
#include <vector>
#include <algorithm>

namespace Deki
{
class Object;
}

namespace DekiEditor
{

// Object and asset selection.
// Invariant: selecting an object clears the asset selection, and selecting an
// asset clears the object selection.
//
// This is the only owner of selection. Anything that observes it, such as
// EditorApplication (which package DLLs read through), registers with
// SetOnChanged. Every mutator must call NotifyChanged; updating observers at
// each call site instead leaves them holding stale, possibly deleted, objects.
class EditorSelection
{
public:
    // Object selection
    const std::vector<Deki::Object*>& GetSelectedObjects() const { return m_SelectedObjects; }
    Deki::Object* GetSelectedObject() const { return m_SelectedObjects.empty() ? nullptr : m_SelectedObjects[0]; }
    bool IsSelected(Deki::Object* obj) const;
    size_t GetSelectionCount() const { return m_SelectedObjects.size(); }
    bool HasMultipleSelected() const { return m_SelectedObjects.size() > 1; }

    void SetSelectedObject(Deki::Object* obj);
    void SetSelectedObjects(const std::vector<Deki::Object*>& objects);
    void AddToSelection(Deki::Object* obj);
    void RemoveFromSelection(Deki::Object* obj);
    void RemoveObjects(const std::vector<Deki::Object*>& objs);
    void ToggleSelection(Deki::Object* obj);
    void ClearObjectSelection();

    /// Called after any change to the selection, with the new active object
    /// (the first selected, or nullptr). Set once at startup.
    void SetOnChanged(std::function<void(Deki::Object*)> cb) { m_OnChanged = std::move(cb); }

    // Asset selection
    const std::string& GetSelectedAssetPath() const { return m_SelectedAssetPath; }
    const std::string& GetSelectedAssetCachePath() const { return m_SelectedAssetCachePath; }
    bool GetSelectedAssetHasCached() const { return m_SelectedAssetHasCached; }
    const std::string& GetSelectedAssetGuid() const { return m_SelectedAssetGuid; }
    bool HasAssetSelection() const;

    void SetSelectedAsset(const std::string& path, const std::string& cachePath, bool hasCached,
                          const std::string& guid = "");

private:
    void ClearAssetSelection();
    void NotifyChanged() const;

    std::function<void(Deki::Object*)> m_OnChanged;

    std::vector<Deki::Object*> m_SelectedObjects;
    std::string m_SelectedAssetPath;
    std::string m_SelectedAssetCachePath;
    bool m_SelectedAssetHasCached = false;
    std::string m_SelectedAssetGuid;
};

}  // namespace DekiEditor
