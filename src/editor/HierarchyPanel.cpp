#include "editor/HierarchyPanel.h"

#include "editor/NodeStyle.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace lm {

namespace {

constexpr const char* kPayloadType = "LM_SCENE_NODE";

ImU32 WithAlpha(ImU32 color, float alpha)
{
    const ImU32 a = static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// Eye icon drawn with the draw list (the default ImGui font has no icon glyphs).
void DrawEyeIcon(ImDrawList* drawList, ImVec2 center, float size, bool open, ImU32 color)
{
    const float w = size * 0.5f;
    const float h = size * 0.42f;
    const float thickness = std::max(1.0f, size * 0.09f);
    const ImVec2 left(center.x - w, center.y);
    const ImVec2 right(center.x + w, center.y);
    if (open) {
        drawList->AddBezierQuadratic(left, ImVec2(center.x, center.y - h * 1.6f), right, color, thickness);
        drawList->AddBezierQuadratic(left, ImVec2(center.x, center.y + h * 1.6f), right, color, thickness);
        drawList->AddCircleFilled(center, size * 0.17f, color);
    } else {
        // Closed lid with lashes.
        drawList->AddBezierQuadratic(left, ImVec2(center.x, center.y + h * 1.4f), right, color, thickness);
        for (int i = -1; i <= 1; ++i) {
            const float x = center.x + float(i) * w * 0.55f;
            const float y = center.y + h * 0.62f - (i == 0 ? h * 0.08f : 0.0f);
            drawList->AddLine(ImVec2(x, y), ImVec2(x + float(i) * w * 0.15f, y + h * 0.55f), color, thickness);
        }
    }
}

// Folder for groups, rounded square for items, tinted by kind.
void DrawKindIcon(ImDrawList* drawList, ImVec2 topLeft, float size, NodeKind kind, float alpha)
{
    const ImU32 color = WithAlpha(KindColor(kind), alpha);
    if (kind == NodeKind::Group) {
        const float tabW = size * 0.45f, tabH = size * 0.18f;
        const ImVec2 bodyMin(topLeft.x, topLeft.y + size * 0.22f);
        drawList->AddRectFilled(ImVec2(topLeft.x, bodyMin.y - tabH), ImVec2(topLeft.x + tabW, bodyMin.y + 1.0f), color, size * 0.08f);
        drawList->AddRectFilled(bodyMin, ImVec2(topLeft.x + size, topLeft.y + size * 0.9f), color, size * 0.1f);
    } else {
        const float inset = size * 0.12f;
        drawList->AddRectFilled(ImVec2(topLeft.x + inset, topLeft.y + inset), ImVec2(topLeft.x + size - inset, topLeft.y + size - inset),
                                color, size * 0.22f);
    }
}

} // namespace

void HierarchyPanel::Draw(SceneTree& tree, float dpiScale)
{
    if (!ImGui::Begin(kWindowName)) {
        ImGui::End();
        return;
    }

    // ---- Toolbar
    if (ImGui::Button("+ Group"))
        m_createGroupRequested = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(tree.SelectedItemCount() == 0 && m_activeNode == kInvalidNode);
    if (ImGui::Button("Delete"))
        m_deleteRequested = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Collapse all")) {
        for (uint32_t i = 0; i < tree.Size(); ++i)
            if (tree.NodeAt(i).IsGroup())
                tree.SetExpanded(tree.NodeAt(i).id, false);
    }

    m_activeGroupThisFrame = ActiveGroup(tree);

    // ---- Rows that are not inside a collapsed group. Collapsed subtrees are skipped in O(1).
    // Rebuilt only when the tree's layout (structure or expand state) changed.
    if (m_rowsLayoutVersion != tree.LayoutVersion()) {
        m_rows.clear();
        for (uint32_t i = 0; i < tree.Size();) {
            m_rows.push_back(i);
            const SceneTree::Node& node = tree.NodeAt(i);
            i = (node.IsGroup() && !node.Has(SceneTree::kExpanded)) ? tree.SubtreeEnd(i) : i + 1;
        }
        m_rowsLayoutVersion = tree.LayoutVersion();
    }

