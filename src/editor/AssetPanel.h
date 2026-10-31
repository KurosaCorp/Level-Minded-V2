#pragma once

#include "scene/ItemCatalog.h"
#include "scene/SceneTree.h"

#include <imgui.h>

#include <string>
#include <vector>

namespace lm {

// Right-side panel listing placeable items (built-in and imported) as a grid of tiles. Clicking a
// tile spawns that item at the world origin inside `targetGroup` (the Hierarchy panel's active group).
// A status line under the search box reports the last import.
class AssetPanel {
public:
    static constexpr const char* kWindowName = "Assets";

    // Returns the node spawned this frame, or kInvalidNode.
    NodeId Draw(SceneTree& tree, const AssetCatalog& catalog, NodeId targetGroup, float dpiScale);

    // Message shown under the search box (UTF-8); errors are drawn in red.
    void SetStatus(std::string text, bool isError);
    // Scrolls to the tile of catalog item `index` on the next Draw and highlights it until the next click.
    void Highlight(size_t index);
    // Forgets per-item state (spawn counters, highlight): call after the catalog's imports were replaced.
    void Reset();

private:
    bool DrawTile(const CatalogItem& item, float tileSize, float dpiScale, bool highlighted);

    ImGuiTextFilter m_filter;
    std::vector<int> m_spawnCounts; // per catalog entry, for naming ("Cube_01", "Cube_02", ...)
    std::string m_status;
    bool m_statusIsError = false;
    size_t m_highlight = static_cast<size_t>(-1);
    bool m_scrollToHighlight = false;
};

} // namespace lm
