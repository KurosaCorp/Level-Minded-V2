#include "scene/SceneTree.h"

#include <algorithm>
#include <string>

namespace lm {

namespace {

// Moves the block [first, first + count) so it starts at `dest` (dest is an index in the array
// before the move; it must lie outside the block).
template <typename T>
void MoveBlock(std::vector<T>& values, uint32_t first, uint32_t count, uint32_t dest)
{
    const auto begin = values.begin();
    if (dest < first)
        std::rotate(begin + dest, begin + first, begin + first + count);
    else if (dest > first + count)
        std::rotate(begin + first, begin + first + count, begin + dest);
}

// Compact the name buffer once at least this much of it is garbage (and at least half of it).
constexpr uint32_t kNameCompactionThreshold = 64 * 1024;

} // namespace

NodeId SceneTree::CreateGroup(std::string_view name, NodeId parent)
{
    return Insert(name, NodeKind::Group, ItemData{}, parent);
}

NodeId SceneTree::CreateItem(std::string_view name, NodeKind kind, const Bounds& bounds, NodeId parent, MeshId mesh)
{
    if (kind == NodeKind::Group)
        return kInvalidNode;
    return Insert(name, kind, ItemData{bounds, mesh == MeshId::Default ? DefaultMesh(kind) : mesh}, parent);
}

SceneTree::NameRef SceneTree::StoreName(std::string_view name)
{
    // `name` may point into m_nameChars itself (e.g. renaming to an existing name); copy first,
    // since appending can reallocate the buffer.
    const char* const begin = m_nameChars.data();
    if (!name.empty() && name.data() >= begin && name.data() < begin + m_nameChars.size()) {
        const std::string copy(name);
        return StoreName(copy);
    }
    const NameRef ref{static_cast<uint32_t>(m_nameChars.size()), static_cast<uint32_t>(name.size())};
    m_nameChars.insert(m_nameChars.end(), name.begin(), name.end());
    return ref;
}

void SceneTree::ReleaseName(const NameRef& ref)
{
    m_nameGarbage += ref.length;
}

void SceneTree::CompactNamesIfNeeded()
{
    if (m_nameGarbage < kNameCompactionThreshold || m_nameGarbage * 2 < m_nameChars.size())
        return;
    // Rewrite live names in tree order, which also restores locality for front-to-back iteration.
    std::vector<char> compacted;
    compacted.reserve(m_nameChars.size() - m_nameGarbage);
    for (NameRef& ref : m_names) {
        const uint32_t offset = static_cast<uint32_t>(compacted.size());
        compacted.insert(compacted.end(), m_nameChars.begin() + ref.offset, m_nameChars.begin() + ref.offset + ref.length);
        ref.offset = offset;
    }
    m_nameChars.swap(compacted);
    m_nameGarbage = 0;
}

NodeId SceneTree::Insert(std::string_view name, NodeKind kind, const ItemData& item, NodeId parentId)
{
    uint32_t parent = kNoIndex;
    uint32_t at = Size();
    uint16_t depth = 0;
    bool parentVisible = true;

    if (parentId != kInvalidNode) {
        parent = IndexOf(parentId);
        if (parent == kNoIndex || !m_nodes[parent].IsGroup())
            return kInvalidNode;
        at = SubtreeEnd(parent);
        depth = static_cast<uint16_t>(m_nodes[parent].depth + 1);
        parentVisible = m_nodes[parent].Has(kEffectivelyVisible);
        for (uint32_t a = parent; a != kNoIndex; a = m_nodes[a].parent)
            ++m_nodes[a].subtreeSize;
    }

    const NodeId id = static_cast<NodeId>(m_indexOf.size());
    uint8_t flags = kVisible | (parentVisible ? kEffectivelyVisible : 0);
    if (kind == NodeKind::Group)
        flags |= kExpanded;

    m_nodes.insert(m_nodes.begin() + at, Node{id, parent, 1, depth, kind, flags});
    m_names.insert(m_names.begin() + at, StoreName(name));
    m_items.insert(m_items.begin() + at, item);
    m_indexOf.push_back(at);

    // Everything after the insertion point shifted one slot to the right. When building a tree in
    // depth-first order, `at` is always the end of the array and this loop does nothing.
    for (uint32_t i = at + 1; i < Size(); ++i) {
        Node& node = m_nodes[i];
        if (node.parent != kNoIndex && node.parent >= at)
            ++node.parent;
        m_indexOf[node.id] = i;
    }
    StructureChanged();
    return id;
}

bool SceneTree::CanMove(NodeId nodeId, NodeId newParentId) const
{
    const uint32_t node = IndexOf(nodeId);
    if (node == kNoIndex)
        return false;
    if (newParentId == kInvalidNode)
        return true;
    const uint32_t parent = IndexOf(newParentId);
    return parent != kNoIndex && m_nodes[parent].IsGroup() && !IsInSubtree(node, parent);
}

bool SceneTree::Move(NodeId nodeId, NodeId newParentId, NodeId beforeId)
{
    if (!CanMove(nodeId, newParentId))
        return false;

    const uint32_t first = IndexOf(nodeId);
    const uint32_t count = m_nodes[first].subtreeSize;
    const uint32_t newParent = newParentId == kInvalidNode ? kNoIndex : IndexOf(newParentId);

    // Destination index in the current array.
    uint32_t dest;
    if (beforeId != kInvalidNode) {
        const uint32_t before = IndexOf(beforeId);
        if (before == kNoIndex || m_nodes[before].parent != newParent)
            return false;
        if (before == first)
            return true; // "move before itself": already there
        dest = before;
    } else {
        dest = newParent == kNoIndex ? Size() : SubtreeEnd(newParent);
    }

    const uint32_t oldParent = m_nodes[first].parent;
    if (oldParent == newParent && (dest == first || dest == first + count))
        return true; // same parent, same position

    // Subtree sizes: the block leaves its old ancestors and joins the new ones. Shared ancestors net to zero.
    for (uint32_t a = oldParent; a != kNoIndex; a = m_nodes[a].parent)
        m_nodes[a].subtreeSize -= count;
    for (uint32_t a = newParent; a != kNoIndex; a = m_nodes[a].parent)
        m_nodes[a].subtreeSize += count;

    MoveBlock(m_nodes, first, count, dest);
    MoveBlock(m_names, first, count, dest);
    MoveBlock(m_items, first, count, dest);
    RebuildLinks();

    const uint32_t movedFirst = IndexOf(nodeId);
    UpdateEffectiveVisibility(movedFirst, movedFirst + count);
    StructureChanged();
    return true;
}

void SceneTree::Remove(NodeId nodeId)
{
    const uint32_t first = IndexOf(nodeId);
    if (first == kNoIndex)
        return;
    const uint32_t end = SubtreeEnd(first);
    const uint32_t count = end - first;

    for (uint32_t a = m_nodes[first].parent; a != kNoIndex; a = m_nodes[a].parent)
        m_nodes[a].subtreeSize -= count;
    for (uint32_t i = first; i < end; ++i) {
        const Node& node = m_nodes[i];
        if (!node.IsGroup() && node.Has(kSelected))
            --m_selectedItemCount;
        m_indexOf[node.id] = kNoIndex;
        ReleaseName(m_names[i]);
    }

    m_nodes.erase(m_nodes.begin() + first, m_nodes.begin() + end);
    m_names.erase(m_names.begin() + first, m_names.begin() + end);
    m_items.erase(m_items.begin() + first, m_items.begin() + end);
    RebuildLinks();
    CompactNamesIfNeeded();
    StructureChanged();
}

void SceneTree::Rename(NodeId nodeId, std::string_view name)
{
    const uint32_t index = IndexOf(nodeId);
    if (index == kNoIndex || NameAt(index) == name)
        return;
    const NameRef old = m_names[index];
    m_names[index] = StoreName(name);
    ReleaseName(old);
    CompactNamesIfNeeded();
}

void SceneTree::Clear()
{
    m_nodes.clear();
    m_names.clear();
    m_items.clear();
    m_nameChars.clear();
    m_indexOf.clear();
    m_selectedItemCount = 0;
    m_nameGarbage = 0;
    StructureChanged();
}

void SceneTree::RebuildLinks()
{
    // Recomputes parent indices, depths and the id -> index map from the pre-order sequence and
    // subtree sizes, which fully describe the tree. One linear pass.
    m_stack.clear();
    for (uint32_t i = 0; i < Size(); ++i) {
        while (!m_stack.empty() && i >= SubtreeEnd(m_stack.back()))
            m_stack.pop_back();
        Node& node = m_nodes[i];
        node.parent = m_stack.empty() ? kNoIndex : m_stack.back();
        node.depth = static_cast<uint16_t>(m_stack.size());
        m_indexOf[node.id] = i;
        if (node.subtreeSize > 1)
            m_stack.push_back(i);
    }
}

void SceneTree::UpdateEffectiveVisibility(uint32_t first, uint32_t end)
{
    // Pre-order guarantees a parent is updated before its children.
    for (uint32_t i = first; i < end; ++i) {
        Node& node = m_nodes[i];
        const bool parentVisible = node.parent == kNoIndex || m_nodes[node.parent].Has(kEffectivelyVisible);
        if (node.Has(kVisible) && parentVisible)
            node.flags |= kEffectivelyVisible;
        else
            node.flags &= ~kEffectivelyVisible;
    }
}

void SceneTree::SetVisible(NodeId nodeId, bool visible)
{
    const uint32_t index = IndexOf(nodeId);
    if (index == kNoIndex)
        return;
    Node& node = m_nodes[index];
    if (node.Has(kVisible) == visible)
        return;
    node.flags = visible ? (node.flags | kVisible) : (node.flags & ~kVisible);
    UpdateEffectiveVisibility(index, SubtreeEnd(index));
    ++m_renderVersion;
}

void SceneTree::SetSelected(NodeId nodeId, bool selected)
{
    const uint32_t index = IndexOf(nodeId);
    if (index == kNoIndex)
        return;

    bool changed = false;
    const uint32_t end = SubtreeEnd(index);
    for (uint32_t i = index; i < end; ++i) {
        Node& node = m_nodes[i];
        if (node.Has(kSelected) == selected)
            continue;
        changed = true;
        node.flags = selected ? (node.flags | kSelected) : (node.flags & ~kSelected);
        if (!node.IsGroup())
            m_selectedItemCount += selected ? 1u : static_cast<uint32_t>(-1);
    }
    if (!selected) {
        for (uint32_t a = m_nodes[index].parent; a != kNoIndex; a = m_nodes[a].parent) {
            changed |= m_nodes[a].Has(kSelected);
            m_nodes[a].flags &= ~kSelected;
        }
    }
    if (changed)
        ++m_renderVersion;
}

void SceneTree::ClearSelection()
{
    uint8_t any = 0;
    for (Node& node : m_nodes) {
        any |= node.flags & kSelected;
        node.flags &= ~kSelected;
    }
    m_selectedItemCount = 0;
    if (any)
        ++m_renderVersion;
}

void SceneTree::SetExpanded(NodeId nodeId, bool expanded)
{
    const uint32_t index = IndexOf(nodeId);
    if (index == kNoIndex)
        return;
    Node& node = m_nodes[index];
    if (node.Has(kExpanded) == expanded)
        return;
    node.flags = expanded ? (node.flags | kExpanded) : (node.flags & ~kExpanded);
    ++m_layoutVersion;
}

uint32_t SceneTree::NextSibling(uint32_t index) const
{
    const uint32_t next = SubtreeEnd(index);
    const uint32_t parent = m_nodes[index].parent;
    const uint32_t parentEnd = parent == kNoIndex ? Size() : SubtreeEnd(parent);
    return next < parentEnd ? next : kNoIndex;
}

SceneTree::MemoryUsage SceneTree::Memory() const
{
    MemoryUsage usage;
    usage.nodes = m_nodes.capacity() * sizeof(Node);
    usage.names = m_names.capacity() * sizeof(NameRef) + m_nameChars.capacity();
    usage.nameAllocations = m_nameChars.capacity() > 0 ? 1 : 0;
    usage.nameGarbage = m_nameGarbage;
    usage.items = m_items.capacity() * sizeof(ItemData);
    usage.indexMap = m_indexOf.capacity() * sizeof(uint32_t);
    return usage;
}

bool SceneTree::Validate() const
{
    const uint32_t size = Size();
    if (m_names.size() != size || m_items.size() != size)
        return false;

    uint32_t selectedItems = 0;
    uint32_t liveIds = 0;
    for (uint32_t id = 0; id < m_indexOf.size(); ++id) {
        if (m_indexOf[id] == kNoIndex)
            continue;
        ++liveIds;
        if (m_indexOf[id] >= size || m_nodes[m_indexOf[id]].id != id)
            return false;
    }
    if (liveIds != size)
        return false;

    // Names: every handle inside the buffer, and live + garbage characters account for all of it.
    size_t liveNameChars = 0;
    for (const NameRef& ref : m_names) {
        if (size_t(ref.offset) + ref.length > m_nameChars.size())
            return false;
        liveNameChars += ref.length;
    }
    if (liveNameChars + m_nameGarbage != m_nameChars.size())
        return false;

    for (uint32_t i = 0; i < size; ++i) {
        const Node& node = m_nodes[i];
        if (node.subtreeSize == 0 || i + node.subtreeSize > size)
            return false;
        if (!node.IsGroup() && node.subtreeSize != 1)
            return false;

        // Parent link, depth and nesting.
        if (node.parent == kNoIndex) {
            if (node.depth != 0)
                return false;
        } else {
            const Node& parent = m_nodes[node.parent];
            if (node.parent >= i || !parent.IsGroup() || i >= SubtreeEnd(node.parent) || node.depth != parent.depth + 1)
                return false;
        }

        // Direct children must tile the subtree exactly.
        uint32_t child = i + 1;
        while (child < SubtreeEnd(i)) {
            if (m_nodes[child].parent != i)
                return false;
            child = SubtreeEnd(child);
        }
        if (child != SubtreeEnd(i))
            return false;

        // Cached visibility.
        const bool parentVisible = node.parent == kNoIndex || m_nodes[node.parent].Has(kEffectivelyVisible);
        if (node.Has(kEffectivelyVisible) != (node.Has(kVisible) && parentVisible))
            return false;

        if (!node.IsGroup() && node.Has(kSelected))
            ++selectedItems;
    }
    return selectedItems == m_selectedItemCount;
}

} // namespace lm
