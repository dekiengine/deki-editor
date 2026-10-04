// Dock splitter snapping.
//
// When the user drags a docked split separator, ImGui's
// DockNodeTreeUpdateSplitter computes the two adjacent child sizes from the
// mouse delta. A tiny hook patched into that function ("Deki patch E" in
// CMakeLists.txt) calls through the GDekiDockSnapFn function pointer BEFORE the
// sizes are applied, letting us nudge the proposed separator so it aligns with a
// nearby dock node edge along the same axis. That makes panel heights/widths
// line up across columns (e.g. the Hierarchy/Asset Browser split snapping to the
// Console's top edge).
//
// The pointer is defined (null) inside imgui.cpp itself, so every binary that
// links imgui gets a harmless default and needs no editor symbol. Only this
// translation unit (compiled directly into DekiEditor.exe, which is the binary
// that runs the DockSpace) assigns it, at static-init time.

#include "imgui.h"
#include "imgui_internal.h"

// Matches the typedef + global the CMake patch injects into imgui.cpp
// (global namespace, null by default).
typedef void (*DekiDockSnapFn)(ImGuiDockNode*, ImGuiDockNode*, ImGuiDockNode*, ImGuiAxis, float*, float*, float, float);
extern DekiDockSnapFn GDekiDockSnapFn;

namespace
{

void SnapDockSplitter(ImGuiDockNode* node, ImGuiDockNode* child0, ImGuiDockNode* child1, ImGuiAxis axis, float* size0,
                      float* size1, float minSize0, float minSize1)
{
    if (!node || !child0 || !child1 || !size0 || !size1)
    {
        return;
    }

    ImGuiContext& g = *GImGui;

    // Snap zone scales with the (DPI-scaled) font size: ~8px at 1x, ~16px at 2x.
    const float snapDist = g.FontSize * 0.6f;
    const float total = *size0 + *size1;  // span shared by both children
    const float spacing = g.Style.DockingSeparatorSize;

    // The panel BELOW the separator (child_1) is the one whose tab top moves with
    // the drag. It sits one separator-thickness below the separator line, so we
    // align THAT tab top - not the separator - with other panels' tab tops.
    // Aligning the separator instead would leave the tabs offset by the gap.
    const float movingTabTop = child0->Pos[axis] + *size0 + spacing;

    // Snap only to the NEAR edge of other dock nodes - the top of a node, where
    // its tab bar sits. The far/bottom edge is intentionally not a target.
    float best = movingTabTop;
    float bestDist = snapDist;
    ImVector<ImGuiDockNode*> stack;
    stack.push_back(ImGui::DockNodeGetRootNode(node));
    while (stack.Size > 0)
    {
        ImGuiDockNode* n = stack.back();
        stack.pop_back();
        if (n == NULL)
        {
            continue;
        }
        if (n->ChildNodes[0])
        {
            stack.push_back(n->ChildNodes[0]);
        }
        if (n->ChildNodes[1])
        {
            stack.push_back(n->ChildNodes[1]);
        }

        // Skip everything inside the region being resized: child_0, child_1 and
        // all their descendants move WITH the separator, so their edges (e.g. a
        // nested panel's tab sitting just under the separator) are not stable
        // alignment targets. Only panels OUTSIDE this split count.
        bool insideResizedRegion = false;
        for (ImGuiDockNode* p = n; p != NULL; p = p->ParentNode)
        {
            if (p == node)
            {
                insideResizedRegion = true;
                break;
            }
        }
        if (insideResizedRegion)
        {
            continue;
        }

        // Only real tabbed panels (leaf nodes that actually host windows), and
        // only their top edge - where the tab bar sits, never the bottom.
        if (!n->IsLeafNode() || n->Windows.Size == 0)
        {
            continue;
        }

        const float topEdge = n->Pos[axis];
        const float dTop = ImFabs(topEdge - movingTabTop);
        if (dTop < bestDist)
        {
            bestDist = dTop;
            best = topEdge;
        }
    }

    if (best == movingTabTop)
    {
        return;  // nothing within the snap zone
    }

    // Solve for child_0's size so child_1's tab top lands on the target:
    //   child_0->Pos + size_0 + spacing == best  ->  size_0 == best - spacing - Pos.
    // Clamp to the same min sizes ImGui already enforces so we never collapse a node.
    const float snapped0 = ImClamp(best - spacing - child0->Pos[axis], minSize0, total - minSize1);
    *size0 = snapped0;
    *size1 = total - snapped0;
}

// Install the hook at static-init time. DockSnap.cpp is a direct source of
// DekiEditor.exe (not a static-lib member), so this object is always linked and
// this initializer always runs - before the first frame draws the DockSpace.
const bool kDockSnapInstalled = []
{
    GDekiDockSnapFn = &SnapDockSplitter;
    return true;
}();

}  // namespace
