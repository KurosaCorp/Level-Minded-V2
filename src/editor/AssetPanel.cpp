#include "editor/AssetPanel.h"

#include "editor/NodeStyle.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace lm {

namespace {

ImU32 Shade(ImU32 color, float factor)
{
    const auto channel = [&](int shift) {
        const float value = float((color >> shift) & 0xFF) * factor;
        return ImU32(std::clamp(value, 0.0f, 255.0f)) << shift;
    };
    return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) | (color & IM_COL32_A_MASK);
}

// Small vector icon for a catalog item, centered in a square of side `size`.
void DrawItemIcon(ImDrawList* dl, ImVec2 c, float size, const CatalogItem& item)
{
    const ImU32 base = KindColor(item.kind);
    const float r = size * 0.36f;

    if (item.format) { // imported model: wireframe cube glyph + format badge
        const ImU32 color = IM_COL32(96, 196, 230, 255);
        const float s = r * 0.9f;
        const ImVec2 top(c.x, c.y - s), upperRight(c.x + s * 0.87f, c.y - s * 0.5f), lowerRight(c.x + s * 0.87f, c.y + s * 0.5f);
        const ImVec2 bottom(c.x, c.y + s), lowerLeft(c.x - s * 0.87f, c.y + s * 0.5f), upperLeft(c.x - s * 0.87f, c.y - s * 0.5f);
        const ImVec2 hexagon[6] = {top, upperRight, lowerRight, bottom, lowerLeft, upperLeft};
        dl->AddConvexPolyFilled(hexagon, 6, (color & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 50));
        dl->AddPolyline(hexagon, 6, color, std::max(1.5f, size * 0.03f), ImDrawFlags_Closed);
        for (const ImVec2& corner : {upperLeft, upperRight, bottom})
            dl->AddLine(c, corner, color, std::max(1.2f, size * 0.025f));
        const ImVec2 badgeSize = ImGui::CalcTextSize(item.format);
        const ImVec2 badgeMin(c.x + s * 0.2f, c.y + s * 0.55f);
        dl->AddRectFilled(ImVec2(badgeMin.x - 2.0f, badgeMin.y - 1.0f), ImVec2(badgeMin.x + badgeSize.x + 2.0f, badgeMin.y + badgeSize.y + 1.0f),
                          IM_COL32(30, 32, 38, 230), 3.0f);
        dl->AddText(badgeMin, color, item.format);
        return;
    }

    if (item.kind == NodeKind::Light) { // sun: disc + rays
        dl->AddCircleFilled(c, r * 0.55f, base);
        for (int i = 0; i < 8; ++i) {
            const float a = 6.2831853f * float(i) / 8.0f;
            const ImVec2 dir(std::cos(a), std::sin(a));
            dl->AddLine(ImVec2(c.x + dir.x * r * 0.75f, c.y + dir.y * r * 0.75f), ImVec2(c.x + dir.x * r, c.y + dir.y * r), base,
                        std::max(1.5f, size * 0.04f));
        }
        return;
    }

    switch (item.mesh) {
    case MeshId::Cube: { // isometric box, proportions follow the item's extents
        const float sx = item.halfExtents.x, sy = item.halfExtents.y, sz = item.halfExtents.z;
        const float k = r / std::max({sx + sy, sz * 1.6f, 0.01f});
        const float wx = sx * k, wy = sy * k, h = sz * k * 1.6f;
        const ImVec2 top(c.x + (wx - wy) * 0.87f * 0.5f, c.y - h * 0.5f - (wx + wy) * 0.25f);
        const ImVec2 left(top.x - wy * 0.87f, top.y + wy * 0.5f);
        const ImVec2 right(top.x + wx * 0.87f, top.y + wx * 0.5f);
        const ImVec2 front(left.x + wx * 0.87f, left.y + wx * 0.5f);
        const ImVec2 down(0.0f, h);
        auto off = [&](ImVec2 p) { return ImVec2(p.x + down.x, p.y + down.y); };
        dl->AddQuadFilled(top, right, front, left, Shade(base, 1.15f));
        dl->AddQuadFilled(left, front, off(front), off(left), Shade(base, 0.85f));
        dl->AddQuadFilled(front, right, off(right), off(front), Shade(base, 0.62f));
        break;
    }
    case MeshId::Cylinder: {
        const float w = r * std::min(1.0f, item.halfExtents.x / std::max(item.halfExtents.z, 0.01f) * 1.4f + 0.25f);
        const float h = r * 1.6f, e = w * 0.35f;
        const ImVec2 t(c.x, c.y - h * 0.5f), b(c.x, c.y + h * 0.5f);
        dl->AddEllipseFilled(b, ImVec2(w, e), Shade(base, 0.7f));
        dl->AddRectFilled(ImVec2(c.x - w, t.y), ImVec2(c.x + w, b.y), Shade(base, 0.85f));
        dl->AddEllipseFilled(t, ImVec2(w, e), Shade(base, 1.15f));
        break;
    }
    case MeshId::Sphere:
        dl->AddCircleFilled(c, r * 0.85f, Shade(base, 0.85f));
        dl->AddCircleFilled(ImVec2(c.x - r * 0.25f, c.y - r * 0.25f), r * 0.35f, Shade(base, 1.2f));
        break;
    case MeshId::Spawn: { // body + head + facing arrow
        const float w = r * 0.42f;
        dl->AddRectFilled(ImVec2(c.x - w, c.y - r * 0.35f), ImVec2(c.x + w, c.y + r), Shade(base, 0.85f), w * 0.5f);
        dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.55f), w * 0.9f, base);
        dl->AddTriangleFilled(ImVec2(c.x + w + r * 0.55f, c.y + r * 0.3f), ImVec2(c.x + w + r * 0.1f, c.y + r * 0.05f),
                              ImVec2(c.x + w + r * 0.1f, c.y + r * 0.55f), Shade(base, 1.15f));
        break;
    }
    case MeshId::None: { // dashed volume outline
        const ImVec2 mn(c.x - r, c.y - r), mx(c.x + r, c.y + r);
        const ImVec2 corners[4] = {mn, ImVec2(mx.x, mn.y), mx, ImVec2(mn.x, mx.y)};
        for (int edge = 0; edge < 4; ++edge) {
            const ImVec2 a = corners[edge], b = corners[(edge + 1) % 4];
            for (int dash = 0; dash < 5; ++dash) {
                const float t0 = float(dash) / 5.0f, t1 = t0 + 0.12f;
                dl->AddLine(ImVec2(a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0), ImVec2(a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1),
                            base, std::max(1.5f, size * 0.035f));
            }
        }
        dl->AddRectFilled(mn, mx, (base & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 40));
        break;
    }
    case MeshId::BuiltinCount:
    case MeshId::Default:
    default:
        break;
    }
}

