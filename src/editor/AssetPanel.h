#pragma once

#include "scene/ItemCatalog.h"
#include "scene/SceneTree.h"

#include <imgui.h>

#include <vector>

namespace lm {

// Right-side panel listing placeable items as a grid of tiles. Clicking a tile spawns that item at
// the world origin inside `targetGroup` (the Hierarchy panel's active group).
class AssetPanel {
public:
    static constexpr const char* kWindowName = "Assets";

    // Returns the node spawned this frame, or kInvalidNode.
    NodeId Draw(SceneTree& tree, NodeId targetGroup, float dpiScale);

private:
    bool DrawTile(const CatalogItem& item, float tileSize, float dpiScale);

    ImGuiTextFilter m_filter;
    std::vector<int> m_spawnCounts; // per catalog entry, for naming ("Cube_01", "Cube_02", ...)
};

} // namespace lm
