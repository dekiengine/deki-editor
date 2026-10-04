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

// Manages object and asset selection state.
// Invariant: selecting an object clears all asset selection fields,
//            and selecting an asset clears object selection.
//
// This is the ONLY owner of selection. Anything that needs to observe it -
// notably EditorApplication, the facade package DLLs read through - registers
// via SetOnChanged rather than being updated at each call site. It was the
// latter, and three of the eight mutators forgot: Add/Remove/ToggleSelection
// left EditorApplication::GetActiveObject() pointing at the previously selected
// object, so a Ctrl+click in the Hierarchy (ToggleSelection) desynced every
// package CustomEditor, and the stale pointer dangled once that object was
// deleted. SetSelectedAsset and RemoveObjects clear objects too and were
// likewise missed.
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

    // Invoked after any change to the selection, with the new active object
    // (first selected, or nullptr). Set once at startup.
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
