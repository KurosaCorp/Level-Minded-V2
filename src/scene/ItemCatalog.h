#pragma once

#include "scene/SceneTree.h"

#include <DirectXMath.h>

#include <span>
#include <string_view>

namespace lm {

// A placeable item type offered by the Assets panel.
struct CatalogItem {
    const char* name;        // display name, also the prefix of spawned node names
    const char* description;
    NodeKind kind;
    MeshId mesh;
    DirectX::XMFLOAT3 halfExtents; // meters
};

// The built-in catalog (mock content until real asset loading exists).
std::span<const CatalogItem> ItemCatalog();

// Creates a node for `item` under `parent` (kInvalidNode = top level), standing on the ground with
// the center of its base at the world origin (0, 0, 0). Returns kInvalidNode if `parent` is invalid.
NodeId SpawnCatalogItem(SceneTree& tree, const CatalogItem& item, NodeId parent, std::string_view name);

} // namespace lm
