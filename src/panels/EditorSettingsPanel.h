#pragma once

#include "IPanel.h"

namespace DekiEditor
{

class EditorApp;

// Preferences for the editor itself, not the project: motion, text, updates
// and the editor log. Each change is saved at once to the editor's settings
// file; the ones that only take effect at startup say so on their row.
class EditorSettingsPanel : public IPanel
{
public:
    const char* Id() const override { return "EditorSettings"; }
    bool PersistOpenState() const override { return false; }
    explicit EditorSettingsPanel(EditorApp* app)
        : m_App(app)
    {
    }

    void Draw() override;

private:
    EditorApp* m_App;
};

}  // namespace DekiEditor
