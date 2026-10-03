#include <deki-editor/NodeCanvas.h>
#include <deki-editor/EditorUI.h>

#include <cmath>

namespace DekiEditor
{

namespace
{
// Layout constants, in graph units (css px at zoom 1).
constexpr float kNodeWidth = 160.0f;
constexpr float kHeaderH = 26.0f;
constexpr float kRowH = 20.0f;
constexpr float kBottomPad = 6.0f;
constexpr float kPinRadius = 5.0f;
constexpr float kPinHitPad = 4.0f;    // extra hit radius around a pin
constexpr float kLinkHitDist = 6.0f;  // screen px (x dpi) distance to select a link
constexpr float kGridStep = 32.0f;
constexpr float kTitleCss = 13.0f;
constexpr float kPinLabelCss = 11.0f;

inline float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Cubic bezier point at t in [0,1].
inline void Bezier(float p0x, float p0y, float p1x, float p1y,
                   float p2x, float p2y, float p3x, float p3y,
                   float t, float* ox, float* oy)
{
    const float u = 1.0f - t;
    const float w0 = u * u * u;
    const float w1 = 3.0f * u * u * t;
    const float w2 = 3.0f * u * t * t;
    const float w3 = t * t * t;
    *ox = w0 * p0x + w1 * p1x + w2 * p2x + w3 * p3x;
    *oy = w0 * p0y + w1 * p1y + w2 * p2y + w3 * p3y;
}

// Classic node-editor S-curve handles for a link from p0 to p3.
inline void LinkHandles(float p0x, float p3x, float* p1x, float* p2x)
{
    const float handle = std::fmax(40.0f, std::fabs(p3x - p0x) * 0.5f);
    *p1x = p0x + handle;
    *p2x = p3x - handle;
}

inline float DistToSegmentSq(float px, float py, float ax, float ay, float bx, float by)
{
    const float abx = bx - ax, aby = by - ay;
    const float apx = px - ax, apy = py - ay;
    const float lenSq = abx * abx + aby * aby;
    float t = lenSq > 0.0f ? (apx * abx + apy * aby) / lenSq : 0.0f;
    t = Clampf(t, 0.0f, 1.0f);
    const float cx = ax + t * abx - px;
    const float cy = ay + t * aby - py;
    return cx * cx + cy * cy;
}
}  // namespace

float NodeCanvas::NodeWidth(const NodeCanvasNode& /*n*/)
{
    return kNodeWidth;
}

float NodeCanvas::NodeHeight(const NodeCanvasNode& n)
{
    const int rows = n.inputCount > n.outputCount ? n.inputCount : n.outputCount;
    return kHeaderH + (rows > 0 ? rows * kRowH : 0.0f) + kBottomPad;
}

void NodeCanvas::PinGraphPos(const NodeCanvasNode& n, int pin, bool output,
                             float* gx, float* gy)
{
    *gx = output ? n.x + kNodeWidth : n.x;
    *gy = n.y + kHeaderH + kRowH * (static_cast<float>(pin) + 0.5f);
}

void NodeCanvas::ToScreen(float originX, float originY, float s,
                          float gx, float gy, float* sx, float* sy) const
{
    *sx = originX + m_PanX + gx * s;
    *sy = originY + m_PanY + gy * s;
}

void NodeCanvas::SetView(float panX, float panY, float zoom)
{
    m_PanX = panX;
    m_PanY = panY;
    m_Zoom = Clampf(zoom, 0.25f, 3.0f);
}

const NodeCanvasNode* NodeCanvas::FindNode(const NodeCanvasNode* nodes, int nodeCount,
                                           uint32_t id) const
{
    for (int i = 0; i < nodeCount; ++i)
        if (nodes[i].id == id)
            return &nodes[i];
    return nullptr;
}

NodeCanvas::PinRef NodeCanvas::HitTestPin(float originX, float originY, float s,
                                          const NodeCanvasNode* nodes, int nodeCount,
                                          float mx, float my) const
{
    const float hit = (kPinRadius + kPinHitPad) * s;
    const float hitSq = hit * hit;
    for (int i = nodeCount - 1; i >= 0; --i)
    {
        const NodeCanvasNode& n = nodes[i];
        for (int side = 0; side < 2; ++side)
        {
            const bool output = side == 1;
            const int count = output ? n.outputCount : n.inputCount;
            for (int p = 0; p < count; ++p)
            {
                float gx, gy, sx, sy;
                PinGraphPos(n, p, output, &gx, &gy);
                ToScreen(originX, originY, s, gx, gy, &sx, &sy);
                const float dx = mx - sx, dy = my - sy;
                if (dx * dx + dy * dy <= hitSq)
                    return { i, p, output };
            }
        }
    }
    return {};
}

int NodeCanvas::HitTestNode(float originX, float originY, float s,
                            const NodeCanvasNode* nodes, int nodeCount,
                            float mx, float my) const
{
    // Topmost wins: nodes draw in array order, so hit-test from the end.
    for (int i = nodeCount - 1; i >= 0; --i)
    {
        const NodeCanvasNode& n = nodes[i];
        float x0, y0, x1, y1;
        ToScreen(originX, originY, s, n.x, n.y, &x0, &y0);
        ToScreen(originX, originY, s, n.x + NodeWidth(n), n.y + NodeHeight(n), &x1, &y1);
        if (mx >= x0 && mx <= x1 && my >= y0 && my <= y1)
            return i;
    }
    return -1;
}

int NodeCanvas::HitTestLink(float originX, float originY, float s,
                            const NodeCanvasNode* nodes, int nodeCount,
                            const NodeCanvasLink* links, int linkCount,
                            float mx, float my) const
{
    auto& ui = EditorUI::Get();
    const float maxDist = kLinkHitDist * ui.GetDpiScale();
    const float maxDistSq = maxDist * maxDist;

    for (int l = 0; l < linkCount; ++l)
    {
        const NodeCanvasNode* from = FindNode(nodes, nodeCount, links[l].fromNode);
        const NodeCanvasNode* to = FindNode(nodes, nodeCount, links[l].toNode);
        if (!from || !to)
            continue;

        float gx, gy, p0x, p0y, p3x, p3y;
        PinGraphPos(*from, links[l].fromPin, true, &gx, &gy);
        ToScreen(originX, originY, s, gx, gy, &p0x, &p0y);
        PinGraphPos(*to, links[l].toPin, false, &gx, &gy);
        ToScreen(originX, originY, s, gx, gy, &p3x, &p3y);

        float p1x, p2x;
        LinkHandles(p0x, p3x, &p1x, &p2x);

        const int segments = 24;
        float px = p0x, py = p0y;
        for (int i = 1; i <= segments; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(segments);
            float cx, cy;
            Bezier(p0x, p0y, p1x, p0y, p2x, p3y, p3x, p3y, t, &cx, &cy);
            if (DistToSegmentSq(mx, my, px, py, cx, cy) <= maxDistSq)
                return l;
            px = cx;
            py = cy;
        }
    }
    return -1;
}

void NodeCanvas::DrawLinkCurve(float p0x, float p0y, float p3x, float p3y,
                               uint32_t color, float thickness) const
{
    auto& ui = EditorUI::Get();

    float p1x, p2x;
    LinkHandles(p0x, p3x, &p1x, &p2x);

    const int segments = 24;
    float px = p0x, py = p0y;
    for (int i = 1; i <= segments; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        float cx, cy;
        Bezier(p0x, p0y, p1x, p0y, p2x, p3y, p3x, p3y, t, &cx, &cy);
        ui.DrawLine(px, py, cx, cy, color, thickness);
        px = cx;
        py = cy;
    }
}

void NodeCanvas::Draw(const char* id, float width, float height,
                      const NodeCanvasNode* nodes, int nodeCount,
                      const NodeCanvasLink* links, int linkCount,
                      NodeCanvasEvents& out)
{
    out = NodeCanvasEvents{};

    auto& ui = EditorUI::Get();

    if (width <= 0.0f) width = 100.0f;
    if (height <= 0.0f) height = 100.0f;

    const float dpi = ui.GetDpiScale();
    float s = m_Zoom * dpi;

    float originX, originY;
    ui.GetCursorScreenPos(&originX, &originY);

    // Frame all content when requested (after opening a graph).
    if (m_FocusRequested)
    {
        m_FocusRequested = false;
        if (nodeCount > 0)
        {
            float minX = nodes[0].x, minY = nodes[0].y;
            float maxX = minX, maxY = minY;
            for (int i = 0; i < nodeCount; ++i)
            {
                minX = std::fmin(minX, nodes[i].x);
                minY = std::fmin(minY, nodes[i].y);
                maxX = std::fmax(maxX, nodes[i].x + NodeWidth(nodes[i]));
                maxY = std::fmax(maxY, nodes[i].y + NodeHeight(nodes[i]));
            }
            // Fit it all, with a margin: zoom out as far as the content needs
            // (down to the wheel's 25 %), never in past 100 %. Always 100 %
            // centred the content and left a wide graph spilling off both
            // sides, which also made "Frame all" look like it did nothing.
            const float margin = 40.0f * dpi;
            const float contentW = (maxX - minX) * dpi;
            const float contentH = (maxY - minY) * dpi;
            float fit = 1.0f;
            if (contentW > 0.0f && contentH > 0.0f)
                fit = std::fmin((width - margin * 2.0f) / contentW, (height - margin * 2.0f) / contentH);
            m_Zoom = Clampf(fit, 0.25f, 1.0f);
            s = m_Zoom * dpi;
            m_PanX = width * 0.5f - (minX + maxX) * 0.5f * s;
            m_PanY = height * 0.5f - (minY + maxY) * 0.5f * s;
        }
        else
        {
            m_Zoom = 1.0f;
            s = dpi;
            m_PanX = 0.0f;
            m_PanY = 0.0f;
        }
    }

    // Reserve the region and capture input over it (left/middle/right).
    // Allow-overlap so anything drawn on top of the canvas LATER in the frame
    // (the live preview overlay and its transport buttons) wins the hit test:
    // this surface spans the whole canvas, so without it it claims the hovered
    // id first and every control above it is dead to the mouse.
    ui.SetNextItemAllowOverlap();
    ui.InvisibleButton(id, width, height, true, true, true);
    const bool hovered = ui.IsItemHovered();

    float mx, my;
    ui.GetMousePos(&mx, &my);

    // ----- Drawing -----
    ui.DrawRectFilled(originX, originY, originX + width, originY + height,
                      m_Palette.background, 0.0f);

    ui.PushClipRect(originX, originY, originX + width, originY + height, true);

    // Grid (graph-space, pans/zooms with content).
    const float step = kGridStep * s;
    if (step >= 6.0f)
    {
        float startX = std::fmod(m_PanX, step);
        for (float gx = startX; gx < width; gx += step)
            ui.DrawLine(originX + gx, originY, originX + gx, originY + height, m_Palette.grid, 1.0f);
        float startY = std::fmod(m_PanY, step);
        for (float gy = startY; gy < height; gy += step)
            ui.DrawLine(originX, originY + gy, originX + width, originY + gy, m_Palette.grid, 1.0f);
    }

    // Links under nodes.
    for (int l = 0; l < linkCount; ++l)
    {
        const NodeCanvasNode* from = FindNode(nodes, nodeCount, links[l].fromNode);
        const NodeCanvasNode* to = FindNode(nodes, nodeCount, links[l].toNode);
        if (!from || !to)
            continue;

        float gx, gy, p0x, p0y, p3x, p3y;
        PinGraphPos(*from, links[l].fromPin, true, &gx, &gy);
        ToScreen(originX, originY, s, gx, gy, &p0x, &p0y);
        PinGraphPos(*to, links[l].toPin, false, &gx, &gy);
        ToScreen(originX, originY, s, gx, gy, &p3x, &p3y);

        DrawLinkCurve(p0x, p0y, p3x, p3y,
                      links[l].selected ? m_Palette.linkSelected : m_Palette.link,
                      2.0f * dpi);
    }

    // Rubber band while dragging a new link.
    if (m_LinkDragActive)
    {
        const NodeCanvasNode* from = FindNode(nodes, nodeCount, m_LinkFromNode);
        if (from && m_LinkFromPin < from->outputCount)
        {
            float gx, gy, p0x, p0y;
            PinGraphPos(*from, m_LinkFromPin, true, &gx, &gy);
            ToScreen(originX, originY, s, gx, gy, &p0x, &p0y);
            DrawLinkCurve(p0x, p0y, mx, my, m_Palette.linkDrag, 2.0f * dpi);
        }
        else
        {
            m_LinkDragActive = false;  // source vanished mid-drag
        }
    }

    // Nodes (array order; last = topmost).
    const PinRef hoverPin = hovered
                                ? HitTestPin(originX, originY, s, nodes, nodeCount, mx, my)
                                : PinRef{};
    for (int i = 0; i < nodeCount; ++i)
    {
        const NodeCanvasNode& n = nodes[i];

        float x0, y0, x1, y1;
        ToScreen(originX, originY, s, n.x, n.y, &x0, &y0);
        ToScreen(originX, originY, s, n.x + NodeWidth(n), n.y + NodeHeight(n), &x1, &y1);

        ui.DrawRectFilled(x0, y0, x1, y1, m_Palette.nodeBody, 6.0f * s);
        ui.DrawRect(x0, y0, x1, y1,
                    n.selected ? m_Palette.nodeBorderSelected : m_Palette.nodeBorder,
                    n.selected ? 2.0f * dpi : 1.5f * dpi, 6.0f * s);

        // Title, vertically centered in the header band.
        {
            float tw, th;
            ui.MeasureTextCss(kTitleCss * m_Zoom, n.title, &tw, &th);
            ui.DrawTextCss(kTitleCss * m_Zoom,
                           x0 + 10.0f * s, y0 + (kHeaderH * s - th) * 0.5f,
                           m_Palette.title, n.title);
        }

        // "Contains a graph" affordance: two nested squares at the header's
        // right edge, the flat-graphics version of a stack of cards. Says
        // there is an inside without saying what kind of inside.
        if (n.hasSubgraph)
        {
            const float box = 9.0f * s;
            const float inset = 3.0f * s;
            const float bx1 = x1 - 10.0f * s;
            const float by0 = y0 + (kHeaderH * s - box) * 0.5f;
            ui.DrawRect(bx1 - box, by0 + inset, bx1 - inset, by0 + box,
                        m_Palette.pinLabel, 1.0f * dpi, 1.0f * s);
            ui.DrawRect(bx1 - box + inset, by0, bx1, by0 + box - inset,
                        m_Palette.pinLabel, 1.0f * dpi, 1.0f * s);
        }

        // Pins + labels.
        for (int side = 0; side < 2; ++side)
        {
            const bool output = side == 1;
            const int count = output ? n.outputCount : n.inputCount;
            const char* const* labels = output ? n.outputLabels : n.inputLabels;
            for (int p = 0; p < count; ++p)
            {
                float gx, gy, sx, sy;
                PinGraphPos(n, p, output, &gx, &gy);
                ToScreen(originX, originY, s, gx, gy, &sx, &sy);

                const bool isHovered = hoverPin.nodeIndex == i &&
                                       hoverPin.pin == p && hoverPin.isOutput == output;
                ui.DrawCircleFilled(sx, sy, kPinRadius * s,
                                    isHovered ? m_Palette.pinHovered : m_Palette.pin);

                const char* label = labels && labels[p] ? labels[p] : "";
                if (label[0] != '\0')
                {
                    float lw, lh;
                    ui.MeasureTextCss(kPinLabelCss * m_Zoom, label, &lw, &lh);
                    const float ly = sy - lh * 0.5f;
                    if (output)
                        ui.DrawTextCss(kPinLabelCss * m_Zoom, sx - 10.0f * s - lw, ly,
                                       m_Palette.pinLabel, label);
                    else
                        ui.DrawTextCss(kPinLabelCss * m_Zoom, sx + 10.0f * s, ly,
                                       m_Palette.pinLabel, label);
                }
            }
        }
    }

    ui.PopClipRect();

    // ----- Interaction -----

    // Zoom to cursor on wheel.
    if (hovered)
    {
        const float wheel = ui.GetMouseWheel();
        if (wheel != 0.0f)
        {
            const float gx = (mx - originX - m_PanX) / s;
            const float gy = (my - originY - m_PanY) / s;
            m_Zoom = Clampf(m_Zoom * (1.0f + wheel * 0.1f), 0.25f, 3.0f);
            s = m_Zoom * dpi;
            m_PanX = mx - originX - gx * s;
            m_PanY = my - originY - gy * s;
        }
    }

    // Left press: pin (start link drag) > node (select + drag) > link > background.
    if (hovered && ui.IsMouseClicked(0))
    {
        m_DragNodeId = 0;
        m_Panning = false;

        const PinRef pin = HitTestPin(originX, originY, s, nodes, nodeCount, mx, my);
        if (pin.nodeIndex >= 0 && pin.isOutput)
        {
            m_LinkDragActive = true;
            m_LinkFromNode = nodes[pin.nodeIndex].id;
            m_LinkFromPin = pin.pin;
        }
        else if (pin.nodeIndex < 0)
        {
            const int nodeIndex = HitTestNode(originX, originY, s, nodes, nodeCount, mx, my);
            if (nodeIndex >= 0)
            {
                m_DragNodeId = nodes[nodeIndex].id;
                m_DragStartX = nodes[nodeIndex].x;
                m_DragStartY = nodes[nodeIndex].y;
                out.nodeClicked = true;
                out.node = nodes[nodeIndex].id;
                // Second click of a double-click: also report activation. The
                // click event still fires (selection follows the cursor either
                // way); the caller decides what descending means.
                if (ui.IsMouseDoubleClicked(0))
                {
                    out.nodeActivated = true;
                    out.activatedNode = nodes[nodeIndex].id;
                    m_DragNodeId = 0;  // don't smear the node while entering it
                }
            }
            else
            {
                const int linkIndex = HitTestLink(originX, originY, s, nodes, nodeCount,
                                                  links, linkCount, mx, my);
                if (linkIndex >= 0)
                {
                    out.linkClicked = true;
                    out.linkIndex = linkIndex;
                }
                else
                {
                    out.backgroundClicked = true;
                    m_Panning = true;
                }
            }
        }
        // Press on an input pin: no action (links are created output -> input).
    }

    // Right press on empty space: context menu at graph position.
    if (hovered && ui.IsMouseClicked(1))
    {
        const PinRef pin = HitTestPin(originX, originY, s, nodes, nodeCount, mx, my);
        const int nodeIndex = pin.nodeIndex < 0
                                  ? HitTestNode(originX, originY, s, nodes, nodeCount, mx, my)
                                  : 0;
        if (pin.nodeIndex < 0 && nodeIndex < 0)
        {
            out.contextMenu = true;
            out.graphX = (mx - originX - m_PanX) / s;
            out.graphY = (my - originY - m_PanY) / s;
        }
    }

    // Drag update.
    if (ui.IsMouseDragging(0, -1.0f) || ui.IsMouseDown(0))
    {
        float dx, dy;
        ui.GetMouseDelta(&dx, &dy);
        if (m_DragNodeId != 0)
        {
            if (const NodeCanvasNode* n = FindNode(nodes, nodeCount, m_DragNodeId))
            {
                if (dx != 0.0f || dy != 0.0f)
                {
                    out.nodeMoving = true;
                    out.movedNode = m_DragNodeId;
                    out.newX = n->x + dx / s;
                    out.newY = n->y + dy / s;
                }
            }
            else
            {
                m_DragNodeId = 0;  // node vanished mid-drag
            }
        }
        else if (m_Panning)
        {
            m_PanX += dx;
            m_PanY += dy;
        }
    }

    // Release.
    if (!ui.IsMouseDown(0))
    {
        if (m_LinkDragActive)
        {
            const PinRef pin = HitTestPin(originX, originY, s, nodes, nodeCount, mx, my);
            if (pin.nodeIndex >= 0 && !pin.isOutput &&
                nodes[pin.nodeIndex].id != m_LinkFromNode)
            {
                out.linkCreated = true;
                out.fromNode = m_LinkFromNode;
                out.fromPin = m_LinkFromPin;
                out.toNode = nodes[pin.nodeIndex].id;
                out.toPin = pin.pin;
            }
            m_LinkDragActive = false;
        }

        if (m_DragNodeId != 0)
        {
            out.nodeMoveEnded = true;
            out.moveEndedNode = m_DragNodeId;
            out.startX = m_DragStartX;
            out.startY = m_DragStartY;
            m_DragNodeId = 0;
        }
        m_Panning = false;
    }

    // Delete key (gated on hover so typing in side panels never deletes nodes).
    if (hovered && ui.IsKeyPressed(EditorUI::Key::Delete, false))
        out.deleteRequested = true;
}

}  // namespace DekiEditor
