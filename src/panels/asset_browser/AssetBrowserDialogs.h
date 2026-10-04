#pragma once

#include <string>
#include <memory>

namespace DekiEditor
{

class EditorApp;
class AssetBrowserPanel;

/// Dialog that creates a folder in the asset browser.
class CreateFolderDialog
{
public:
    CreateFolderDialog(AssetBrowserPanel* browser);
    void Draw();
    void Open();

private:
    AssetBrowserPanel* m_Browser;
    bool m_ShouldOpen = false;
    char m_NewFolderName[256] = "";
    std::string m_Error;  // shown under the field; the dialog stays open
};

/// Dialog that creates an asset file (Scene, Animation and so on).
class CreateAssetDialog
{
public:
    CreateAssetDialog(EditorApp* app, AssetBrowserPanel* browser);
    void Draw();
    void Open(const std::string& assetType);

private:
    EditorApp* m_App;
    AssetBrowserPanel* m_Browser;
    bool m_ShouldOpen = false;
    std::string m_CreateAssetType;
    char m_NewAssetName[256] = "";
    std::string m_Error;  // shown under the field; the dialog stays open
};

/// Dialog that confirms deleting assets or folders.
class DeleteConfirmDialog
{
public:
    DeleteConfirmDialog(AssetBrowserPanel* browser);
    void Draw();
    void Open();

private:
    AssetBrowserPanel* m_Browser;
    bool m_ShouldOpen = false;
};

/// Dialog that renames an asset or folder.
class RenameDialog
{
public:
    RenameDialog(AssetBrowserPanel* browser);
    void Draw();
    void Open(int index, const std::string& currentName, bool isDirectory);

private:
    AssetBrowserPanel* m_Browser;
    bool m_ShouldOpen = false;
    int m_RenameIndex = -1;
    char m_RenameBuffer[256] = "";
};

}  // namespace DekiEditor
