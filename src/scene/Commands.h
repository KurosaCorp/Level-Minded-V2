#pragma once

#include "scene/ItemCatalog.h"
#include "scene/SceneTree.h"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lm {

// Undoable edit of the scene (Command pattern). Every user edit of a SceneTree goes through one of
// these and the CommandHistory, so it can be undone and redone:
//
//   SpawnCommand, CreateGroupCommand  add a node          (undo: snapshot + remove; redo: restore)
//   DeleteCommand                     remove subtrees     (undo: restore them where they were)
//   TransformCommand                  move/rotate/scale   (before/after values of each item)
//   VisibilityCommand, RenameCommand  one node's toggle / name
//   MoveCommand                       reparent / reorder  (remembers the old parent and sibling)
//   MacroCommand                      several of the above as one step
//
// Commands refer to nodes by NodeId. Removed nodes come back with their original ids
// (SceneTree::Restore), so commands further along the history still find them.
class Command {
public:
    virtual ~Command() = default;
    // Applies the edit (also used for redo). Returns false if it changed nothing; such a command is
    // not recorded by CommandHistory::Execute.
    virtual bool Execute(SceneTree& scene) = 0;
    virtual void Undo(SceneTree& scene) = 0;
    // Short label for the Edit menu ("Move", "Delete 3 items").
    virtual const char* Name() const = 0;
    // Heap memory held by the command (snapshots, value lists), for the history's memory budget.
    virtual size_t MemoryBytes() const { return 0; }
};

// ---- Adding nodes

// Base for commands that create one node: the first Execute creates it, Undo snapshots and removes
// it, and later Executes restore that snapshot with the same NodeId.
class InsertCommand : public Command {
public:
    bool Execute(SceneTree& scene) final;
    void Undo(SceneTree& scene) final;
    size_t MemoryBytes() const override { return m_snapshot.MemoryBytes(); }
    NodeId Node() const { return m_node; }

protected:
    explicit InsertCommand(NodeId parent) : m_parent(parent) {}
    virtual NodeId Create(SceneTree& scene) = 0; // first Execute only
    NodeId Parent() const { return m_parent; }

private:
    NodeId m_parent;
    NodeId m_before = kInvalidNode;
    NodeId m_node = kInvalidNode;
    SubtreeSnapshot m_snapshot;
};

// Places a palette item (see SpawnCatalogItem) under `parent`.
class SpawnCommand final : public InsertCommand {
public:
    SpawnCommand(const CatalogItem& item, NodeId parent, std::string_view name);
    const char* Name() const override { return "Place Item"; }
    size_t MemoryBytes() const override { return InsertCommand::MemoryBytes() + m_name.capacity(); }

private:
    NodeId Create(SceneTree& scene) override;
    std::string m_name;
    NodeKind m_kind;
    MeshId m_mesh;
    DirectX::XMFLOAT3 m_halfExtents;
};

// Creates an item with a full placement (mods, scripted edits).
class CreateItemCommand final : public InsertCommand {
public:
    CreateItemCommand(std::string_view name, NodeKind kind, const Transform& transform, const DirectX::XMFLOAT3& halfExtents,
                      NodeId parent, MeshId mesh)
        : InsertCommand(parent), m_name(name), m_kind(kind), m_transform(transform), m_halfExtents(halfExtents), m_mesh(mesh)
    {
    }
    const char* Name() const override { return "Place Item"; }
    size_t MemoryBytes() const override { return InsertCommand::MemoryBytes() + m_name.capacity(); }

private:
    NodeId Create(SceneTree& scene) override { return scene.CreateItem(m_name, m_kind, m_transform, m_halfExtents, Parent(), m_mesh); }
    std::string m_name;
    NodeKind m_kind;
    Transform m_transform;
    DirectX::XMFLOAT3 m_halfExtents;
    MeshId m_mesh;
};

class CreateGroupCommand final : public InsertCommand {
public:
    CreateGroupCommand(std::string_view name, NodeId parent) : InsertCommand(parent), m_name(name) {}
    const char* Name() const override { return "New Group"; }
    size_t MemoryBytes() const override { return InsertCommand::MemoryBytes() + m_name.capacity(); }

private:
    NodeId Create(SceneTree& scene) override { return scene.CreateGroup(m_name, Parent()); }
    std::string m_name;
};

// ---- Removing nodes

// Removes the given nodes with their subtrees. Undo puts each back at its old place.
class DeleteCommand final : public Command {
public:
    explicit DeleteCommand(std::vector<NodeId> roots);
    bool Execute(SceneTree& scene) override;
    void Undo(SceneTree& scene) override;
    const char* Name() const override { return m_name; }
    size_t MemoryBytes() const override;

private:
    struct Removed {
        NodeId root;
        NodeId parent;
        NodeId before;
        SubtreeSnapshot snapshot;
    };
    std::vector<Removed> m_removed;
    char m_name[32];
};

// Delete of the top-most selected nodes (their subtrees go with them), or of `fallback` when nothing
// is selected. Null if there is nothing to delete.
std::unique_ptr<DeleteCommand> MakeDeleteSelectionCommand(const SceneTree& scene, NodeId fallback = kInvalidNode);

