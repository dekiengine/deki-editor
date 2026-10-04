// Dock splitter snapping.
//
// When the user drags a docked split separator, ImGui's
// DockNodeTreeUpdateSplitter computes the two adjacent child sizes from the
// mouse delta. A small hook patched into that function ("Deki patch E" in
// CMakeLists.txt) calls the GDekiDockSnapFn function pointer before the sizes
// are applied, so we can nudge the separator to line up with a nearby dock
// node edge on the same axis. Panel heights and widths then line up across
// columns (e.g. the Hierarchy/Asset Browser split snapping to the Console's
// top edge).
//
// The pointer is defined (null) in imgui.cpp itself, so every binary that
// links imgui gets a harmless default and needs no editor symbol. Only this
// translation unit, compiled directly into DekiEditor.exe (the binary that
// runs the DockSpace), sets it, during static initialisation.

#include "imgui.h"
#include "imgui_internal.h"

// Must match the typedef and global the CMake patch adds to imgui.cpp (global
// namespace, null by default).
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

    // The panel below the separator (child_1) is the one whose tab top moves
    // with the drag. It sits one separator thickness below the separator line,
    // so its tab top, not the separator, is aligned with other panels' tab
    // tops; aligning the separator would leave the tabs off by the gap.
    const float movingTabTop = child0->Pos[axis] + *size0 + spacing;

    // Snap only to the near edge of other dock nodes: the top, where the tab
    // bar sits. The bottom edge is not a target, on purpose.
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

        // Skip everything in the region being resized: child_0, child_1 and
        // all their descendants move with the separator, so their edges (e.g.
        // a nested panel's tab just under the separator) are not stable
        // targets. Only panels outside this split count.
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

        // Only real tabbed panels (leaf nodes that host windows), and only
        // their top edge, where the tab bar sits.
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
    // Clamp to the minimum sizes ImGui enforces, so a node never collapses.
    const float snapped0 = ImClamp(best - spacing - child0->Pos[axis], minSize0, total - minSize1);
    *size0 = snapped0;
    *size1 = total - snapped0;
}

// Installs the hook during static initialisation. DockSnap.cpp is a direct
// source of DekiEditor.exe, not a static library member, so it is always
// linked and this runs before the first frame draws the DockSpace.
const bool kDockSnapInstalled = []
{
    GDekiDockSnapFn = &SnapDockSplitter;
    return true;
}();

}  // namespace
