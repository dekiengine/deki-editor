#pragma once

#include "IPanel.h"

#include "imgui.h"
#include <memory>
#include <string>
#include <vector>
#include <deki/Engine.h>  // Deki::ColorFormat

class CameraComponent;

namespace DekiEditor
{

class EditorApp;
class EditModeRenderer;

class PlayViewPanel : public IPanel
{
public:
    const char* Id() const override { return "PlayView"; }
    PlayViewPanel(EditorApp* app);
    ~PlayViewPanel();
    void Draw();

private:
    EditorApp* m_App;

    // Renders the camera's view for the preview shown while stopped
    std::unique_ptr<EditModeRenderer> m_PreviewRenderer;

    // What the preview was last rendered from. It is a full software render of
    // the scene (tens of ms), so it runs only when one of these changes or an
    // edit is in progress, as in the Scene view; not on every frame.
    bool m_PreviewStale = true;
    const void* m_PreviewScene = nullptr;
    const void* m_PreviewCamera = nullptr;
    uint64_t m_PreviewGeneration = 0;
    uint64_t m_PreviewAssetEpoch = 0;
    uint8_t m_PreviewClear[3] = {};

    // Canvas position, set during Draw
    ImVec2 m_CanvasTopLeft;
    ImVec2 m_CanvasSize;

    // Play mode drag tracking: input keeps going to the game when the mouse
    // leaves the canvas mid-drag.
    bool m_PlayModeDragging = false;
    bool m_WasHoveringInBounds = false;

    // Touch input: move events go out only while pressed, as on an embedded
    // touchscreen.
    bool m_TouchInputMode = true;

    // Last key state forwarded to the game (one slot per entry of the
    // forward() table in Draw: space/enter/esc/backspace + 4 arrows + A-Z).
    bool m_PlayKeyDown[34] = {};

    // Previewed screen: "free" (Free Aspect, the panel's size), "aspect:W:H"
    // (the largest screen of that shape in the panel), "res:WxH", or a platform
    // id whose screen is used. Pushed to SceneView every frame.
    std::string m_PreviewId = "free";

    // Screen sizes the user added from the picker. Stored per user, for every
    // project, in EditorSettings, like Unity's custom Game view sizes.
    struct UserScreen
    {
        std::string name;  // optional; the size is shown either way
        bool aspect = false;
        int w = 0, h = 0;
        std::string Id() const;
    };
    std::vector<UserScreen> m_UserScreens;
    bool m_UserScreensLoaded = false;
    int m_RemoveUserScreen = -1;
    bool m_OpenAddScreen = false;
    UserScreen m_NewScreen;
    char m_NewScreenName[64] = {};
    void LoadUserScreens();
    void SaveUserScreens() const;
    void DrawAddScreenDialog();
    std::string ScreenLabel(const std::string& id) const;

    // Previewed screen format: a platform's own when one is picked, full
    // colour otherwise; the dropdown changes it.
    Deki::ColorFormat m_PreviewFormat = Deki::ColorFormat::ARGB8888;
    void DrawFormatPicker(float& cursorX, const ImVec2& rowMin, float rowH, float dpi);
    void SelectPreview(const std::string& id);

    // The picks above are remembered per project in EditorSettings: loaded when
    // the open project changes, saved when the user changes one.
    std::string m_PreviewProject;
    void LoadPreview(const std::string& projectPath);
    void SavePreview() const;

    void DrawPreviewPicker(float& cursorX, const ImVec2& rowMin, float rowH, float dpi);

    float m_CanvasDpi = 1.0f;  // the window's DPI scale when the canvas was measured
    void ApplyPreviewSize();
};

}  // namespace DekiEditor
