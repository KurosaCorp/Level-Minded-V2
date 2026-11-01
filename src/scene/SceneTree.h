#pragma once

#include "core/MeshId.h"
#include "scene/Transform.h"

#include <DirectXMath.h>

#include <cstdint>
#include <span>
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
// The item's shape is its mesh scaled to `halfExtents`, then placed by `transform`.
struct ItemData {
    Transform transform;
    DirectX::XMFLOAT3 halfExtents{}; // local size (meters at scale 1)
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
// pointer chasing. Hot per-node data is a 16-byte struct; name handles and item data live in parallel
// arrays with the same order, and all name characters share one buffer. Indices change on structural
// edits; NodeId stays stable for a node's life.
//
// Visibility: each node has its own toggle (kVisible). A node is effectively visible only if it and
// all its ancestors are visible, so hiding a group hides its contents while each child keeps its own
// setting for when the group is shown again.
// A copy of one subtree, taken before it is removed, so it can be put back exactly (undo / redo).
struct SubtreeSnapshot {
    struct Entry {
        uint32_t id;          // NodeId
        uint32_t subtreeSize;
        uint32_t nameOffset;  // in `names`
        uint32_t nameLength;
        uint8_t kind;         // NodeKind
        uint8_t flags;        // SceneTree::Flag bits
        ItemData item;
    };
    std::vector<Entry> nodes; // pre-order; nodes[0] is the root
    std::vector<char> names;

    bool Empty() const { return nodes.empty(); }
    size_t MemoryBytes() const { return nodes.capacity() * sizeof(Entry) + names.capacity(); }
};

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
    // `bounds` gives the initial placement: centered at bounds.center, unrotated, scale 1.
    NodeId CreateItem(std::string_view name, NodeKind kind, const Bounds& bounds, NodeId parent = kInvalidNode,
                      MeshId mesh = MeshId::Default);
    // Full placement (map loading): `halfExtents` is the item's local size, `transform` its placement.
    NodeId CreateItem(std::string_view name, NodeKind kind, const Transform& transform, const DirectX::XMFLOAT3& halfExtents,
                      NodeId parent, MeshId mesh);

    // Moves `node` and its subtree under `newParent` (kInvalidNode = top level), placed before the
    // sibling `before` (kInvalidNode = last). Returns false and changes nothing if the move is invalid.
    bool Move(NodeId node, NodeId newParent, NodeId before = kInvalidNode);
    // True if `node` may be re-parented under `newParent` (not into itself or its own subtree).
    bool CanMove(NodeId node, NodeId newParent) const;

    // Removes the node and its whole subtree.
    void Remove(NodeId node);
    void Rename(NodeId node, std::string_view name);
    // Removes every node and frees all of the tree's memory (not just its contents). Versions keep
    // counting up, so caches built from the old tree are rebuilt.
    void Clear();
    // Pre-allocates for `nodes` nodes with `nameChars` name characters in total (bulk building).
    void Reserve(uint32_t nodes, size_t nameChars);

    // Copies the subtree rooted at `node` (empty if it doesn't exist).
    SubtreeSnapshot Snapshot(NodeId node) const;
    // Puts a removed subtree back under `parent` (kInvalidNode = top level) before the sibling `before`
    // (kInvalidNode = last), with its original NodeIds, names, transforms and visibility toggles.
    // Selection is not restored. Fails (and changes nothing) if an id is in use again, or if
    // `parent`/`before` are not valid there.
    bool Restore(const SubtreeSnapshot& snapshot, NodeId parent, NodeId before);

    // ---- Visibility, selection, UI state (no index changes).

    void SetVisible(NodeId node, bool visible);
    // Selecting a node selects its whole subtree. Deselecting also deselects its ancestors, since
    // they no longer have all of their contents selected.
    void SetSelected(NodeId node, bool selected);
    void ClearSelection();
    void SetExpanded(NodeId node, bool expanded);
    // Items only (groups have no transform). If the value changed, bumps TransformVersion() and logs
    // the item in TransformChanges().
    void SetTransform(NodeId node, const Transform& transform);
    void SetTransformAt(uint32_t index, const Transform& transform);
    uint32_t SelectedItemCount() const { return m_selectedItemCount; } // items only, not groups

    // ---- Lookup and iteration. Indices are valid until the next structural edit.

    uint32_t Size() const { return static_cast<uint32_t>(m_nodes.size()); }
    uint32_t IndexOf(NodeId node) const { return node < m_indexOf.size() ? m_indexOf[node] : kNoIndex; }
    bool Contains(NodeId node) const { return IndexOf(node) != kNoIndex; }

