#include "scene/MockLevel.h"

#include "scene/SceneTree.h"

#include <cstdio>
#include <string>

namespace lm {

namespace {

Bounds Box(float x, float y, float z, float hx, float hy, float hz)
{
    return Bounds{{x, y, z}, {hx, hy, hz}};
}

std::string Numbered(const char* prefix, int n)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s_%02d", prefix, n);
    return buffer;
}

} // namespace

void BuildMockLevel(SceneTree& tree)
{
    // Z-up, meters. A 24 x 20 m two-storey building with props and gameplay markers.
    const NodeId world = tree.CreateGroup("World");

    const NodeId architecture = tree.CreateGroup("Architecture", world);
    {
        const NodeId ground = tree.CreateGroup("Ground Floor", architecture);
        tree.CreateItem("Floor_01", NodeKind::Brush, Box(0, 0, -0.1f, 12, 10, 0.1f), ground);
        tree.CreateItem("Wall_North", NodeKind::Brush, Box(0, 10, 1.5f, 12, 0.2f, 1.5f), ground);
        tree.CreateItem("Wall_South", NodeKind::Brush, Box(0, -10, 1.5f, 12, 0.2f, 1.5f), ground);
        tree.CreateItem("Wall_East", NodeKind::Brush, Box(12, 0, 1.5f, 0.2f, 10, 1.5f), ground);
        tree.CreateItem("Wall_West", NodeKind::Brush, Box(-12, 0, 1.5f, 0.2f, 10, 1.5f), ground);
        tree.CreateItem("Stairs", NodeKind::Brush, Box(-2, 7, 1.5f, 1.2f, 2.5f, 1.5f), ground);

        const NodeId upper = tree.CreateGroup("Upper Floor", architecture);
        tree.CreateItem("Floor_02", NodeKind::Brush, Box(6, 4, 3.1f, 6, 6, 0.1f), upper);
        tree.CreateItem("Railing", NodeKind::Mesh, Box(0.1f, 4, 3.7f, 0.05f, 6, 0.5f), upper);
        const float pillars[4][2] = {{0.5f, -1.5f}, {11.5f, -1.5f}, {0.5f, 9.5f}, {11.5f, 9.5f}};
        for (int i = 0; i < 4; ++i)
            tree.CreateItem(Numbered("Pillar", i + 1), NodeKind::Mesh, Box(pillars[i][0], pillars[i][1], 1.5f, 0.3f, 0.3f, 1.5f), upper,
                            MeshId::Cylinder);
    }

    const NodeId lighting = tree.CreateGroup("Lighting", world);
    tree.CreateItem("Sun", NodeKind::Light, Box(0, 0, 14, 0.6f, 0.6f, 0.6f), lighting);
    tree.CreateItem("Fill_01", NodeKind::Light, Box(-6, -5, 2.7f, 0.25f, 0.25f, 0.25f), lighting);
    tree.CreateItem("Fill_02", NodeKind::Light, Box(-6, 5, 2.7f, 0.25f, 0.25f, 0.25f), lighting);
    tree.CreateItem("Fill_03", NodeKind::Light, Box(6, 4, 5.5f, 0.25f, 0.25f, 0.25f), lighting);

    const NodeId props = tree.CreateGroup("Props", world);
    {
        const NodeId crates = tree.CreateGroup("Crates", props);
        for (int i = 0; i < 9; ++i)
            tree.CreateItem(Numbered("Crate", i + 1), NodeKind::Mesh,
                            Box(-9.0f + float(i % 3) * 1.2f, -7.5f + float(i / 3) * 1.2f, 0.5f, 0.5f, 0.5f, 0.5f), crates);
        tree.SetExpanded(crates, false);

        const NodeId barrels = tree.CreateGroup("Barrels", props);
        for (int i = 0; i < 4; ++i)
            tree.CreateItem(Numbered("Barrel", i + 1), NodeKind::Mesh, Box(-4.0f + float(i) * 1.0f, -8.5f, 0.6f, 0.4f, 0.4f, 0.6f), barrels,
                            MeshId::Cylinder);

        tree.CreateItem("Table_01", NodeKind::Mesh, Box(4, -4, 0.4f, 1.0f, 0.6f, 0.4f), props);
    }

    const NodeId gameplay = tree.CreateGroup("Gameplay", world);
    {
        tree.CreateItem("PlayerStart", NodeKind::Entity, Box(-9, 7, 0.9f, 0.4f, 0.4f, 0.9f), gameplay);
        tree.CreateItem("Trigger_Door", NodeKind::Entity, Box(0, -10, 1.5f, 1.5f, 0.8f, 1.5f), gameplay, MeshId::None);
        const NodeId spawns = tree.CreateGroup("Spawn Points", gameplay);
        const float spawnPoints[4][2] = {{8, -7}, {9, 7}, {3, 3}, {-5, 0}};
        for (int i = 0; i < 4; ++i)
            tree.CreateItem(std::string("Spawn_") + char('A' + i), NodeKind::Entity,
                            Box(spawnPoints[i][0], spawnPoints[i][1], 0.9f, 0.4f, 0.4f, 0.9f), spawns);
        tree.SetExpanded(spawns, false);
    }

    const NodeId unsorted = tree.CreateGroup("Unsorted");
    for (int i = 0; i < 3; ++i)
        tree.CreateItem(Numbered("Debris", i + 1), NodeKind::Brush, Box(16.0f + float(i) * 1.5f, -3.0f, 0.25f, 0.5f, 0.4f, 0.25f), unsorted);
}

} // namespace lm
