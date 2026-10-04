#pragma once

// Immediate-mode node-graph widget: a pan/zoom canvas with nodes, labelled
// pins and bezier links. It handles link dragging and selection, and reports
// context-menu and delete events.
//
// The canvas is only a view. The caller owns the graph and passes it in every
// frame; the canvas keeps only view state (pan, zoom, drags in progress) and
// reports what the user did through NodeCanvasEvents. It draws only through
// EditorUI, because it lives in deki-editor.dll and is called across the DLL
// boundary. It knows nothing about node types, documents or undo: the window
// hosting it turns events into commands.
//
// Coordinates: node x/y are graph units (css px at zoom 1). The canvas applies
// zoom and the DPI scale to geometry, and sizes text with DrawTextCss so labels
// match widget text at any DPI.

#include <cstdint>

#include <deki-editor/EditorExport.h>
namespace DekiEditor
{

/// One node to draw. Pin labels are for display; events report pin indices.
struct NodeCanvasNode
{
    uint32_t id = 0;           // caller's node id (nonzero)
    float x = 0.0f, y = 0.0f;  // graph-space top-left
    const char* title = "";
    const char* const* inputLabels = nullptr;  // may be null when inputCount == 0
    int inputCount = 0;
    const char* const* outputLabels = nullptr;
    int outputCount = 0;  // caller resolves dynamic pin counts
    bool selected = false;
    // Display hint: the node has something to descend into, so the canvas
    // marks it as enterable. The canvas only reports the double-click; the
    // caller decides what "inside" means.
    bool hasSubgraph = false;
};

/// One link to draw, by node ids and pin indices.
struct NodeCanvasLink
{
    uint32_t fromNode = 0;
    int fromPin = 0;
    uint32_t toNode = 0;
    int toPin = 0;
    bool selected = false;
};

/// Canvas colours, packed as EditorUI::Rgba does: a<<24 | b<<16 | g<<8 | r.
struct NodeCanvasPalette
{
    uint32_t background = 0xFF25201Eu;          // rgb(30, 32, 37)
    uint32_t grid = 0xFF342D2Au;                // rgb(42, 45, 52)
    uint32_t nodeBody = 0xF03C3430u;            // rgba(48, 52, 60, 240)
    uint32_t nodeBorder = 0xFF6C605Au;          // rgb(90, 96, 108)
    uint32_t nodeBorderSelected = 0xFFD29A4Fu;  // rgb(79, 154, 210) accent
    uint32_t title = 0xFFECE8E6u;               // rgb(230, 232, 236)
    uint32_t pinLabel = 0xFFB4ACA6u;            // rgb(166, 172, 180)
    uint32_t pin = 0xFF5AB4DCu;                 // rgb(220, 180, 90)
    uint32_t pinHovered = 0xFF8CD2F0u;          // rgb(240, 210, 140)
    uint32_t link = 0xFFCDBEB4u;                // rgb(180, 190, 205)
    uint32_t linkSelected = 0xFFD29A4Fu;        // accent
    uint32_t linkDrag = 0xFFD29A4Fu;            // rubber-band while connecting
};

/// What happened this frame. Every field is false or zero unless its event fired.
struct NodeCanvasEvents
{
    bool nodeClicked = false;  // left-click on a node body
    uint32_t node = 0;

    bool nodeActivated = false;  // left DOUBLE-click on a node body (descend into it)
    uint32_t activatedNode = 0;

    bool linkClicked = false;  // left-click near a link curve
    int linkIndex = -1;

    bool backgroundClicked = false;  // left-click on empty space (deselect)

    bool nodeMoving = false;  // fires every frame of a node drag (live position)
    uint32_t movedNode = 0;
    float newX = 0.0f, newY = 0.0f;

    bool nodeMoveEnded = false;  // fires once on release; start* = position at drag begin
    uint32_t moveEndedNode = 0;
    float startX = 0.0f, startY = 0.0f;

    bool linkCreated = false;  // released an output-pin drag over an input pin
    uint32_t fromNode = 0;
    int fromPin = 0;
    uint32_t toNode = 0;
    int toPin = 0;

    bool contextMenu = false;  // right-click on empty space (open add-node menu)
    float graphX = 0.0f, graphY = 0.0f;

    bool deleteRequested = false;  // Del pressed while the canvas is hovered
};

class DEKI_EDITOR_API NodeCanvas
{
public:
    /// Reserves width x height at the cursor and draws the graph. `nodes` and
    /// `links` only need to live for the call. `out` is reset, then filled with
    /// this frame's events. Node positions are read only: apply nodeMoving to
    /// your model each frame to drag live.
    void Draw(const char* id, float width, float height, const NodeCanvasNode* nodes, int nodeCount,
              const NodeCanvasLink* links, int linkCount, NodeCanvasEvents& out);

    /// Frames all content on the next Draw. Call after opening a graph.
    void FocusContent() { m_FocusRequested = true; }

    // View state, saved with the session.
    float GetPanX() const { return m_PanX; }
    float GetPanY() const { return m_PanY; }
    float GetZoom() const { return m_Zoom; }
    void SetView(float panX, float panY, float zoom);

    void SetPalette(const NodeCanvasPalette& palette) { m_Palette = palette; }

    /// Node size in graph units, for layout outside Draw (the window uses it to
    /// centre new nodes on the cursor).
    static float NodeWidth(const NodeCanvasNode& n);
    static float NodeHeight(const NodeCanvasNode& n);

private:
    struct PinRef
    {
        int nodeIndex = -1;  // index into this frame's nodes array
        int pin = -1;
        bool isOutput = false;
    };

    static void PinGraphPos(const NodeCanvasNode& n, int pin, bool output, float* gx, float* gy);
    // graph-space -> screen-space (s = zoom * dpi)
    void ToScreen(float originX, float originY, float s, float gx, float gy, float* sx, float* sy) const;

    PinRef HitTestPin(float originX, float originY, float s, const NodeCanvasNode* nodes, int nodeCount, float mx,
                      float my) const;
    int HitTestNode(float originX, float originY, float s, const NodeCanvasNode* nodes, int nodeCount, float mx,
                    float my) const;
    int HitTestLink(float originX, float originY, float s, const NodeCanvasNode* nodes, int nodeCount,
                    const NodeCanvasLink* links, int linkCount, float mx, float my) const;

    void DrawLinkCurve(float p0x, float p0y, float p3x, float p3y, uint32_t color, float thickness) const;
    const NodeCanvasNode* FindNode(const NodeCanvasNode* nodes, int nodeCount, uint32_t id) const;

    // Viewport transform: screen = origin + pan + graph * zoom * dpi.
    float m_Zoom = 1.0f;
    float m_PanX = 0.0f;
    float m_PanY = 0.0f;

    // Interaction state.
    uint32_t m_DragNodeId = 0;  // nonzero while dragging a node
    float m_DragStartX = 0.0f;  // node graph position at drag begin
    float m_DragStartY = 0.0f;
    bool m_Panning = false;
    bool m_LinkDragActive = false;
    uint32_t m_LinkFromNode = 0;
    int m_LinkFromPin = 0;

    bool m_FocusRequested = false;

    NodeCanvasPalette m_Palette;
};

}  // namespace DekiEditor