    const float footerHeight = ImGui::GetFrameHeightWithSpacing();
    constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_NoPadOuterX;
    if (ImGui::BeginTable("##HierarchyTree", 2, kTableFlags, ImVec2(0.0f, -footerHeight))) {
        ImGui::TableSetupColumn("##Visibility", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_rows.size()));
        if (m_scrollToNode != kInvalidNode) {
            const uint32_t target = tree.IndexOf(m_scrollToNode);
            const auto row = std::find(m_rows.begin(), m_rows.end(), target);
            if (row != m_rows.end())
                clipper.IncludeItemByIndex(static_cast<int>(row - m_rows.begin()));
        }
        while (clipper.Step())
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                DrawRow(tree, m_rows[static_cast<size_t>(row)], dpiScale);

        // Click on empty space below the rows clears the selection.
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) {
            tree.ClearSelection();
            m_activeNode = kInvalidNode;
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled("%u nodes   %u items selected", tree.Size(), tree.SelectedItemCount());

    // ---- Keyboard (only while this panel has focus and no text field is active)
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            m_deleteRequested = true;
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && tree.Contains(m_activeNode))
            BeginRename(tree, m_activeNode);
    }

    ImGui::End();
    ApplyPendingEdits(tree);
}

void HierarchyPanel::DrawRow(SceneTree& tree, uint32_t index, float dpiScale)
{
    const SceneTree::Node node = tree.NodeAt(index); // copy: flags may change while the row is drawn
    const NodeId id = node.id;
    const bool selected = node.Has(SceneTree::kSelected);
    const bool visible = node.Has(SceneTree::kEffectivelyVisible);
    const bool hasChildren = node.subtreeSize > 1;
    const bool renaming = (m_renamingNode == id);

    ImGui::PushID(static_cast<int>(id));
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    DrawEyeButton(tree, node, dpiScale);

    ImGui::TableSetColumnIndex(1);
    const float indent = float(node.depth) * ImGui::GetStyle().IndentSpacing;
    if (indent > 0.0f)
        ImGui::Indent(indent);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                               ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_FramePadding;
    if (!hasChildren)
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (selected)
        flags |= ImGuiTreeNodeFlags_Selected;
    if (renaming)
        flags |= ImGuiTreeNodeFlags_AllowOverlap;
    if (hasChildren)
        ImGui::SetNextItemOpen(node.Has(SceneTree::kExpanded), ImGuiCond_Always);

    // The label is drawn by hand below (icon + name), so the tree node itself has an empty label.
    const bool open = ImGui::TreeNodeEx("##node", flags);
    const bool toggled = ImGui::IsItemToggledOpen();
    if (toggled)
        tree.SetExpanded(id, open);
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !toggled)
        HandleClick(tree, id, selected);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !toggled)
        BeginRename(tree, id);

    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kPayloadType, &id, sizeof(id));
        const std::string_view name = tree.NameAt(index);
        ImGui::TextUnformatted(name.data(), name.data() + name.size());
        ImGui::EndDragDropSource();
    }

    // Icon + name.
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float labelX = rowMin.x + ImGui::GetTreeNodeToLabelSpacing();
    const float fontSize = ImGui::GetFontSize();
    const float rowCenterY = (rowMin.y + rowMax.y) * 0.5f;
    DrawKindIcon(drawList, ImVec2(labelX, rowCenterY - fontSize * 0.5f), fontSize, node.kind, visible ? 1.0f : 0.4f);
    const float textX = labelX + fontSize + 4.0f * dpiScale;

    // Accent bar on the group that receives newly placed items (Assets panel target).
    if (id == m_activeGroupThisFrame)
        drawList->AddRectFilled(ImVec2(rowMin.x - 3.0f * dpiScale, rowMin.y + 2.0f * dpiScale), ImVec2(rowMin.x, rowMax.y - 2.0f * dpiScale),
                                kSelectionColor);

    if (renaming) {
        ImGui::SetCursorScreenPos(ImVec2(textX, rowMin.y));
        ImGui::SetNextItemWidth(std::max(rowMax.x - textX, 40.0f * dpiScale));
        if (m_renameNeedsFocus) {
            ImGui::SetKeyboardFocusHere();
            m_renameNeedsFocus = false;
        }
        const bool committed = ImGui::InputText("##rename", m_renameBuffer, sizeof(m_renameBuffer),
                                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if ((committed || ImGui::IsItemDeactivatedAfterEdit()) && m_renameBuffer[0] != '\0')
            tree.Rename(id, m_renameBuffer);
        if (committed || ImGui::IsItemDeactivated())
            m_renamingNode = kInvalidNode;
    } else {
        const ImU32 textColor = ImGui::GetColorU32(visible ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        const std::string_view name = tree.NameAt(index);
        drawList->AddText(ImVec2(textX, rowCenterY - fontSize * 0.5f), textColor, name.data(), name.data() + name.size());
    }

    HandleDropTarget(tree, index, rowMin, rowMax, labelX);

    if (m_scrollToNode == id) {
        ImGui::SetScrollHereY(0.5f);
        m_scrollToNode = kInvalidNode;
    }

    if (indent > 0.0f)
        ImGui::Unindent(indent);
    ImGui::PopID();
}

void HierarchyPanel::DrawEyeButton(SceneTree& tree, const SceneTree::Node& node, float dpiScale)
{
    const float size = ImGui::GetFrameHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##eye", ImVec2(size, size)))
        tree.SetVisible(node.id, !node.Has(SceneTree::kVisible));

    const bool ownVisible = node.Has(SceneTree::kVisible);
    const bool hiddenByParent = ownVisible && !node.Has(SceneTree::kEffectivelyVisible);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 center(min.x + size * 0.5f, min.y + size * 0.5f);
    if (hovered)
        drawList->AddCircleFilled(center, size * 0.42f, WithAlpha(ImGui::GetColorU32(ImGuiCol_HeaderHovered), 0.3f));

    ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    if (!ownVisible || hiddenByParent)
        color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    else if (!hovered)
        color = WithAlpha(color, 0.75f);
    DrawEyeIcon(drawList, center, size * 0.55f, ownVisible, color);

    if (hovered && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted(ownVisible ? "Hide" : "Show");
        if (hiddenByParent)
            ImGui::TextDisabled("Hidden by a parent group");
        ImGui::EndTooltip();
    }
    (void)dpiScale;
}

NodeId HierarchyPanel::ActiveGroup(const SceneTree& tree) const
{
    const uint32_t active = tree.IndexOf(m_activeNode);
    if (active == SceneTree::kNoIndex)
        return kInvalidNode;
    const SceneTree::Node& node = tree.NodeAt(active);
    if (node.IsGroup())
        return node.id;
    return node.parent == SceneTree::kNoIndex ? kInvalidNode : tree.NodeAt(node.parent).id;
}

void HierarchyPanel::Reset()
{
    m_rows.clear();
    m_rowsLayoutVersion = ~0ull;
    m_pendingMoves.clear();
    m_createGroupRequested = false;
    m_deleteRequested = false;
    m_activeNode = kInvalidNode;
    m_activeGroupThisFrame = kInvalidNode;
    m_scrollToNode = kInvalidNode;
    m_renamingNode = kInvalidNode;
    m_renameNeedsFocus = false;
    m_renameBuffer[0] = '\0';
    m_newGroupCounter = 1;
}

void HierarchyPanel::Reveal(SceneTree& tree, NodeId node)
{
    const uint32_t index = tree.IndexOf(node);
    if (index == SceneTree::kNoIndex)
        return;
    for (uint32_t a = tree.NodeAt(index).parent; a != SceneTree::kNoIndex; a = tree.NodeAt(a).parent)
        tree.SetExpanded(tree.NodeAt(a).id, true);
    m_scrollToNode = node;
}

void HierarchyPanel::HandleClick(SceneTree& tree, NodeId id, bool wasSelected)
{
    if (ImGui::GetIO().KeyCtrl) {
        tree.SetSelected(id, !wasSelected);
    } else {
        tree.ClearSelection();
        tree.SetSelected(id, true); // selects the whole subtree
    }
    m_activeNode = id;
}

void HierarchyPanel::HandleDropTarget(SceneTree& tree, uint32_t index, const ImVec2& rowMin, const ImVec2& rowMax, float labelX)
{
    if (!ImGui::BeginDragDropTarget())
        return;

    const ImGuiPayload* payload =
        ImGui::AcceptDragDropPayload(kPayloadType, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
    if (payload && payload->DataSize == sizeof(NodeId)) {
        NodeId dragged;
        std::memcpy(&dragged, payload->Data, sizeof(dragged));

        const SceneTree::Node& target = tree.NodeAt(index);
        const NodeId targetParent = target.parent == SceneTree::kNoIndex ? kInvalidNode : tree.NodeAt(target.parent).id;
        const float t = (ImGui::GetMousePos().y - rowMin.y) / std::max(rowMax.y - rowMin.y, 1.0f);
        const bool expandedWithChildren = target.IsGroup() && target.subtreeSize > 1 && target.Has(SceneTree::kExpanded);

        // Work out the move: parent + insert-before sibling, and where to draw the indicator.
        enum class Place { Before, After, Inside } place;
        PendingMove move{dragged, targetParent, kInvalidNode};
        if (target.IsGroup() && t >= 0.25f && t <= 0.75f) {
            place = Place::Inside;
            move = {dragged, target.id, kInvalidNode};
        } else if (t < 0.5f) {
            place = Place::Before;
            move.before = target.id;
        } else if (expandedWithChildren) {
            // Below an open group's row is visually its first child slot.
            place = Place::After;
            move = {dragged, target.id, tree.NodeAt(index + 1).id};
        } else {
            place = Place::After;
            const uint32_t next = tree.NextSibling(index);
            move.before = next == SceneTree::kNoIndex ? kInvalidNode : tree.NodeAt(next).id;
        }

        if (dragged != target.id && tree.CanMove(dragged, move.parent)) {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImU32 color = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
            drawList->PushClipRectFullScreen();
            if (place == Place::Inside)
                drawList->AddRect(rowMin, rowMax, color, 2.0f, 2.0f); // rounding, thickness
            else {
                const float y = place == Place::Before ? rowMin.y : rowMax.y;
                drawList->AddLine(ImVec2(labelX, y), ImVec2(rowMax.x, y), color, 2.0f);
            }
            drawList->PopClipRect();

            if (payload->IsDelivery())
                m_pendingMoves.push_back(move);
        }
    }
    ImGui::EndDragDropTarget();
}

void HierarchyPanel::BeginRename(const SceneTree& tree, NodeId id)
{
    const uint32_t index = tree.IndexOf(id);
    if (index == SceneTree::kNoIndex)
        return;
    const std::string_view name = tree.NameAt(index);
    std::snprintf(m_renameBuffer, sizeof(m_renameBuffer), "%.*s", static_cast<int>(name.size()), name.data());
    m_renamingNode = id;
    m_renameNeedsFocus = true;
}

void HierarchyPanel::ApplyPendingEdits(SceneTree& tree)
{
    for (const PendingMove& move : m_pendingMoves) {
        if (tree.Move(move.node, move.parent, move.before) && move.parent != kInvalidNode)
            tree.SetExpanded(move.parent, true);
    }
    m_pendingMoves.clear();

    if (m_deleteRequested) {
        m_deleteRequested = false;
        // Top-most selected nodes (their subtrees go with them); fall back to the active node.
        std::vector<NodeId> doomed;
        for (uint32_t i = 0; i < tree.Size();) {
            if (tree.NodeAt(i).Has(SceneTree::kSelected)) {
                doomed.push_back(tree.NodeAt(i).id);
                i = tree.SubtreeEnd(i);
            } else {
                ++i;
            }
        }
        if (doomed.empty() && tree.Contains(m_activeNode))
            doomed.push_back(m_activeNode);
        for (NodeId id : doomed)
            tree.Remove(id);
        if (!tree.Contains(m_activeNode))
            m_activeNode = kInvalidNode;
    }

    if (m_createGroupRequested) {
        m_createGroupRequested = false;
        // Inside the active group, next to the active item, or at the top level.
        NodeId parent = kInvalidNode;
        const uint32_t active = tree.IndexOf(m_activeNode);
        if (active != SceneTree::kNoIndex) {
            const SceneTree::Node& node = tree.NodeAt(active);
            if (node.IsGroup())
                parent = node.id;
            else if (node.parent != SceneTree::kNoIndex)
                parent = tree.NodeAt(node.parent).id;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "Group %d", m_newGroupCounter++);
        const NodeId group = tree.CreateGroup(name, parent);
        if (parent != kInvalidNode)
            tree.SetExpanded(parent, true);
        tree.ClearSelection();
        tree.SetSelected(group, true);
        m_activeNode = group;
        m_scrollToNode = group;
        BeginRename(tree, group);
    }
}

} // namespace lm
