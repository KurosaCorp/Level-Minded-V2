#include "scene/Commands.h"

#include <algorithm>
#include <cstdio>

namespace lm {

namespace {

// Id of the sibling right after `node` (kInvalidNode if it is the last child), and of its parent.
void PlaceOf(const SceneTree& scene, NodeId node, NodeId& parent, NodeId& before)
{
    parent = kInvalidNode;
    before = kInvalidNode;
    const uint32_t index = scene.IndexOf(node);
    if (index == SceneTree::kNoIndex)
        return;
    const uint32_t parentIndex = scene.NodeAt(index).parent;
    parent = parentIndex == SceneTree::kNoIndex ? kInvalidNode : scene.NodeAt(parentIndex).id;
    const uint32_t next = scene.NextSibling(index);
    before = next == SceneTree::kNoIndex ? kInvalidNode : scene.NodeAt(next).id;
}

} // namespace

// ------------------------------------------------------------------------------------------- insert

bool InsertCommand::Execute(SceneTree& scene)
{
    if (m_node == kInvalidNode) { // first time: create the node
        m_node = Create(scene);
        return m_node != kInvalidNode;
    }
    const bool restored = scene.Restore(m_snapshot, m_parent, m_before);
    if (restored)
        m_snapshot = SubtreeSnapshot{}; // the node lives in the scene again; free the copy
    return restored;
}

void InsertCommand::Undo(SceneTree& scene)
{
    PlaceOf(scene, m_node, m_parent, m_before);
    m_snapshot = scene.Snapshot(m_node);
    scene.Remove(m_node);
}

SpawnCommand::SpawnCommand(const CatalogItem& item, NodeId parent, std::string_view name)
    : InsertCommand(parent), m_name(name), m_kind(item.kind), m_mesh(item.mesh), m_halfExtents(item.halfExtents)
{
}

NodeId SpawnCommand::Create(SceneTree& scene)
{
    CatalogItem item{};
    item.kind = m_kind;
    item.mesh = m_mesh;
    item.halfExtents = m_halfExtents;
    return SpawnCatalogItem(scene, item, Parent(), m_name);
}

// ------------------------------------------------------------------------------------------- delete

DeleteCommand::DeleteCommand(std::vector<NodeId> roots)
{
    m_removed.reserve(roots.size());
    for (const NodeId root : roots)
        m_removed.push_back(Removed{root, kInvalidNode, kInvalidNode, {}});
    if (roots.size() == 1)
        std::snprintf(m_name, sizeof(m_name), "Delete");
    else
        std::snprintf(m_name, sizeof(m_name), "Delete %zu items", roots.size());
}

bool DeleteCommand::Execute(SceneTree& scene)
{
    bool changed = false;
    for (Removed& removed : m_removed) {
        if (!scene.Contains(removed.root))
            continue; // already gone (e.g. inside another deleted root)
        PlaceOf(scene, removed.root, removed.parent, removed.before);
        removed.snapshot = scene.Snapshot(removed.root);
        scene.Remove(removed.root);
        changed = true;
    }
    return changed;
}

void DeleteCommand::Undo(SceneTree& scene)
{
    // Reverse order: each root's recorded neighbours exist again by the time it is restored.
    for (auto it = m_removed.rbegin(); it != m_removed.rend(); ++it) {
        if (it->snapshot.Empty())
            continue;
        scene.Restore(it->snapshot, it->parent, it->before);
        it->snapshot = SubtreeSnapshot{};
    }
}

size_t DeleteCommand::MemoryBytes() const
{
    size_t bytes = m_removed.capacity() * sizeof(Removed);
    for (const Removed& removed : m_removed)
        bytes += removed.snapshot.MemoryBytes();
    return bytes;
}

std::unique_ptr<DeleteCommand> MakeDeleteSelectionCommand(const SceneTree& scene, NodeId fallback)
{
    std::vector<NodeId> roots;
    for (uint32_t i = 0; i < scene.Size();) {
        if (scene.NodeAt(i).Has(SceneTree::kSelected)) {
            roots.push_back(scene.NodeAt(i).id);
            i = scene.SubtreeEnd(i); // its subtree goes with it
        } else {
            ++i;
        }
    }
    if (roots.empty() && scene.Contains(fallback))
        roots.push_back(fallback);
    if (roots.empty())
        return nullptr;
    return std::make_unique<DeleteCommand>(std::move(roots));
}

// ---------------------------------------------------------------------------------------- transform

TransformCommand::TransformCommand(const char* name, std::vector<NodeId> nodes, std::vector<Transform> before,
                                   std::vector<Transform> after)
    : m_name(name), m_nodes(std::move(nodes)), m_before(std::move(before)), m_after(std::move(after))
{
}

bool TransformCommand::Execute(SceneTree& scene)
{
    const uint64_t version = scene.TransformVersion();
    for (size_t i = 0; i < m_nodes.size() && i < m_after.size(); ++i)
        scene.SetTransform(m_nodes[i], m_after[i]);
    return scene.TransformVersion() != version;
}

void TransformCommand::Undo(SceneTree& scene)
{
    for (size_t i = 0; i < m_nodes.size() && i < m_before.size(); ++i)
        scene.SetTransform(m_nodes[i], m_before[i]);
}

size_t TransformCommand::MemoryBytes() const
{
    return m_nodes.capacity() * sizeof(NodeId) + (m_before.capacity() + m_after.capacity()) * sizeof(Transform);
}

// ------------------------------------------------------------------------------- visibility, rename

bool VisibilityCommand::Execute(SceneTree& scene)
{
    const uint32_t index = scene.IndexOf(m_node);
    if (index == SceneTree::kNoIndex || scene.NodeAt(index).Has(SceneTree::kVisible) == m_visible)
        return false;
    scene.SetVisible(m_node, m_visible);
    return true;
}

void VisibilityCommand::Undo(SceneTree& scene)
{
    scene.SetVisible(m_node, !m_visible);
}

bool RenameCommand::Execute(SceneTree& scene)
{
    const uint32_t index = scene.IndexOf(m_node);
    if (index == SceneTree::kNoIndex || scene.NameAt(index) == m_newName)
        return false;
    m_oldName.assign(scene.NameAt(index)); // copy: the view is invalidated by the rename
    scene.Rename(m_node, m_newName);
    return true;
}

void RenameCommand::Undo(SceneTree& scene)
{
    scene.Rename(m_node, m_oldName);
}

// --------------------------------------------------------------------------------------------- move

bool MoveCommand::Execute(SceneTree& scene)
{
    if (!scene.Contains(m_node) || !scene.CanMove(m_node, m_newParent))
        return false;
    PlaceOf(scene, m_node, m_oldParent, m_oldBefore);
    if (m_oldParent == m_newParent && (m_newBefore == m_oldBefore || m_newBefore == m_node))
        return false; // already there
    const uint64_t layout = scene.LayoutVersion();
    scene.Move(m_node, m_newParent, m_newBefore);
    return scene.LayoutVersion() != layout;
}

void MoveCommand::Undo(SceneTree& scene)
{
    scene.Move(m_node, m_oldParent, m_oldBefore);
}

// -------------------------------------------------------------------------------------------- macro

bool MacroCommand::Execute(SceneTree& scene)
{
    m_applied.assign(m_commands.size(), 0);
    bool changed = false;
    for (size_t i = 0; i < m_commands.size(); ++i) {
        m_applied[i] = m_commands[i]->Execute(scene) ? 1 : 0;
        changed |= m_applied[i] != 0;
    }
    return changed;
}

void MacroCommand::Undo(SceneTree& scene)
{
    for (size_t i = m_commands.size(); i-- > 0;)
        if (i < m_applied.size() && m_applied[i])
            m_commands[i]->Undo(scene);
}

size_t MacroCommand::MemoryBytes() const
{
    size_t bytes = m_commands.capacity() * sizeof(void*) + m_applied.capacity() + m_name.capacity();
    for (const auto& command : m_commands)
        bytes += sizeof(*command) + command->MemoryBytes();
    return bytes;
}

// ------------------------------------------------------------------------------------------ history

CommandHistory::CommandHistory(size_t maxSteps, size_t maxBytes) : m_slots(std::max<size_t>(maxSteps, 1)), m_maxBytes(maxBytes)
{
}

Command* CommandHistory::Execute(std::unique_ptr<Command> command, SceneTree& scene)
{
    if (!command || !command->Execute(scene))
        return nullptr;
    return Push(std::move(command));
}

Command* CommandHistory::Push(std::unique_ptr<Command> command)
{
    if (!command)
        return nullptr;
    // A new edit makes the undone steps unreachable.
    for (size_t step = m_cursor; step < m_count; ++step) {
        Slot& slot = At(step);
        m_bytes -= slot.bytes;
        slot = Slot{};
    }
    m_count = m_cursor;
    if (m_count == m_slots.size())
        DropOldest(); // ring full: forget the oldest step

    Slot& slot = At(m_count);
    slot.bytes = sizeof(*command) + command->MemoryBytes();
    slot.command = std::move(command);
    m_bytes += slot.bytes;
    ++m_count;
    m_cursor = m_count;
    while (m_bytes > m_maxBytes && m_count > 1)
        DropOldest(); // memory budget: the newest step is always kept
    return At(m_count - 1).command.get();
}

void CommandHistory::DropOldest()
{
    Slot& oldest = At(0);
    m_bytes -= oldest.bytes;
    oldest = Slot{};
    m_head = (m_head + 1) % m_slots.size();
    --m_count;
    if (m_cursor > 0)
        --m_cursor;
}

void CommandHistory::Remeasure(size_t step)
{
    // Snapshots appear and disappear as commands are undone and redone.
    Slot& slot = At(step);
    m_bytes -= slot.bytes;
    slot.bytes = sizeof(*slot.command) + slot.command->MemoryBytes();
    m_bytes += slot.bytes;
}

bool CommandHistory::Undo(SceneTree& scene)
{
    if (!CanUndo())
        return false;
    --m_cursor;
    At(m_cursor).command->Undo(scene);
    Remeasure(m_cursor);
    return true;
}

bool CommandHistory::Redo(SceneTree& scene)
{
    if (!CanRedo())
        return false;
    At(m_cursor).command->Execute(scene);
    Remeasure(m_cursor);
    ++m_cursor;
    while (m_bytes > m_maxBytes && m_cursor > 1)
        DropOldest();
    return true;
}

const char* CommandHistory::UndoName() const
{
    return CanUndo() ? At(m_cursor - 1).command->Name() : nullptr;
}

const char* CommandHistory::RedoName() const
{
    return CanRedo() ? At(m_cursor).command->Name() : nullptr;
}

void CommandHistory::Clear()
{
    for (Slot& slot : m_slots)
        slot = Slot{};
    m_head = m_count = m_cursor = 0;
    m_bytes = 0;
}

} // namespace lm
