#pragma once

#include "IPanel.h"

#include <string>

namespace DekiEditor
{

class EditorApp;

// Records Play mode frames as a PNG sequence, to check the real frame rate.
class FrameCapturePanel : public IPanel
{
public:
    const char* Id() const override { return "FrameCapture"; }
    FrameCapturePanel(EditorApp* app);
    ~FrameCapturePanel() = default;

    void Draw();

private:
    EditorApp* m_App;

    // Output settings
    std::string m_OutputPath;
    bool m_UseCustomPath = false;
};

}  // namespace DekiEditor
