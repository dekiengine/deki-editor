#pragma once

#include "IPanel.h"

#include <string>

namespace DekiEditor
{

class EditorApp;

// Panel for capturing play mode frames to PNG sequence
// Used to verify actual FPS by recording and analyzing frame output
class FrameCapturePanel : public IPanel
{
public:
    const char* Id() const override { return "FrameCapture"; }
    FrameCapturePanel(EditorApp* app);
    ~FrameCapturePanel() = default;

    void Draw();

    // UI state

private:
    EditorApp* m_App;

    // Output settings
    std::string m_OutputPath;
    bool m_UseCustomPath = false;
};

}  // namespace DekiEditor
