#pragma once

#include "scene/SceneTree.h"

#include <DirectXMath.h>

#include <cstddef>
#include <deque>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lm {

// A placeable item type offered by the Assets panel.
struct CatalogItem {
    const char* name;        // display name, also the prefix of spawned node names
    const char* description;
    NodeKind kind;
    MeshId mesh;
    DirectX::XMFLOAT3 halfExtents; // meters
    const char* format = nullptr;  // imported models: "glTF" / "OBJ"; nullptr for built-in items
};

// The built-in items.
std::span<const CatalogItem> ItemCatalog();

// Everything the Assets panel offers: the built-in items followed by imported models.
class AssetCatalog {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);

    AssetCatalog(); // the built-in items

    std::span<const CatalogItem> Items() const { return m_items; }
    bool IsImported(size_t index) const { return m_items[index].format != nullptr; }
    // File an imported item came from (empty for built-in items).
    const std::filesystem::path& Source(size_t index) const { return m_sources[index]; }

    // Registers an imported model whose mesh is already in the MeshLibrary. The name is made unique
    // ("Chair", "Chair (2)", ...). Returns the new item's index.
    size_t AddImported(std::string_view name, std::string_view description, const char* format, MeshId mesh,
                       const DirectX::XMFLOAT3& halfExtents, const std::filesystem::path& source);
    // Index of the item imported from `source` (same file, any spelling of its path), or npos.
    size_t FindSource(const std::filesystem::path& source) const;

private:
    static std::wstring SourceKey(const std::filesystem::path& path);

    std::vector<CatalogItem> m_items;
    std::deque<std::string> m_text; // stable storage for imported names and descriptions
    std::vector<std::filesystem::path> m_sources; // parallel to m_items
    std::vector<std::wstring> m_sourceKeys;       // parallel to m_items, normalized for comparison
};

// Creates a node for `item` under `parent` (kInvalidNode = top level), standing on the ground with
// the center of its base at the world origin (0, 0, 0). Returns kInvalidNode if `parent` is invalid.
NodeId SpawnCatalogItem(SceneTree& tree, const CatalogItem& item, NodeId parent, std::string_view name);

} // namespace lm
