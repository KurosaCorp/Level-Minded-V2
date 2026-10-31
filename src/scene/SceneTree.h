#pragma once

#include "core/MeshId.h"

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lm {

using NodeId = uint32_t;
inline constexpr NodeId kInvalidNode = 0xFFFFFFFFu;

enum class NodeKind : uint8_t { Group, Brush, Mesh, Light, Entity };

// Axis-aligned box in world space (meters).
struct Bounds {
    DirectX::XMFLOAT3 center{};
    DirectX::XMFLOAT3 halfExtents{};
};

// Per-item data that is not needed for tree traversal (cold path, parallel to the node array).
struct ItemData {
    Bounds bounds;               // also the item's placement: mesh scaled to halfExtents at center
    MeshId mesh = MeshId::None;
};

// Mesh shown for an item kind when CreateItem is called with MeshId::Default.
constexpr MeshId DefaultMesh(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Brush:  return MeshId::Cube;
    case NodeKind::Mesh:   return MeshId::Cube;
    case NodeKind::Light:  return MeshId::Sphere;
    case NodeKind::Entity: return MeshId::Spawn;
    case NodeKind::Group:  break;
    }
    return MeshId::None;
}

// Hierarchy of groups and level items, stored as one flat array in depth-first (pre-order) order.
//
// Every subtree occupies the contiguous index range [i, i + subtreeSize), so whole-subtree work
// (mass selection, visibility propagation, drawing) is a linear scan over adjacent memory with no
// pointer chasing. Hot per-node data is a 16-byte struct; names and item data live in parallel arrays
// with the same order. Indices change on structural edits; NodeId stays stable for a node's life.
//
// Visibility: each node has its own toggle (kVisible). A node is effectively visible only if it and
// all its ancestors are visible, so hiding a group hides its contents while each child keeps its own
// setting for when the group is shown again.
class SceneTree {
public:
    static constexpr uint32_t kNoIndex = 0xFFFFFFFFu;

    enum Flag : uint8_t {
        kVisible = 1 << 0,            // the node's own eye toggle
        kEffectivelyVisible = 1 << 1, // kVisible on the node and on every ancestor
        kSelected = 1 << 2,
        kExpanded = 1 << 3,           // UI state: group is open in the hierarchy panel
    };

    struct Node {
        NodeId id;
        uint32_t parent;      // index of the parent, kNoIndex for top-level nodes
        uint32_t subtreeSize; // nodes in this subtree, including itself
        uint16_t depth;       // 0 for top-level nodes
        NodeKind kind;
        uint8_t flags;        // Flag bits

        bool Has(Flag flag) const { return (flags & flag) != 0; }
        bool IsGroup() const { return kind == NodeKind::Group; }
    };
    static_assert(sizeof(Node) == 16, "keep the hot node data at 16 bytes (4 per cache line)");

    // ---- Editing. Structural edits invalidate indices (not NodeIds).

    // New nodes are appended as the last child of `parent` (kInvalidNode = top level).
    // Only groups can have children. Returns kInvalidNode on failure.
    NodeId CreateGroup(std::string_view name, NodeId parent = kInvalidNode);
    NodeId CreateItem(std::string_view name, NodeKind kind, const Bounds& bounds, NodeId parent = kInvalidNode,
                      MeshId mesh = MeshId::Default);

    // Moves `node` and its subtree under `newParent` (kInvalidNode = top level), placed before the
    // sibling `before` (kInvalidNode = last). Returns false and changes nothing if the move is invalid.
    bool Move(NodeId node, NodeId newParent, NodeId before = kInvalidNode);
    // True if `node` may be re-parented under `newParent` (not into itself or its own subtree).
    bool CanMove(NodeId node, NodeId newParent) const;

    // Removes the node and its whole subtree.
    void Remove(NodeId node);
    void Rename(NodeId node, std::string_view name);
    void Clear();

    // ---- Visibility, selection, UI state (no index changes).

    void SetVisible(NodeId node, bool visible);
    // Selecting a node selects its whole subtree. Deselecting also deselects its ancestors, since
    // they no longer have all of their contents selected.
    void SetSelected(NodeId node, bool selected);
    void ClearSelection();
    void SetExpanded(NodeId node, bool expanded);
    uint32_t SelectedItemCount() const { return m_selectedItemCount; } // items only, not groups

    // ---- Lookup and iteration. Indices are valid until the next structural edit.

    uint32_t Size() const { return static_cast<uint32_t>(m_nodes.size()); }
    uint32_t IndexOf(NodeId node) const { return node < m_indexOf.size() ? m_indexOf[node] : kNoIndex; }
    bool Contains(NodeId node) const { return IndexOf(node) != kNoIndex; }

    const Node& NodeAt(uint32_t index) const { return m_nodes[index]; }
    const std::string& NameAt(uint32_t index) const { return m_names[index]; }
    const Bounds& BoundsAt(uint32_t index) const { return m_items[index].bounds; }
    MeshId MeshAt(uint32_t index) const { return m_items[index].mesh; }
    // One past the last node of the subtree rooted at `index`.
    uint32_t SubtreeEnd(uint32_t index) const { return index + m_nodes[index].subtreeSize; }
    // Index of the next sibling, or kNoIndex if `index` is the last child.
    uint32_t NextSibling(uint32_t index) const;
    bool IsInSubtree(uint32_t root, uint32_t index) const { return index >= root && index < SubtreeEnd(root); }

    // Checks every structural invariant (ordering, sizes, links, cached flags, counters).
    bool Validate() const;

private:
    NodeId Insert(std::string_view name, NodeKind kind, const ItemData& item, NodeId parent);
    void RebuildLinks();
    void UpdateEffectiveVisibility(uint32_t first, uint32_t end);

    std::vector<Node> m_nodes;
    std::vector<std::string> m_names;
    std::vector<ItemData> m_items;
    std::vector<uint32_t> m_indexOf;    // NodeId -> index, kNoIndex once removed
    std::vector<uint32_t> m_stack;      // scratch for RebuildLinks
    uint32_t m_selectedItemCount = 0;
};

} // namespace lm
