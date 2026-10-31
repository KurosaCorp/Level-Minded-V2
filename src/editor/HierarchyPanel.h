#pragma once

#include "scene/SceneTree.h"

#include <vector>

struct ImDrawList;
struct ImVec2;

namespace lm {

// Left-side panel showing the scene hierarchy as a tree.
//
//   Click            select node + everything inside it (Ctrl+click: toggle)
//   Eye icon         show / hide node; hiding a group hides its contents
//   Drag & drop      top/bottom edge of a row: reorder before/after it; middle of a group: move into it
//   Double-click/F2  rename           Delete   delete selection          "+ Group"   new group
//
// Only rows on screen are submitted (list clipper over the expanded rows), so panel cost does not
// grow with level size. Structural edits are queued during drawing and applied after the tree
// widget ends, so indices stay valid while rows are being drawn.
class HierarchyPanel {
public:
    static constexpr const char* kWindowName = "Hierarchy";

    void Draw(SceneTree& tree, float dpiScale);

    // Group that new content goes into: the active (last clicked) node if it is a group, otherwise
    // the active item's parent. kInvalidNode means the top level.
    NodeId ActiveGroup(const SceneTree& tree) const;
    void SetActiveNode(NodeId node) { m_activeNode = node; }
    // Expands all ancestors of `node` and scrolls it into view on the next Draw().
    void Reveal(SceneTree& tree, NodeId node);

private:
    struct PendingMove {
        NodeId node;
        NodeId parent;
        NodeId before;
    };

    void DrawRow(SceneTree& tree, uint32_t index, float dpiScale);
    void DrawEyeButton(SceneTree& tree, const SceneTree::Node& node, float dpiScale);
    void HandleDropTarget(SceneTree& tree, uint32_t index, const ImVec2& rowMin, const ImVec2& rowMax, float labelX);
    void HandleClick(SceneTree& tree, NodeId id, bool wasSelected);
    void BeginRename(const SceneTree& tree, NodeId id);
    void ApplyPendingEdits(SceneTree& tree);

    std::vector<uint32_t> m_rows; // indices of rows not hidden inside collapsed groups (rebuilt per frame)
    std::vector<PendingMove> m_pendingMoves;
    bool m_createGroupRequested = false;
    bool m_deleteRequested = false;

    NodeId m_activeNode = kInvalidNode; // last clicked node: anchor for "+ Group" and the Assets panel
    NodeId m_activeGroupThisFrame = kInvalidNode;
    NodeId m_scrollToNode = kInvalidNode;
    NodeId m_renamingNode = kInvalidNode;
    bool m_renameNeedsFocus = false;
    char m_renameBuffer[128] = {};
    int m_newGroupCounter = 1;
};

} // namespace lm