// Writes "World / Props / Crates" for a group (or "Top level") into `out`. No heap allocations;
// very deep or long paths are truncated.
void FormatGroupPath(const SceneTree& tree, NodeId group, char (&out)[256])
{
    uint32_t index = tree.IndexOf(group);
    if (index == SceneTree::kNoIndex) {
        std::snprintf(out, sizeof(out), "Top level");
        return;
    }
    uint32_t chain[32];
    int depth = 0;
    for (; index != SceneTree::kNoIndex && depth < 32; index = tree.NodeAt(index).parent)
        chain[depth++] = index;

    size_t length = 0;
    out[0] = '\0';
    for (int i = depth - 1; i >= 0 && length + 1 < sizeof(out); --i) {
        const std::string_view name = tree.NameAt(chain[i]);
        const int written = std::snprintf(out + length, sizeof(out) - length, i == depth - 1 ? "%.*s" : " / %.*s",
                                          static_cast<int>(name.size()), name.data());
        if (written < 0)
            break;
        length = std::min(sizeof(out) - 1, length + static_cast<size_t>(written));
    }
}

} // namespace

void AssetPanel::SetStatus(std::string text, bool isError)
{
    m_status = std::move(text);
    m_statusIsError = isError;
}

void AssetPanel::Reset()
{
    m_spawnCounts.clear();
    m_highlight = static_cast<size_t>(-1);
    m_scrollToHighlight = false;
    m_status.clear();
    m_statusIsError = false;
}

void AssetPanel::Highlight(size_t index)
{
    m_highlight = index;
    m_scrollToHighlight = true;
}