// ---- Changing nodes

// Sets the transforms of several items (one gizmo drag) from `before` to `after`.
class TransformCommand final : public Command {
public:
    TransformCommand(const char* name, std::vector<NodeId> nodes, std::vector<Transform> before, std::vector<Transform> after);
    bool Execute(SceneTree& scene) override;
    void Undo(SceneTree& scene) override;
    const char* Name() const override { return m_name; }
    size_t MemoryBytes() const override;

private:
    const char* m_name; // string literal
    std::vector<NodeId> m_nodes;
    std::vector<Transform> m_before;
    std::vector<Transform> m_after;
};

class VisibilityCommand final : public Command {
public:
    VisibilityCommand(NodeId node, bool visible) : m_node(node), m_visible(visible) {}
    bool Execute(SceneTree& scene) override;
    void Undo(SceneTree& scene) override;
    const char* Name() const override { return m_visible ? "Show" : "Hide"; }

private:
    NodeId m_node;
    bool m_visible;
};

class RenameCommand final : public Command {
public:
    RenameCommand(NodeId node, std::string_view name) : m_node(node), m_newName(name) {}
    bool Execute(SceneTree& scene) override;
    void Undo(SceneTree& scene) override;
    const char* Name() const override { return "Rename"; }
    size_t MemoryBytes() const override { return m_newName.capacity() + m_oldName.capacity(); }

private:
    NodeId m_node;
    std::string m_newName;
    std::string m_oldName;
};

// SceneTree::Move: reparent and/or reorder a node with its subtree.
class MoveCommand final : public Command {
public:
    MoveCommand(NodeId node, NodeId newParent, NodeId newBefore) : m_node(node), m_newParent(newParent), m_newBefore(newBefore) {}
    bool Execute(SceneTree& scene) override;
    void Undo(SceneTree& scene) override;
    const char* Name() const override { return "Reparent"; }

private:
    NodeId m_node;
    NodeId m_newParent;
    NodeId m_newBefore;
    NodeId m_oldParent = kInvalidNode;
    NodeId m_oldBefore = kInvalidNode;
};

// Several commands as one undo step (executed in order, undone in reverse).
class MacroCommand final : public Command {
public:
    MacroCommand(std::string_view name, std::vector<std::unique_ptr<Command>> commands) : m_name(name), m_commands(std::move(commands)) {}
    bool Execute(SceneTree& scene) override;
    void Undo(SceneTree& scene) override;
    const char* Name() const override { return m_name.c_str(); }
    size_t MemoryBytes() const override;
    // For sub-commands that were already executed one by one (then CommandHistory::Push the macro).
    void MarkAllApplied() { m_applied.assign(m_commands.size(), 1); }

private:
    std::string m_name;
    std::vector<std::unique_ptr<Command>> m_commands;
    std::vector<uint8_t> m_applied; // which sub-commands changed something on the last Execute
};

// ---- History

// Undo/redo history: a fixed-size ring of the most recent commands (100 by default) with a memory
// budget on top, so long sessions don't grow without bound. When either limit is reached the oldest
// steps are dropped. Recording a new command discards everything that could have been redone.
class CommandHistory {
public:
    static constexpr size_t kDefaultMaxSteps = 100;
    static constexpr size_t kDefaultMaxBytes = size_t(256) << 20; // snapshots of very large deletes

    explicit CommandHistory(size_t maxSteps = kDefaultMaxSteps, size_t maxBytes = kDefaultMaxBytes);

    // Executes `command` and records it. Returns the recorded command (owned by the history), or
    // nullptr if it changed nothing (then it is discarded).
    Command* Execute(std::unique_ptr<Command> command, SceneTree& scene);
    // Records a command whose effect is already applied (e.g. a finished gizmo drag).
    Command* Push(std::unique_ptr<Command> command);

    bool Undo(SceneTree& scene);
    bool Redo(SceneTree& scene);
    bool CanUndo() const { return m_cursor > 0; }
    bool CanRedo() const { return m_cursor < m_count; }
    const char* UndoName() const; // nullptr if nothing to undo
    const char* RedoName() const;

    void Clear();
    size_t Size() const { return m_count; }       // recorded steps (undoable + redoable)
    size_t UndoCount() const { return m_cursor; }
    size_t MaxSteps() const { return m_slots.size(); }
    size_t MemoryBytes() const { return m_bytes; }  // held by the recorded commands

private:
    struct Slot {
        std::unique_ptr<Command> command;
        size_t bytes = 0;
    };
    Slot& At(size_t step) { return m_slots[(m_head + step) % m_slots.size()]; } // 0 = oldest
    const Slot& At(size_t step) const { return m_slots[(m_head + step) % m_slots.size()]; }
    void DropOldest();
    void Remeasure(size_t step);

    std::vector<Slot> m_slots; // ring buffer, fixed size
    size_t m_head = 0;         // ring index of the oldest step
    size_t m_count = 0;        // recorded steps
    size_t m_cursor = 0;       // steps currently applied (the next Undo reverts step m_cursor - 1)
    size_t m_bytes = 0;
    size_t m_maxBytes;
};

} // namespace lm
