// The editor's search-first picker, shared by everything that adds a thing from
// a list: Add Component in the Inspector, Add Action in the node graph's stacks.
//
// It browses in two levels, categories and then what is inside one, because
// browsing beats one long list for the thing you cannot name yet ("it was
// something under Animation"), and searching beats browsing for the thing you
// can. Typing searches every level at once, with results grouped under their
// categories and ranked best first, and clearing the box takes you back where
// you were. It all works from the keyboard: type, arrows, Enter to open a
// group or take an item, Left to go back.
//
// The caller owns the popup (and its placement); this draws the body.

#include <deki-editor/EditorTheme.h>
#include <deki-editor/Localization.h>
#include "imgui.h"
#include "imgui_internal.h"
#include <deki-editor/IconsTabler.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace DekiEditor
{

namespace
{
// Per-picker state, keyed by the caller's id. Immediate mode rebuilds the
// list every frame; the query, the level you are in and where the highlight
// sits have to outlive that.
struct PickerState
{
    char search[128] = {};
    int highlight = 0;
    bool scrollTo = false;
    std::string lastQuery;
    std::string category;  // empty at the top level

    // Level change animation: the incoming level slides in from the side it
    // came from. `dir` is +1 when going deeper (in from the right) and -1
    // when coming back out (in from the left).
    float slideT = 1.0f;  // 1 = settled
    float slideDir = 0.0f;
};
std::unordered_map<ImGuiID, PickerState> s_Pickers;

std::string LowerOf(std::string v)
{
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

float SmoothStep(float t)
{
    return t * t * (3.0f - 2.0f * t);
}
}  // namespace

int SchematicPicker(const char* id, const char* title, const PickerItem* items, int itemCount, bool justOpened,
                    float listHeight, const char* itemIcon, const char* searchHint, const char* emptyText)
{
    if (!id || !items)
    {
        return -1;
    }

    ImGuiContext& g = *GImGui;
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    PickerState& st = s_Pickers[ImGui::GetID(id)];

    if (justOpened)
    {
        st.search[0] = 0;
        st.highlight = 0;
        st.lastQuery.clear();
        st.category.clear();
        st.slideT = 1.0f;
        // The magnifier glyph inside SearchField is not focusable, so this lands
        // on the search input that follows it.
        ImGui::SetKeyboardFocusHere();
    }

    // Title: a popup that appears under the cursor should say what it is. The
    // rule underneath makes the title and the search field read as one header
    // block instead of a stray line floating above a field.
    if (title && title[0])
    {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::Dummy(ImVec2(0.0f, 2.0f * dpi));
        if (g_FontDisplay)
        {
            ImGui::PushFont(g_FontDisplay, Fonts::CssPx(g_FontDisplay, 16.0f));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Fg);
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        if (g_FontDisplay)
        {
            ImGui::PopFont();
        }
        ImGui::Dummy(ImVec2(0.0f, 10.0f * dpi));
        FullBleedSeparator();
        ImGui::PopStyleVar();
    }

    SearchField("##picker_search", st.search, sizeof(st.search), 0.0f, /*bottomBorder*/ true, nullptr, nullptr, nullptr,
                nullptr, searchHint && searchHint[0] ? searchHint : "Search");

    const std::string query = LowerOf(st.search);

    // A new query invalidates the highlight: it pointed at a row that is not
    // there any more. Back to the top, which is now the best match.
    if (query != st.lastQuery)
    {
        st.lastQuery = query;
        st.highlight = 0;
        st.scrollTo = true;
    }

    // The query's words. Split so "random number" can match a name and a
    // description at once, rather than looking for that exact phrase.
    std::vector<std::string> words;
    std::string squashed;  // the query with the spaces dropped
    for (size_t i = 0; i < query.size();)
    {
        while (i < query.size() && query[i] == ' ')
        {
            ++i;
        }
        size_t j = i;
        while (j < query.size() && query[j] != ' ')
        {
            ++j;
        }
        if (j > i)
        {
            words.push_back(query.substr(i, j - i));
        }
        i = j;
    }
    for (char c : query)
    {
        if (c != ' ')
        {
            squashed.push_back(c);
        }
    }

    // Every letter of `needle`, in order, somewhere in `hay`.
    auto inOrder = [](const std::string& hay, const std::string& needle) -> bool
    {
        if (needle.empty())
        {
            return false;
        }
        size_t at = 0;
        for (char c : needle)
        {
            at = hay.find(c, at);
            if (at == std::string::npos)
            {
                return false;
            }
            ++at;
        }
        return true;
    };

    // Where the query matches, and how well. Lower rank is better, in bands:
    //     0..52  the name or the type name contains the query: exact first,
    //            then starts-with, then a match further in
    //   100..    every word of the query appears somewhere on the row, the
    //            description included, so "random number" finds Random Property
    //   200      the query's letters appear in order in the name, which is what
    //            catches a typo or an abbreviation ("sprtcomp")
    // The description is matched by whole words only: letters-in-order over a
    // sentence matches nearly everything, and that is noise, not search.
    struct Hit
    {
        int index = -1;
        int rank = 0;
        size_t matchPos = std::string::npos;  // in the label, for the tint
        size_t matchLen = 0;
    };
    // Lower-cased copies of every item's label, alternate name and
    // description, built only when the item set changes, not every frame.
    struct LoweredItem
    {
        const char* label;  // the source pointer, to detect a changed item set
        std::string labelLow, altLow, descLow;
    };
    static std::vector<LoweredItem> s_Lowered;
    static const PickerItem* s_LoweredItems = nullptr;
    static int s_LoweredCount = 0;
    bool loweredStale = (s_LoweredItems != items || s_LoweredCount != itemCount);
    for (int i = 0; !loweredStale && i < itemCount; ++i)
    {
        loweredStale = s_Lowered[i].label != items[i].label;
    }
    if (loweredStale)
    {
        s_Lowered.clear();
        s_Lowered.reserve(itemCount);
        for (int i = 0; i < itemCount; ++i)
        {
            const PickerItem& it = items[i];
            s_Lowered.push_back({ it.label, LowerOf(it.label ? it.label : ""), LowerOf(it.altName ? it.altName : ""),
                                  LowerOf(it.description ? it.description : "") });
        }
        s_LoweredItems = items;
        s_LoweredCount = itemCount;
    }

    auto scoreOf = [&](int index, Hit* out) -> bool
    {
        const std::string& labelLow = s_Lowered[index].labelLow;
        const std::string& altLow = s_Lowered[index].altLow;
        const std::string& descLow = s_Lowered[index].descLow;

        out->index = index;
        out->matchPos = std::string::npos;
        out->matchLen = query.size();

        if (query.empty())
        {
            out->rank = 0;
            return true;
        }

        const size_t inLabel = labelLow.find(query);
        const size_t inAlt = altLow.find(query);
        if (inLabel != std::string::npos || inAlt != std::string::npos)
        {
            out->matchPos = inLabel;  // npos when only the alternate name matched
            const size_t at = inLabel < inAlt ? inLabel : inAlt;
            if (at == 0 && (labelLow == query || altLow == query))
            {
                out->rank = 0;
            }
            else if (at == 0)
            {
                out->rank = 1;
            }
            else
            {
                out->rank = 2 + static_cast<int>(at < 50 ? at : 50);
            }
            return true;
        }

        // All words present, anywhere on the row. The description is where the
        // answer to "which one of these does X?" lives. Searched field by
        // field, so nothing is concatenated per item per frame.
        size_t earliest = std::string::npos;
        bool allWords = !words.empty();
        for (const std::string& word : words)
        {
            size_t at = labelLow.find(word);
            if (at == std::string::npos && altLow.find(word) != std::string::npos)
            {
                at = labelLow.size() + 1 + altLow.find(word);
            }
            if (at == std::string::npos && descLow.find(word) != std::string::npos)
            {
                at = labelLow.size() + altLow.size() + 2 + descLow.find(word);
            }
            if (at == std::string::npos)
            {
                allWords = false;
                break;
            }
            if (at < earliest)
            {
                earliest = at;
            }
        }
        if (allWords)
        {
            out->rank = 100 + static_cast<int>(earliest < 50 ? earliest : 50);
            return true;
        }

        // Last resort, names only: letters in order.
        if (inOrder(labelLow, squashed) || inOrder(altLow, squashed))
        {
            out->rank = 200;
            return true;
        }
        return false;
    };

    // Categories in first-seen order, so the list does not reshuffle itself
    // between frames the way a set would.
    std::vector<std::string> categories;
    for (int i = 0; i < itemCount; ++i)
    {
        const std::string cat = items[i].category ? items[i].category : "";
        if (std::find(categories.begin(), categories.end(), cat) == categories.end())
        {
            categories.push_back(cat);
        }
    }
    const bool flat = (categories.size() <= 1);  // nothing to browse: one list

    auto categoryHits = [&](const std::string& category, std::vector<Hit>* out)
    {
        for (int i = 0; i < itemCount; ++i)
        {
            const std::string cat = items[i].category ? items[i].category : "";
            if (cat != category)
            {
                continue;
            }
            Hit hit;
            if (scoreOf(i, &hit))
            {
                out->push_back(hit);
            }
        }
        std::sort(out->begin(), out->end(),
                  [&](const Hit& a, const Hit& b)
                  {
                      if (a.rank != b.rank)
                      {
                          return a.rank < b.rank;
                      }
                      const char* an = items[a.index].label;
                      const char* bn = items[b.index].label;
                      return std::strcmp(an ? an : "", bn ? bn : "") < 0;
                  });
    };

    enum class RowKind
    {
        Header,
        Back,
        Category,
        Item
    };
    struct Row
    {
        RowKind kind = RowKind::Header;
        std::string text;
        int count = 0;
        Hit hit;
    };
    std::vector<Row> rows;

    if (!query.empty() || flat)
    {
        // Search (or a picker with nothing to group): every category at once,
        // each headed, the group holding the best match first.
        struct Group
        {
            std::string header;
            std::vector<Hit> hits;
            int best = INT_MAX;
        };
        std::vector<Group> groups;
        for (const auto& category : categories)
        {
            Group group;
            group.header = category;
            categoryHits(category, &group.hits);
            if (group.hits.empty())
            {
                continue;
            }
            group.best = group.hits.front().rank;
            groups.push_back(std::move(group));
        }
        std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.best < b.best; });

        for (const Group& group : groups)
        {
            if (!group.header.empty() && !flat)
            {
                Row header;
                header.kind = RowKind::Header;
                header.text = group.header;
                rows.push_back(header);
            }
            for (const Hit& hit : group.hits)
            {
                Row row;
                row.kind = RowKind::Item;
                row.hit = hit;
                rows.push_back(row);
            }
        }
    }
    else if (st.category.empty())
    {
        // Top level: the categories themselves, with how much is inside.
        for (const auto& category : categories)
        {
            std::vector<Hit> hits;
            categoryHits(category, &hits);
            if (hits.empty())
            {
                continue;
            }

            Row row;
            row.kind = RowKind::Category;
            row.text = category;
            row.count = static_cast<int>(hits.size());
            rows.push_back(row);
        }
    }
    else
    {
        // Inside a category: a way back, then its items.
        Row back;
        back.kind = RowKind::Back;
        back.text = st.category;
        rows.push_back(back);

        std::vector<Hit> hits;
        categoryHits(st.category, &hits);
        for (const Hit& hit : hits)
        {
            Row row;
            row.kind = RowKind::Item;
            row.hit = hit;
            rows.push_back(row);
        }
    }

    int navCount = 0;
    for (const Row& row : rows)
    {
        if (row.kind != RowKind::Header)
        {
            ++navCount;
        }
    }

    // --- keyboard --------------------------------------------------------
    // Read after the field has handled the input. A single-line InputText
    // leaves the arrows and Enter alone, so type, arrow, Enter works without
    // the query losing focus.
    if (navCount > 0)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
        {
            st.highlight = (st.highlight + 1) % navCount;
            st.scrollTo = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
        {
            st.highlight = (st.highlight + navCount - 1) % navCount;
            st.scrollTo = true;
        }
        if (st.highlight >= navCount)
        {
            st.highlight = navCount - 1;
        }
    }
    const bool acceptPressed = ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                               ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                               ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);

    auto goToLevel = [&](const std::string& category, float dir)
    {
        st.category = category;
        st.highlight = 0;
        st.scrollTo = true;
        st.slideT = 0.0f;  // the new level slides in from `dir`
        st.slideDir = dir;
    };

    // Left steps back out of a category. Only while the query is empty: with
    // text in the box the arrows belong to the caret.
    if (query.empty() && !st.category.empty() && ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))
    {
        goToLevel(std::string(), -1.0f);
    }

    // --- the list --------------------------------------------------------
    // Runs edge to edge inside the popup. Left inside the popup's window
    // padding, the child's scrollbar would sit a padding-width in from the
    // right and look misaligned. The child keeps that padding inside it, so
    // rows stay inset while the scrollbar and the row highlight reach the
    // edges.
    {
        const float listY = ImGui::GetCursorScreenPos().y + 14.0f * dpi;
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x, listY));
    }
    const float listW = ImGui::GetWindowSize().x;
    // Fixed height, never sized to the rows. The popup keeps one size whatever
    // it shows: eight components, two, or none while a query matches nothing.
    // A picker that resizes as you type moves the rows you are aiming at, and
    // stepping into a small category would make the whole window jump; a menu
    // does not shrink around its items for the same reason.
    ImGui::BeginChild("##picker_list", ImVec2(listW, listHeight), ImGuiChildFlags_None);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 4.0f * dpi));

    // Advance the level slide. Rows are drawn a whole list-width off to one
    // side and travel in, which is what makes stepping into a group feel like
    // going somewhere rather than like the list being swapped out underneath.
    if (st.slideT < 1.0f)
    {
        st.slideT += g.IO.DeltaTime / 0.16f;
        if (st.slideT > 1.0f)
        {
            st.slideT = 1.0f;
        }
    }
    const float slideX = (st.slideT >= 1.0f) ? 0.0f : (1.0f - SmoothStep(st.slideT)) * st.slideDir * listW;

    // Two-line rows when there is something to say about the item, one line
    // when there is not. The second line matters: a project collects a dozen
    // scripts with near-identical names, and a picker that shows only names
    // asks you to remember which is which.
    const float rowH1 = 30.0f * dpi;
    const float rowH2 = 46.0f * dpi;
    // Content inset, applied to what a row draws rather than to the row itself,
    // so the highlight and the scrollbar still reach the popup's edges while the
    // icons and labels keep clear of them.
    const float rowPadX = 10.0f * dpi;

    // One row: an empty selectable with the content placed by hand over it, so a
    // row can carry an icon, a tinted match and a trailing count or chevron
    // without fighting a label's layout.
    auto drawRow = [&](const char* leadIcon, const std::string& label, const Hit* hit, const char* trailText,
                       bool trailChevron, bool disabled, bool highlighted, const char* subtitle = nullptr) -> bool
    {
        const bool twoLine = (subtitle && subtitle[0]);
        const float rowH = twoLine ? rowH2 : rowH1;

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + slideX);
        const ImVec2 rowStart = ImGui::GetCursorScreenPos();
        if (disabled)
        {
            ImGui::BeginDisabled();
        }
        // Explicit size: a default Selectable is one line of text tall, a
        // cramped target for a list you scan and click. Explicit width too, so
        // a row keeps its full width while the level slides across.
        const bool clicked = ImGui::Selectable("##row", highlighted, ImGuiSelectableFlags_None, ImVec2(listW, rowH));
        if (disabled)
        {
            ImGui::EndDisabled();
        }

        if (highlighted && st.scrollTo)
        {
            ImGui::SetScrollHereY(0.5f);
            st.scrollTo = false;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize();
        const ImVec2 rowSize = ImGui::GetItemRectSize();
        // Sub-line face: the editor's small UI font, asked for in CSS px and
        // then DPI-scaled, because AddText takes a final pixel size.
        ImFont* subFont = g_FontUISmall ? g_FontUISmall : font;
        const float subSize = Fonts::CssPx(subFont, 11.5f) * dpi;
        const float subGap = 3.0f * dpi;
        // One line centres; two lines stack around the centre.
        const float textY = twoLine ? rowStart.y + (rowSize.y - (fontSize + subGap + subSize)) * 0.5f
                                    : rowStart.y + (rowSize.y - fontSize) * 0.5f;
        const ImU32 dim = ImGui::ColorConvertFloat4ToU32(disabled ? Palette::Dim2 : Palette::Dim);
        const ImU32 fg = ImGui::ColorConvertFloat4ToU32(disabled ? Palette::Dim2 : Palette::Fg);
        const ImU32 tint = ImGui::ColorConvertFloat4ToU32(Palette::Accent);

        float x = rowStart.x + rowPadX;
        if (leadIcon && leadIcon[0])
        {
            dl->AddText(font, fontSize, ImVec2(x, textY), dim, leadIcon);
            x += ImGui::CalcTextSize(leadIcon).x + fontSize * 0.75f;
        }

        if (hit && hit->matchPos != std::string::npos && hit->matchLen > 0 && !disabled &&
            hit->matchPos + hit->matchLen <= label.size())
        {
            const std::string pre = label.substr(0, hit->matchPos);
            const std::string mid = label.substr(hit->matchPos, hit->matchLen);
            const std::string post = label.substr(hit->matchPos + hit->matchLen);
            dl->AddText(font, fontSize, ImVec2(x, textY), fg, pre.c_str());
            x += ImGui::CalcTextSize(pre.c_str()).x;
            dl->AddText(font, fontSize, ImVec2(x, textY), tint, mid.c_str());
            x += ImGui::CalcTextSize(mid.c_str()).x;
            dl->AddText(font, fontSize, ImVec2(x, textY), fg, post.c_str());
        }
        else
        {
            dl->AddText(font, fontSize, ImVec2(x, textY), fg, label.c_str());
        }

        // Trailing count and chevron, inset from the row's right edge: drawn
        // flush, they land under the scrollbar and get clipped away.
        float right = rowStart.x + rowSize.x - rowPadX;
        if (trailChevron)
        {
            const float w = ImGui::CalcTextSize(ICON_TI_CHEVRON_RIGHT).x;
            right -= w;
            dl->AddText(font, fontSize, ImVec2(right, textY), dim, ICON_TI_CHEVRON_RIGHT);
            right -= fontSize * 0.4f;
        }
        if (trailText && trailText[0])
        {
            const float w = ImGui::CalcTextSize(trailText).x;
            right -= w;
            dl->AddText(font, fontSize, ImVec2(right, textY), dim, trailText);
        }

        if (twoLine)
        {
            // Truncated to the row rather than wrapped: a picker is a list you
            // scan, and rows of varying height are harder to scan than a line
            // that ends in an ellipsis. The whole text is one hover away.
            const float avail = rowSize.x - rowPadX * 2.0f;
            std::string text = subtitle;
            const ImU32 subCol = ImGui::ColorConvertFloat4ToU32(disabled ? Palette::Dim2 : Palette::Dim);
            if (subFont->CalcTextSizeA(subSize, FLT_MAX, 0.0f, text.c_str()).x > avail)
            {
                const char* ellipsis = "...";
                const float dotsW = subFont->CalcTextSizeA(subSize, FLT_MAX, 0.0f, ellipsis).x;
                while (!text.empty() && subFont->CalcTextSizeA(subSize, FLT_MAX, 0.0f, text.c_str()).x + dotsW > avail)
                {
                    text.pop_back();
                }
                text += ellipsis;
            }
            dl->AddText(subFont, subSize, ImVec2(rowStart.x + rowPadX, textY + fontSize + subGap), subCol,
                        text.c_str());
        }
        return clicked;
    };

    int chosen = -1;
    int navIndex = 0;
    bool firstHeader = true;
    for (const Row& row : rows)
    {
        if (row.kind == RowKind::Header)
        {
            if (!firstHeader)
            {
                ImGui::Spacing();
            }
            firstHeader = false;
            // The editor's sub-header treatment, the same one a DEKI_GROUP gets
            // inside a component: a category is a heading here, not a caption.
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + rowPadX + slideX);
            SchematicSubHeader(row.text.c_str());
            continue;
        }

        const bool highlighted = (navIndex == st.highlight);
        ImGui::PushID(navIndex);

        if (row.kind == RowKind::Back)
        {
            std::string upper = row.text;
            std::transform(upper.begin(), upper.end(), upper.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (drawRow(ICON_TI_CHEVRON_LEFT, upper, nullptr, nullptr, false, false, highlighted) ||
                (highlighted && acceptPressed))
            {
                goToLevel(std::string(), -1.0f);
            }
            if (ImGui::IsItemHovered())
            {
                st.highlight = navIndex;
            }
        }
        else if (row.kind == RowKind::Category)
        {
            char count[16];
            std::snprintf(count, sizeof(count), "%d", row.count);
            if (drawRow(ICON_TI_FOLDER, row.text, nullptr, count, /*chevron*/ true, false, highlighted) ||
                (highlighted && acceptPressed))
            {
                goToLevel(row.text, 1.0f);
            }
            if (ImGui::IsItemHovered())
            {
                st.highlight = navIndex;
            }
        }
        else
        {
            const PickerItem& item = items[row.hit.index];
            const std::string label = item.label ? item.label : "";
            // What the second line says: the item's own description when it has
            // one, otherwise its type name when that differs from the label,
            // which is the next most useful answer to "which of these is it?".
            // A row with neither stays one line rather than showing filler.
            const char* subtitle = nullptr;
            if (item.description && item.description[0])
            {
                subtitle = item.description;
            }
            else if (item.altName && item.altName[0] && label != item.altName)
            {
                subtitle = item.altName;
            }

            const bool clicked =
                drawRow(itemIcon, label, &row.hit, nullptr, false, item.disabled, highlighted, subtitle);
            if (item.disabled)
            {
                if (item.disabledTip && item.disabledTip[0] &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    Tooltip("%s", item.disabledTip);
                }
            }
            else
            {
                // The untruncated text on hover, since the row cuts it to width.
                if (ImGui::IsItemHovered())
                {
                    if (item.tooltip && item.tooltip[0])
                    {
                        Tooltip("%s", item.tooltip);
                    }
                    else if (subtitle)
                    {
                        Tooltip("%s\n%s", label.c_str(), subtitle);
                    }
                }
                // Mouse and keyboard share one highlight, so Enter always acts
                // on the row you are pointing at.
                if (ImGui::IsItemHovered())
                {
                    st.highlight = navIndex;
                }
                if (clicked || (highlighted && acceptPressed))
                {
                    chosen = row.hit.index;
                }
            }
        }

        ImGui::PopID();
        ++navIndex;
    }

    if (navCount == 0)
    {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, Palette::Dim);
        ImGui::TextUnformatted(query.empty() ? (emptyText && emptyText[0] ? emptyText : Tr("Nothing available"))
                                             : Tr("No matches"));
        ImGui::PopStyleColor();
    }

    ImGui::PopStyleVar();  // list ItemSpacing
    ImGui::EndChild();

    if (chosen >= 0)
    {
        st.search[0] = 0;  // next open starts clean even if the caller keeps the popup
    }
    return chosen;
}

}  // namespace DekiEditor