NodeId AssetPanel::Draw(SceneTree& tree, const AssetCatalog& assets, NodeId targetGroup, float dpiScale)
{
    const std::span<const CatalogItem> catalog = assets.Items();
    if (m_spawnCounts.size() < catalog.size())
        m_spawnCounts.resize(catalog.size(), 0); // keeps the counts of existing items

    NodeId spawned = kInvalidNode;
    if (!ImGui::Begin(kWindowName)) {
        ImGui::End();
        return spawned;
    }

    ImGui::TextDisabled("Adds to");
    ImGui::SameLine();
    char path[256];
    FormatGroupPath(tree, targetGroup, path);
    ImGui::TextWrapped("%s", path);
    ImGui::SetNextItemWidth(-FLT_MIN);
    m_filter.DrawWithHint("##filter", "Search...");
    if (!m_status.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, m_statusIsError ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(0.55f, 0.85f, 0.55f, 1.0f));
        ImGui::TextWrapped("%s", m_status.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("Drop .gltf, .glb or .obj files here, or use File > Import Asset.");
    }
    ImGui::Separator();

    // The highlight of a just-imported tile lasts until the next click (no timer: the editor only
    // draws frames while something happens).
    if (!m_scrollToHighlight && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
        m_highlight = static_cast<size_t>(-1);

    if (ImGui::BeginChild("##tiles")) {
        const float tileSize = 84.0f * dpiScale;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const int columns = std::max(1, int((ImGui::GetContentRegionAvail().x + spacing) / (tileSize + spacing)));

        int column = 0;
        for (size_t i = 0; i < catalog.size(); ++i) {
            const CatalogItem& item = catalog[i];
            if (!m_filter.PassFilter(item.name))
                continue;
            if (column > 0)
                ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i));
            const bool highlighted = i == m_highlight;
            if (i == m_highlight && m_scrollToHighlight) {
                ImGui::SetScrollHereY(0.5f);
                m_scrollToHighlight = false;
            }
            if (DrawTile(item, tileSize, dpiScale, highlighted)) {
                char name[64];
                std::snprintf(name, sizeof(name), "%s_%02d", item.name, ++m_spawnCounts[i]);
                for (char& ch : name)
                    if (ch == ' ')
                        ch = '_';
                spawned = SpawnCatalogItem(tree, item, targetGroup, name);
            }
            ImGui::PopID();
            column = (column + 1) % columns;
        }
    }
    ImGui::EndChild();
    ImGui::End();
    return spawned;
}

bool AssetPanel::DrawTile(const CatalogItem& item, float tileSize, float dpiScale, bool highlighted)
{
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(tileSize, tileSize));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImVec2 max(min.x + tileSize, min.y + tileSize);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiCol background = active ? ImGuiCol_FrameBgActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg;
    dl->AddRectFilled(min, max, ImGui::GetColorU32(background), 6.0f * dpiScale);
    if (hovered)
        dl->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_ButtonHovered), 6.0f * dpiScale);
    if (highlighted) // just imported
        dl->AddRect(min, max, IM_COL32(110, 220, 120, 255), 6.0f * dpiScale, 2.0f * dpiScale);

    const float labelHeight = ImGui::GetTextLineHeight() + 6.0f * dpiScale;
    const float iconArea = tileSize - labelHeight;
    DrawItemIcon(dl, ImVec2(min.x + tileSize * 0.5f, min.y + iconArea * 0.55f), iconArea, item);

    // Centered label, clipped to the tile.
    const ImVec2 textSize = ImGui::CalcTextSize(item.name);
    const float textX = min.x + std::max(4.0f * dpiScale, (tileSize - textSize.x) * 0.5f);
    dl->PushClipRect(ImVec2(min.x + 2.0f, min.y), ImVec2(max.x - 2.0f, max.y), true);
    dl->AddText(ImVec2(textX, max.y - labelHeight + 1.0f * dpiScale), ImGui::GetColorU32(ImGuiCol_Text), item.name);
    dl->PopClipRect();

    if (hovered && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted(item.name);
        ImGui::TextDisabled("%s", item.description);
        ImGui::TextDisabled("%s, %.1f x %.1f x %.1f m", KindName(item.kind), item.halfExtents.x * 2.0f, item.halfExtents.y * 2.0f,
                            item.halfExtents.z * 2.0f);
        ImGui::TextDisabled("Click to place at the origin");
        ImGui::EndTooltip();
    }
    return clicked;
}

} // namespace lm
