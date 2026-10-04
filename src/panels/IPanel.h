#pragma once

namespace DekiEditor
{

// Base of every dockable editor panel. EditorApp draws the panels and
// restores and saves their open state through this interface.
class IPanel
{
public:
    virtual ~IPanel() = default;

    virtual void Draw() = 0;

    // Settings key and crash-guard name: "Hierarchy", "Console", ...
    virtual const char* Id() const = 0;

    // Visible before any saved layout is applied.
    virtual bool OpenByDefault() const { return false; }

    // Whether the open state is saved with the layout.
    virtual bool PersistOpenState() const { return true; }

    bool IsOpen() const { return isOpen; }
    void SetOpen(bool open) { isOpen = open; }

    // Public: the Window menu and the layout reset write it directly.
    bool isOpen = false;
};

}  // namespace DekiEditor
