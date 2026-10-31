#include "scene/ItemCatalog.h"

namespace lm {

namespace {

constexpr CatalogItem kCatalog[] = {
    {"Cube", "1 m block brush", NodeKind::Brush, MeshId::Cube, {0.5f, 0.5f, 0.5f}},
    {"Wall", "4 x 0.2 x 3 m wall brush", NodeKind::Brush, MeshId::Cube, {2.0f, 0.1f, 1.5f}},
    {"Floor Tile", "4 x 4 m floor slab", NodeKind::Brush, MeshId::Cube, {2.0f, 2.0f, 0.05f}},
    {"Pillar", "Round 3 m pillar", NodeKind::Mesh, MeshId::Cylinder, {0.3f, 0.3f, 1.5f}},
    {"Barrel", "Prop barrel", NodeKind::Mesh, MeshId::Cylinder, {0.4f, 0.4f, 0.6f}},
    {"Sphere", "1 m sphere prop", NodeKind::Mesh, MeshId::Sphere, {0.5f, 0.5f, 0.5f}},
    {"Light", "Point light", NodeKind::Light, MeshId::Sphere, {0.25f, 0.25f, 0.25f}},
    {"Spawn Point", "Player spawn; arrow shows facing (+X)", NodeKind::Entity, MeshId::Spawn, {0.4f, 0.4f, 0.9f}},
    {"Trigger", "2 m trigger volume (wireframe)", NodeKind::Entity, MeshId::None, {1.0f, 1.0f, 1.0f}},
};

} // namespace

std::span<const CatalogItem> ItemCatalog()
{
    return kCatalog;
}

NodeId SpawnCatalogItem(SceneTree& tree, const CatalogItem& item, NodeId parent, std::string_view name)
{
    // Pivot at the bottom center: the item rests on the ground plane at the origin.
    const Bounds bounds{{0.0f, 0.0f, item.halfExtents.z}, item.halfExtents};
    return tree.CreateItem(name, item.kind, bounds, parent, item.mesh);
}

} // namespace lm