    const Node& NodeAt(uint32_t index) const { return m_nodes[index]; }
    // Not null-terminated. Valid until the next edit (names live in one shared buffer that may grow).
    std::string_view NameAt(uint32_t index) const
    {
        const NameRef& ref = m_names[index];
        return {m_nameChars.data() + ref.offset, ref.length};
    }
    const ItemData& ItemAt(uint32_t index) const { return m_items[index]; }
    const Transform& TransformAt(uint32_t index) const { return m_items[index].transform; }
    MeshId MeshAt(uint32_t index) const { return m_items[index].mesh; }
    // World-space axis-aligned box around the item's (rotated, scaled) box.
    Bounds WorldBoundsAt(uint32_t index) const;
    // One past the last node of the subtree rooted at `index`.
    uint32_t SubtreeEnd(uint32_t index) const { return index + m_nodes[index].subtreeSize; }
    // Index of the next sibling, or kNoIndex if `index` is the last child.
    uint32_t NextSibling(uint32_t index) const;
    bool IsInSubtree(uint32_t root, uint32_t index) const { return index >= root && index < SubtreeEnd(root); }

    // ---- Change tracking. Consumers cache derived data and rebuild only when a version changes.

    // Bumped by structural edits and expand/collapse: anything that changes the hierarchy's rows.
    uint64_t LayoutVersion() const { return m_layoutVersion; }
    // Bumped by structural edits, visibility and selection: changes that need the viewport's draw
    // list rebuilt. Transform edits are tracked separately (below) so moved items can be patched.
    uint64_t RenderVersion() const { return m_renderVersion; }
    // Bumped by every transform change.
    uint64_t TransformVersion() const { return m_transformVersion; }
    // Bumped by every change that would be saved to a map file: structure, names, visibility and
    // transforms (not selection or expand/collapse). Used to detect unsaved changes.
    uint64_t ContentVersion() const { return m_contentVersion; }
    // Items whose transform changed since RenderVersion() last changed, oldest first (ids may repeat).
    // Any RenderVersion() bump clears it (consumers rebuild then anyway). If it would grow past the
    // scene size, a RenderVersion() bump replaces it, so it stays bounded during long drags.
    std::span<const NodeId> TransformChanges() const { return m_transformChanges; }
    // One past the largest NodeId handed out so far (for arrays indexed by NodeId).
    uint32_t IdCapacity() const { return static_cast<uint32_t>(m_indexOf.size()); }

    // Checks every structural invariant (ordering, sizes, links, cached flags, counters, names).
    bool Validate() const;

    // Bytes held by each internal array (capacity).
    struct MemoryUsage {
        size_t nodes = 0;
        size_t names = 0;           // name handles + the shared character buffer
        size_t nameAllocations = 0; // separate heap blocks used for name characters
        size_t nameGarbage = 0;     // characters of removed/renamed names awaiting compaction
        size_t items = 0;
        size_t indexMap = 0;
        size_t Total() const { return nodes + names + items + indexMap; }
    };
    MemoryUsage Memory() const;

private:
    // A name inside m_nameChars. 8 bytes, so moving nodes moves 8 bytes per name instead of a
    // 32-byte std::string (and long names never become separate heap blocks).
    struct NameRef {
        uint32_t offset;
        uint32_t length;
    };

    NodeId Insert(std::string_view name, NodeKind kind, const ItemData& item, NodeId parent);
    NameRef StoreName(std::string_view name);
    void ReleaseName(const NameRef& ref);
    void CompactNamesIfNeeded();
    void RebuildLinks();
    void UpdateEffectiveVisibility(uint32_t first, uint32_t end);
    void StructureChanged() { ++m_layoutVersion; ++m_contentVersion; RenderChanged(); }
    void RenderChanged() { ++m_renderVersion; m_transformChanges.clear(); }

    std::vector<Node> m_nodes;          // hot: traversal, flags
    std::vector<NameRef> m_names;       // parallel to m_nodes
    std::vector<ItemData> m_items;      // parallel to m_nodes
    std::vector<char> m_nameChars;      // all names back to back, not null-terminated
    std::vector<uint32_t> m_indexOf;    // NodeId -> index, kNoIndex once removed
    std::vector<uint32_t> m_stack;      // scratch for RebuildLinks
    std::vector<NodeId> m_transformChanges;
    uint32_t m_selectedItemCount = 0;
    uint32_t m_nameGarbage = 0;
    uint64_t m_layoutVersion = 0;
    uint64_t m_renderVersion = 0;
    uint64_t m_transformVersion = 0;
    uint64_t m_contentVersion = 0;
};

} // namespace lm
