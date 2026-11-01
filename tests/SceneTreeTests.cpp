// Unit tests and a small benchmark for lm::SceneTree. Run: build\<preset>\bin\LevelMindedTests.exe
// (or `ctest` in the build directory). Exit code is non-zero on failure.

#include "import/ModelImporter.h"
#include "mods/ModRuntime.h"
#include "render/MeshGeometry.h"
#include "render/MeshLibrary.h"
#include "scene/Commands.h"
#include "scene/ItemCatalog.h"
#include "scene/MapFile.h"
#include "scene/MockLevel.h"
#include "scene/Picking.h"
#include "scene/SceneTree.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <cmath>
#include <set>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <windows.h>
#include <psapi.h>

using namespace lm;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                             \
        }                                                                             \
    } while (0)

// Names of all nodes in array (pre-order) order, with depth shown as leading dots.
std::string Dump(const SceneTree& tree)
{
    std::string out;
    for (uint32_t i = 0; i < tree.Size(); ++i) {
        out.append(tree.NodeAt(i).depth, '.');
        out += tree.NameAt(i);
        out += ' ';
    }
    return out;
}

bool Visible(const SceneTree& tree, NodeId id)
{
    return tree.NodeAt(tree.IndexOf(id)).Has(SceneTree::kEffectivelyVisible);
}

bool Selected(const SceneTree& tree, NodeId id)
{
    return tree.NodeAt(tree.IndexOf(id)).Has(SceneTree::kSelected);
}

struct Fixture {
    SceneTree tree;
    NodeId a, a1, a2, b, b1, bSub, bSub1, c;

    Fixture()
    {
        a = tree.CreateGroup("A");
        a1 = tree.CreateItem("a1", NodeKind::Brush, {});
        tree.Move(a1, a); // also exercises Move on a fresh item
        a2 = tree.CreateItem("a2", NodeKind::Mesh, {}, a);
        b = tree.CreateGroup("B");
        b1 = tree.CreateItem("b1", NodeKind::Light, {}, b);
        bSub = tree.CreateGroup("BSub", b);
        bSub1 = tree.CreateItem("bs1", NodeKind::Entity, {}, bSub);
        c = tree.CreateItem("c", NodeKind::Brush, {});
    }
};

void TestBuild()
{
    Fixture f;
    CHECK(f.tree.Validate());
    CHECK(Dump(f.tree) == "A .a1 .a2 B .b1 .BSub ..bs1 c ");
    CHECK(f.tree.NodeAt(f.tree.IndexOf(f.b)).subtreeSize == 4);
    CHECK(f.tree.NextSibling(f.tree.IndexOf(f.a)) == f.tree.IndexOf(f.b));
    CHECK(f.tree.NextSibling(f.tree.IndexOf(f.c)) == SceneTree::kNoIndex);
    CHECK(f.tree.CreateItem("bad", NodeKind::Brush, {}, f.a1) == kInvalidNode); // items can't have children
    CHECK(f.tree.CreateItem("bad", NodeKind::Group, {}) == kInvalidNode);
}

void TestInsertInMiddle()
{
    Fixture f;
    const NodeId a3 = f.tree.CreateItem("a3", NodeKind::Brush, {}, f.a); // shifts B.. right by one
    CHECK(f.tree.Validate());
    CHECK(Dump(f.tree) == "A .a1 .a2 .a3 B .b1 .BSub ..bs1 c ");
    CHECK(f.tree.NameAt(f.tree.IndexOf(a3)) == "a3");
    CHECK(f.tree.NameAt(f.tree.IndexOf(f.bSub1)) == "bs1");
}

void TestMove()
{
    {   // Re-parent an item into another group (appended last).
        Fixture f;
        CHECK(f.tree.Move(f.a1, f.bSub));
        CHECK(f.tree.Validate());
        CHECK(Dump(f.tree) == "A .a2 B .b1 .BSub ..bs1 ..a1 c ");
    }
    {   // Move a whole group to the top level, before A.
        Fixture f;
        CHECK(f.tree.Move(f.bSub, kInvalidNode, f.a));
        CHECK(f.tree.Validate());
        CHECK(Dump(f.tree) == "BSub .bs1 A .a1 .a2 B .b1 c ");
    }
    {   // Move a group forward past later nodes (to the end of the top level).
        Fixture f;
        CHECK(f.tree.Move(f.a, kInvalidNode));
        CHECK(f.tree.Validate());
        CHECK(Dump(f.tree) == "B .b1 .BSub ..bs1 c A .a1 .a2 ");
    }
    {   // Reorder siblings inside a group.
        Fixture f;
        CHECK(f.tree.Move(f.bSub, f.b, f.b1));
        CHECK(f.tree.Validate());
        CHECK(Dump(f.tree) == "A .a1 .a2 B .BSub ..bs1 .b1 c ");
    }
    {   // Move to the end of an ancestor (array order unchanged, parent changes).
        Fixture f;
        CHECK(f.tree.Move(f.bSub1, f.b));
        CHECK(f.tree.Validate());
        CHECK(Dump(f.tree) == "A .a1 .a2 B .b1 .BSub .bs1 c ");
    }
    {   // Invalid moves change nothing.
        Fixture f;
        const std::string before = Dump(f.tree);
        CHECK(!f.tree.Move(f.b, f.bSub));          // into own subtree
        CHECK(!f.tree.Move(f.b, f.b));             // into itself
        CHECK(!f.tree.Move(f.a1, f.c));            // under an item
        CHECK(!f.tree.Move(f.a1, f.a, f.b1));      // `before` is not a child of the new parent
        CHECK(f.tree.Move(f.a1, f.a, f.a2) && Dump(f.tree) == before); // already there: no-op
        CHECK(f.tree.Validate());
        CHECK(Dump(f.tree) == before);
    }
}

void TestVisibility()
{
    Fixture f;
    f.tree.SetVisible(f.b, false);
    CHECK(f.tree.Validate());
    CHECK(!Visible(f.tree, f.b) && !Visible(f.tree, f.b1) && !Visible(f.tree, f.bSub) && !Visible(f.tree, f.bSub1));
    CHECK(Visible(f.tree, f.a1) && Visible(f.tree, f.c));
    // Children keep their own toggle while hidden by the parent.
    CHECK(f.tree.NodeAt(f.tree.IndexOf(f.b1)).Has(SceneTree::kVisible));

    // A child hidden on its own stays hidden when the parent is shown again.
    f.tree.SetVisible(f.bSub, false);
    f.tree.SetVisible(f.b, true);
    CHECK(f.tree.Validate());
    CHECK(Visible(f.tree, f.b) && Visible(f.tree, f.b1));
    CHECK(!Visible(f.tree, f.bSub) && !Visible(f.tree, f.bSub1));

    // Moving a node under a hidden group hides it; moving it out shows it again.
    f.tree.SetVisible(f.a, false);
    CHECK(f.tree.Move(f.c, f.a));
    CHECK(!Visible(f.tree, f.c));
    CHECK(f.tree.Move(f.c, kInvalidNode));
    CHECK(Visible(f.tree, f.c));
    // New nodes inherit hidden-ness from their parent.
    const NodeId hiddenNew = f.tree.CreateItem("new", NodeKind::Mesh, {}, f.a);
    CHECK(!Visible(f.tree, hiddenNew));
    CHECK(f.tree.Validate());
}

void TestSelection()
{
    Fixture f;
    f.tree.SetSelected(f.b, true); // mass selection
    CHECK(f.tree.Validate());
    CHECK(Selected(f.tree, f.b) && Selected(f.tree, f.b1) && Selected(f.tree, f.bSub) && Selected(f.tree, f.bSub1));
    CHECK(!Selected(f.tree, f.a1) && !Selected(f.tree, f.c));
    CHECK(f.tree.SelectedItemCount() == 2); // b1, bs1 (groups don't count)

    // Deselecting a child deselects its ancestors but leaves siblings selected.
    f.tree.SetSelected(f.bSub1, false);
    CHECK(f.tree.Validate());
    CHECK(!Selected(f.tree, f.bSub) && !Selected(f.tree, f.b) && Selected(f.tree, f.b1));
    CHECK(f.tree.SelectedItemCount() == 1);

    // Selection survives moves and is updated by removals.
    f.tree.SetSelected(f.a, true);
    CHECK(f.tree.SelectedItemCount() == 3);
    CHECK(f.tree.Move(f.a, f.bSub));
    CHECK(f.tree.Validate() && f.tree.SelectedItemCount() == 3);
    f.tree.Remove(f.a);
    CHECK(f.tree.Validate());
    CHECK(f.tree.SelectedItemCount() == 1);

    f.tree.ClearSelection();
    CHECK(f.tree.Validate() && f.tree.SelectedItemCount() == 0);
}

void TestRemove()
{
    Fixture f;
    f.tree.Remove(f.b);
    CHECK(f.tree.Validate());
    CHECK(Dump(f.tree) == "A .a1 .a2 c ");
    CHECK(!f.tree.Contains(f.b) && !f.tree.Contains(f.bSub1));
    // Ids are never reused.
    const NodeId d = f.tree.CreateGroup("D");
    CHECK(d != f.b && d != f.bSub1);
    CHECK(f.tree.Validate());
}

// Stress: random-ish moves keep every invariant intact.
void TestManyMoves()
{
    SceneTree tree;
    std::vector<NodeId> groups{tree.CreateGroup("root")};
    std::vector<NodeId> all = groups;
    uint32_t seed = 12345;
    auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };

    for (int i = 0; i < 300; ++i) {
        const NodeId parent = groups[next() % groups.size()];
        if (next() % 3 == 0) {
            groups.push_back(tree.CreateGroup("g", parent));
            all.push_back(groups.back());
        } else {
            all.push_back(tree.CreateItem("i", NodeKind::Brush, {}, parent));
        }
    }
    int moved = 0;
    for (int i = 0; i < 2000; ++i) {
        const NodeId node = all[next() % all.size()];
        const NodeId parent = (next() % 8 == 0) ? kInvalidNode : groups[next() % groups.size()];
        moved += tree.Move(node, parent) ? 1 : 0;
        if (i % 7 == 0)
            tree.SetVisible(groups[next() % groups.size()], next() % 2 == 0);
        if (i % 11 == 0)
            tree.SetSelected(all[next() % all.size()], next() % 2 == 0);
    }
    CHECK(moved > 500);
    CHECK(tree.Validate());
}

void TestNamesAndVersions()
{
    Fixture f;
    const uint64_t layout0 = f.tree.LayoutVersion();
    const uint64_t render0 = f.tree.RenderVersion();

    // Rename, including to a long name and to a view into the tree's own name storage.
    f.tree.Rename(f.a1, "a_much_longer_name_than_any_small_string_buffer");
    CHECK(f.tree.NameAt(f.tree.IndexOf(f.a1)) == "a_much_longer_name_than_any_small_string_buffer");
    f.tree.Rename(f.a2, f.tree.NameAt(f.tree.IndexOf(f.b1))); // aliases the buffer that may grow
    CHECK(f.tree.NameAt(f.tree.IndexOf(f.a2)) == "b1");
    CHECK(f.tree.Validate());
    CHECK(f.tree.Memory().nameAllocations == 1); // all names share one buffer
    CHECK(f.tree.LayoutVersion() == layout0 && f.tree.RenderVersion() == render0); // renames don't affect either

    // Expand/collapse: layout only. Selection/visibility: render only. No-op calls change nothing.
    f.tree.SetExpanded(f.b, false);
    CHECK(f.tree.LayoutVersion() == layout0 + 1 && f.tree.RenderVersion() == render0);
    f.tree.SetExpanded(f.b, false);
    CHECK(f.tree.LayoutVersion() == layout0 + 1);
    f.tree.SetSelected(f.b, true);
    f.tree.SetVisible(f.c, false);
    CHECK(f.tree.RenderVersion() == render0 + 2 && f.tree.LayoutVersion() == layout0 + 1);
    f.tree.SetSelected(f.b, true);
    f.tree.ClearSelection();
    f.tree.ClearSelection();
    CHECK(f.tree.RenderVersion() == render0 + 3);
    // Structural edits: both.
    CHECK(f.tree.Move(f.c, f.a));
    CHECK(f.tree.LayoutVersion() == layout0 + 2 && f.tree.RenderVersion() == render0 + 4);

    // Garbage from removals/renames is reclaimed by compaction, keeping names intact.
    SceneTree tree;
    const NodeId group = tree.CreateGroup("G");
    std::vector<NodeId> items;
    for (int i = 0; i < 5000; ++i)
        items.push_back(tree.CreateItem("Item_with_a_reasonably_long_name_" + std::to_string(i), NodeKind::Brush, {}, group));
    const size_t charsBefore = tree.Memory().names;
    for (int i = 0; i < 5000; i += 2)
        tree.Remove(items[static_cast<size_t>(i)]);
    for (int round = 0; round < 3; ++round)
        for (int i = 1; i < 5000; i += 2)
            tree.Rename(items[static_cast<size_t>(i)], "Renamed_item_" + std::to_string(round) + "_" + std::to_string(i));
    CHECK(tree.Validate());
    CHECK(tree.Memory().nameGarbage < 64 * 1024 * 2); // compaction ran
    CHECK(tree.NameAt(tree.IndexOf(items[4999])) == "Renamed_item_2_4999");
    CHECK(tree.Memory().names <= charsBefore * 2);
}

void TestCatalogSpawn()
{
    const auto catalog = ItemCatalog();
    CHECK(catalog.size() >= 3);
    std::set<MeshId> meshes;
    std::set<NodeKind> kinds;
    for (const CatalogItem& item : catalog) {
        meshes.insert(item.mesh);
        kinds.insert(item.kind);
    }
    CHECK(meshes.size() >= 3 && kinds.size() >= 3); // distinct item types

    Fixture f;
    const CatalogItem& spawnPoint = catalog[catalog.size() - 2];
    const NodeId spawned = SpawnCatalogItem(f.tree, spawnPoint, f.bSub, "Spawn_Point_01");
    CHECK(spawned != kInvalidNode);
    CHECK(f.tree.Validate());
    // Appended as the last child of the target group.
    CHECK(Dump(f.tree) == "A .a1 .a2 B .b1 .BSub ..bs1 ..Spawn_Point_01 c ");
    const uint32_t index = f.tree.IndexOf(spawned);
    CHECK(f.tree.NodeAt(index).kind == spawnPoint.kind);
    CHECK(f.tree.MeshAt(index) == spawnPoint.mesh);
    // Base at the origin: centered on X/Y, resting on Z = 0.
    const ItemData& item = f.tree.ItemAt(index);
    CHECK(item.transform.position.x == 0.0f && item.transform.position.y == 0.0f &&
          item.transform.position.z == spawnPoint.halfExtents.z);
    CHECK(item.transform.scale.x == 1.0f && item.transform.rotation.w == 1.0f); // unrotated, scale 1
    CHECK(item.halfExtents.x == spawnPoint.halfExtents.x && item.halfExtents.z == spawnPoint.halfExtents.z);

    // Top level, and inheriting hidden-ness from a hidden target group.
    CHECK(SpawnCatalogItem(f.tree, catalog[0], kInvalidNode, "Cube_01") != kInvalidNode);
    f.tree.SetVisible(f.a, false);
    const NodeId hidden = SpawnCatalogItem(f.tree, catalog[0], f.a, "Cube_02");
    CHECK(!Visible(f.tree, hidden));
    // Items are not valid targets.
    CHECK(SpawnCatalogItem(f.tree, catalog[0], f.c, "bad") == kInvalidNode);
    CHECK(f.tree.Validate());

    // Default meshes by kind; explicit None stays None.
    SceneTree tree;
    BuildMockLevel(tree);
    CHECK(tree.Validate());
    for (uint32_t i = 0; i < tree.Size(); ++i) {
        const SceneTree::Node& node = tree.NodeAt(i);
        if (node.IsGroup())
            CHECK(tree.MeshAt(i) == MeshId::None);
        else if (tree.NameAt(i) == "Trigger_Door")
            CHECK(tree.MeshAt(i) == MeshId::None);
        else
            CHECK(tree.MeshAt(i) != MeshId::None && tree.MeshAt(i) != MeshId::Default);
    }
}

// Every triangle of every built-in mesh must be non-degenerate and wound counter-clockwise seen
// from outside, i.e. its geometric normal agrees with the vertex normals. The renderer culls back
// faces on that assumption.
void TestMeshWinding()
{
    const MeshGeometry geometry = BuildBuiltinMeshes();
    for (uint32_t m = 1; m < kBuiltinMeshCount; ++m) {
        const MeshRange& range = geometry.ranges[m];
        CHECK(range.indexCount > 0 && range.indexCount % 3 == 0);
        int bad = 0;
        for (uint32_t t = 0; t < range.indexCount; t += 3) {
            const MeshVertex* v[3];
            for (uint32_t k = 0; k < 3; ++k) {
                const uint32_t index = geometry.indices[range.firstIndex + t + k];
                CHECK(index < range.vertexCount);
                v[k] = &geometry.vertices[static_cast<size_t>(range.baseVertex) + index];
            }
            const DirectX::XMFLOAT3 e1{v[1]->position.x - v[0]->position.x, v[1]->position.y - v[0]->position.y,
                                       v[1]->position.z - v[0]->position.z};
            const DirectX::XMFLOAT3 e2{v[2]->position.x - v[0]->position.x, v[2]->position.y - v[0]->position.y,
                                       v[2]->position.z - v[0]->position.z};
            const DirectX::XMFLOAT3 face{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
            const float area = std::sqrt(face.x * face.x + face.y * face.y + face.z * face.z);
            float agreement = 0.0f;
            for (const MeshVertex* vertex : v)
                agreement += face.x * vertex->normal.x + face.y * vertex->normal.y + face.z * vertex->normal.z;
            if (area < 1e-7f || agreement <= 0.0f)
                ++bad;
        }
        if (bad != 0)
            std::fprintf(stderr, "mesh %u: %d bad triangles\n", m, bad);
        CHECK(bad == 0);
    }

    // Wire box: 12 edges, every index inside its 8 corners, every edge axis-aligned with length 2.
    const MeshRange& wire = geometry.wireBox;
    CHECK(wire.indexCount == 24 && wire.vertexCount == 8);
    for (uint32_t e = 0; e < wire.indexCount; e += 2) {
        const uint32_t i0 = geometry.indices[wire.firstIndex + e];
        const uint32_t i1 = geometry.indices[wire.firstIndex + e + 1];
        CHECK(i0 < 8 && i1 < 8);
        const auto& p0 = geometry.vertices[static_cast<size_t>(wire.baseVertex) + i0].position;
        const auto& p1 = geometry.vertices[static_cast<size_t>(wire.baseVertex) + i1].position;
        const float d = std::fabs(p0.x - p1.x) + std::fabs(p0.y - p1.y) + std::fabs(p0.z - p1.z);
        CHECK(d == 2.0f);
    }
}

bool Near(float a, float b, float epsilon = 1e-4f)
{
    return std::fabs(a - b) <= epsilon;
}

bool Near3(const DirectX::XMFLOAT3& v, float x, float y, float z, float epsilon = 1e-4f)
{
    return Near(v.x, x, epsilon) && Near(v.y, y, epsilon) && Near(v.z, z, epsilon);
}

DirectX::XMFLOAT3 Store3(DirectX::FXMVECTOR v)
{
    DirectX::XMFLOAT3 out;
    DirectX::XMStoreFloat3(&out, v);
    return out;
}

DirectX::XMFLOAT4 RotationZ(float degrees)
{
    DirectX::XMFLOAT4 q;
    DirectX::XMStoreFloat4(&q, DirectX::XMQuaternionRotationNormal(DirectX::g_XMIdentityR2, DirectX::XMConvertToRadians(degrees)));
    return q;
}

// The transform-tool operations used by the viewport gizmo, and the scene's transform storage.
void TestTransforms()
{
    using namespace DirectX;
    const XMVECTOR origin = XMVectorZero();

    // Right-handed rotation: +90 degrees around +Z turns +X into +Y (the shader rotates the same way).
    const XMFLOAT4 quarterTurn = RotationZ(90.0f);
    CHECK(Near3(Store3(XMVector3Rotate(g_XMIdentityR0, XMLoadFloat4(&quarterTurn))), 0, 1, 0));

    Transform t;
    t.position = {2.0f, 0.0f, 0.0f};
    CHECK(Near3(Translated(t, XMVectorSet(1, 2, 3, 0)).position, 3, 2, 3));

    // Rotating about a pivot turns both the offset and the orientation.
    const XMVECTOR quarterZ = XMQuaternionRotationNormal(g_XMIdentityR2, XM_PIDIV2);
    const Transform turned = RotatedAbout(t, origin, quarterZ);
    CHECK(Near3(turned.position, 0, 2, 0));
    const XMFLOAT3 halfOne{1.0f, 1.0f, 1.0f};
    CHECK(Near3(Store3(ItemMatrix(turned, halfOne).r[0]), 0, 1, 0)); // local +X now points along world +Y

    // A second rotation applies on top of the first, in world space: local +X goes Y -> Z.
    const Transform twice = RotatedAbout(turned, XMLoadFloat3(&turned.position), XMQuaternionRotationNormal(g_XMIdentityR0, XM_PIDIV2));
    CHECK(Near3(twice.position, 0, 2, 0)); // rotated about its own position
    CHECK(Near3(Store3(ItemMatrix(twice, halfOne).r[0]), 0, 0, 1));
    CHECK(Near(XMVectorGetX(XMQuaternionLength(XMLoadFloat4(&twice.rotation))), 1.0f));

    // Scale about a pivot along world axes.
    const Transform stretched = ScaledAbout(t, origin, XMQuaternionIdentity(), XMVectorSet(2, 1, 1, 0));
    CHECK(Near3(stretched.position, 4, 0, 0));
    CHECK(Near3(stretched.scale, 2, 1, 1));
    // A rotated item takes each factor on its local axis that lines up with the frame axis:
    // after +90 degrees around Z, its local Y lies along world X.
    const Transform rotatedStretched = ScaledAbout(turned, origin, XMQuaternionIdentity(), XMVectorSet(2, 1, 1, 0));
    CHECK(Near3(rotatedStretched.scale, 1, 2, 1));
    CHECK(Near3(rotatedStretched.position, 0, 2, 0)); // offset along Y: unaffected by an X factor
    // In the item's own frame, the factor goes to the matching local axis.
    const Transform localStretched = ScaledAbout(turned, XMLoadFloat3(&turned.position), XMLoadFloat4(&turned.rotation),
                                                 XMVectorSet(3, 1, 1, 0));
    CHECK(Near3(localStretched.scale, 3, 1, 1) && Near3(localStretched.position, 0, 2, 0));
    // Never collapses to zero.
    CHECK(ScaledAbout(t, origin, XMQuaternionIdentity(), XMVectorZero()).scale.x == kMinScale);

    // Scene storage: version bumps only on change, groups have no transform, world bounds rotate.
    SceneTree tree;
    const NodeId group = tree.CreateGroup("G");
    const NodeId wall = tree.CreateItem("Wall", NodeKind::Brush, Bounds{{1, 2, 3}, {2.0f, 0.1f, 1.5f}}, group);
    const uint32_t index = tree.IndexOf(wall);
    CHECK(Near3(tree.TransformAt(index).position, 1, 2, 3));
    CHECK(Near3(tree.WorldBoundsAt(index).halfExtents, 2.0f, 0.1f, 1.5f));
    const uint64_t renderVersion = tree.RenderVersion();
    uint64_t version = tree.TransformVersion();
    Transform wallTransform = tree.TransformAt(index);
    tree.SetTransform(wall, wallTransform);
    CHECK(tree.TransformVersion() == version && tree.TransformChanges().empty()); // unchanged value
    wallTransform.rotation = RotationZ(90.0f);
    tree.SetTransform(wall, wallTransform);
    CHECK(tree.TransformVersion() > version);
    CHECK(tree.RenderVersion() == renderVersion); // moving needs no full rebuild...
    CHECK(tree.TransformChanges().size() == 1 && tree.TransformChanges()[0] == wall); // ...just a patch
    CHECK(Near3(tree.WorldBoundsAt(index).halfExtents, 0.1f, 2.0f, 1.5f));
    CHECK(Near3(tree.WorldBoundsAt(index).center, 1, 2, 3));
    version = tree.TransformVersion();
    tree.SetTransform(group, Transform{});
    CHECK(tree.TransformVersion() == version);
    tree.SetTransform(kInvalidNode, Transform{}); // ignored
    CHECK(tree.Validate());

    // Other render changes clear the log (consumers rebuild then).
    tree.SetSelected(wall, true);
    CHECK(tree.RenderVersion() > renderVersion && tree.TransformChanges().empty());
    // A long drag can't grow the log without bound: past its cap it turns into one full rebuild.
    const uint64_t beforeDrag = tree.RenderVersion();
    for (int frame = 0; frame < 5000; ++frame) {
        wallTransform.position.x = float(frame);
        tree.SetTransform(wall, wallTransform);
    }
    CHECK(tree.RenderVersion() > beforeDrag);
    CHECK(tree.TransformChanges().size() <= 4096);
}

// Click selection: exact mesh shapes, rotation and scale, hidden items, volumes behind solids.
void TestPicking()
{
    using namespace DirectX;
    SceneTree tree;
    const NodeId cube = tree.CreateItem("Cube", NodeKind::Brush, Bounds{{0, 0, 0}, {1, 1, 1}});
    const NodeId sphere = tree.CreateItem("Sphere", NodeKind::Mesh, Bounds{{5, 0, 0}, {1, 1, 1}}, kInvalidNode, MeshId::Sphere);
    const NodeId wall = tree.CreateItem("Wall", NodeKind::Brush, Bounds{{-20, 0, 0}, {2.0f, 0.1f, 1.5f}});
    const NodeId flat = tree.CreateItem("Flat", NodeKind::Brush, Bounds{{40, 0, 0}, {1.0f, 1.0f, 0.0f}});
    const MeshLibrary meshes;

    const auto pick = [&](XMFLOAT3 origin, XMFLOAT3 direction) {
        Ray ray{origin, {}};
        XMStoreFloat3(&ray.direction, XMVector3Normalize(XMLoadFloat3(&direction)));
        return PickItem(tree, meshes, ray);
    };

    PickHit hit = pick({-10, 0, 0}, {1, 0, 0}); // the wall at x = -20 is behind the ray
    CHECK(hit.node == cube && Near(hit.distance, 9.0f));
    CHECK(pick({5, -10, 0.5f}, {0, 1, 0}).node == sphere);
    // Through the corner of the sphere's box but outside the sphere: the mesh decides, not the box.
    CHECK(!pick({5.9f, -10, 0.9f}, {0, 1, 0}));
    CHECK(!pick({0, -10, 3}, {0, 1, 0})); // above everything
    CHECK(!pick({0, 10, 0}, {0, 1, 0}));  // pointing away
    CHECK(!pick({40, 0, 10}, {0, 0, -1})); // zero-thickness item is skipped, not divided by zero

    // Wall along X: hit at x = -18.5, missed once it is turned to run along Y.
    CHECK(pick({-18.5f, -10, 0}, {0, 1, 0}).node == wall);
    Transform t = tree.TransformAt(tree.IndexOf(wall));
    t.rotation = RotationZ(90.0f);
    tree.SetTransform(wall, t);
    CHECK(!pick({-18.5f, -10, 0}, {0, 1, 0}));
    hit = pick({-20, -10, 0}, {0, 1, 0});
    CHECK(hit.node == wall && Near(hit.distance, 8.0f, 1e-3f)); // enters at y = -2
    // Scaled 2x along its own Y axis, which now lies along world X: 0.2 m thick instead of 0.1 m.
    t.scale = {1.0f, 2.0f, 1.0f};
    tree.SetTransform(wall, t);
    CHECK(pick({-20.15f, -10, 0}, {0, 1, 0}).node == wall);
    CHECK(!pick({-20.25f, -10, 0}, {0, 1, 0}));

    // Hidden items can't be clicked.
    tree.SetVisible(cube, false);
    CHECK(pick({-10, 0, 0}, {1, 0, 0}).node == sphere);
    tree.SetVisible(cube, true);

    // A trigger volume around the cube: the solid cube still wins even though the volume is nearer;
    // where no solid is hit, the volume is.
    const NodeId volume = tree.CreateItem("Trigger", NodeKind::Entity, Bounds{{0, 0, 0}, {3, 3, 3}}, kInvalidNode, MeshId::None);
    CHECK(pick({-10, 0, 0}, {1, 0, 0}).node == cube);
    CHECK(pick({-10, 0, 2}, {1, 0, 0}).node == volume);
    hit = pick({0, 0, 2}, {1, 0, 0}); // starting inside the volume: hit where the ray leaves it
    CHECK(hit.node == volume && Near(hit.distance, 3.0f));
    (void)flat;
}

// ---- Model import

std::filesystem::path TestDir()
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "LevelMindedTests";
    std::filesystem::create_directories(dir);
    return dir;
}

void WriteFile(const std::filesystem::path& path, const void* data, size_t size)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

void WriteText(const std::filesystem::path& path, const std::string& text)
{
    WriteFile(path, text.data(), text.size());
}

std::string Base64(const std::vector<uint8_t>& data)
{
    static const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t chunk = (uint32_t(data[i]) << 16) | (i + 1 < data.size() ? uint32_t(data[i + 1]) << 8 : 0u) |
                               (i + 2 < data.size() ? uint32_t(data[i + 2]) : 0u);
        out += kAlphabet[(chunk >> 18) & 63];
        out += kAlphabet[(chunk >> 12) & 63];
        out += i + 1 < data.size() ? kAlphabet[(chunk >> 6) & 63] : '=';
        out += i + 2 < data.size() ? kAlphabet[chunk & 63] : '=';
    }
    return out;
}

template <typename T>
void Append(std::vector<uint8_t>& bytes, const T* values, size_t count)
{
    const auto* raw = reinterpret_cast<const uint8_t*>(values);
    bytes.insert(bytes.end(), raw, raw + count * sizeof(T));
}

// A unit quad in glTF's XY plane, facing +Z, counter-clockwise: positions, normals, uint16 indices.
std::vector<uint8_t> QuadBuffer(bool withNormals)
{
    const float positions[] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const float normals[] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    std::vector<uint8_t> bytes;
    Append(bytes, positions, 12);
    if (withNormals)
        Append(bytes, normals, 12);
    Append(bytes, indices, 6);
    return bytes;
}

// glTF JSON for QuadBuffer: `nodes` is the JSON array of nodes (node 0 is the scene root).
std::string QuadGltf(const std::string& bufferJson, bool withNormals, const std::string& nodes)
{
    const int indexOffset = withNormals ? 96 : 48;
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":)" + nodes +
                       R"(,"meshes":[{"primitives":[{"attributes":{"POSITION":0)";
    json += withNormals ? R"(,"NORMAL":1},"indices":2,"material":0}]}],)" : R"(},"indices":1,"material":0}]}],)";
    json += R"("materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]},"doubleSided":true}],)";
    json += R"("buffers":[)" + bufferJson + "],";
    json += R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48})";
    if (withNormals)
        json += R"(,{"buffer":0,"byteOffset":48,"byteLength":48})";
    json += R"(,{"buffer":0,"byteOffset":)" + std::to_string(indexOffset) + R"(,"byteLength":12}],)";
    json += R"("accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0]})";
    if (withNormals)
        json += R"(,{"bufferView":1,"componentType":5126,"count":4,"type":"VEC3"})";
    json += R"(,{"bufferView":)" + std::to_string(withNormals ? 2 : 1) + R"(,"componentType":5123,"count":6,"type":"SCALAR"}]})";
    return json;
}

// Every triangle's winding agrees with its vertex normals (counter-clockwise seen from outside).
bool WindingMatchesNormals(const ImportedModel& model)
{
    for (size_t t = 0; t + 2 < model.indices.size(); t += 3) {
        const MeshVertex& a = model.vertices[model.indices[t]];
        const MeshVertex& b = model.vertices[model.indices[t + 1]];
        const MeshVertex& c = model.vertices[model.indices[t + 2]];
        const DirectX::XMVECTOR face = DirectX::XMVector3Cross(
            DirectX::XMVectorSubtract(DirectX::XMLoadFloat3(&b.position), DirectX::XMLoadFloat3(&a.position)),
            DirectX::XMVectorSubtract(DirectX::XMLoadFloat3(&c.position), DirectX::XMLoadFloat3(&a.position)));
        float agreement = 0.0f;
        for (const MeshVertex* v : {&a, &b, &c})
            agreement += DirectX::XMVectorGetX(DirectX::XMVector3Dot(face, DirectX::XMLoadFloat3(&v->normal)));
        if (!(agreement > 0.0f))
            return false;
    }
    return true;
}

bool InsideUnitBox(const ImportedModel& model)
{
    for (const MeshVertex& v : model.vertices)
        if (std::fabs(v.position.x) > 1.0001f || std::fabs(v.position.y) > 1.0001f || std::fabs(v.position.z) > 1.0001f)
            return false;
    return true;
}

void TestImportObj()
{
    const std::filesystem::path dir = TestDir();
    // A 2 x 4 x 6 box in OBJ's Y-up space (x width, y height, z depth): quads, no normals, one
    // material, the last face with relative (negative) indices.
    WriteText(dir / "box.mtl", "newmtl Purple\nKd 0.5 0.25 1.0\n");
    WriteText(dir / "box.obj",
              "mtllib box.mtl\n"
              "v -1 -2 3\nv 1 -2 3\nv 1 2 3\nv -1 2 3\nv -1 -2 -3\nv 1 -2 -3\nv 1 2 -3\nv -1 2 -3\n"
              "usemtl Purple\n"
              "f 1 2 3 4\nf 6 5 8 7\nf 2 6 7 3\nf 5 1 4 8\nf 4 3 7 8\nf -4 -3 -7 -8\n");
    const ImportResult result = ImportModel(dir / "box.obj");
    CHECK(result.ok);
    const ImportedModel& model = result.model;
    CHECK(model.name == "box" && std::string(model.format) == "OBJ");
    CHECK(model.indices.size() == 36); // 6 quads -> 12 triangles
    CHECK(model.vertices.size() == 8); // corners welded (same position, no normal, same material)
    // Y-up -> Z-up: the 4 m height becomes Z, the 6 m depth becomes Y.
    CHECK(Near3(model.halfExtents, 1.0f, 3.0f, 2.0f));
    CHECK(InsideUnitBox(model));
    CHECK(WindingMatchesNormals(model));
    const uint32_t purple = model.vertices[0].color; // Kd (linear) -> sRGB
    CHECK((purple & 0xFF) == 188 && ((purple >> 8) & 0xFF) == 137 && ((purple >> 16) & 0xFF) == 255);
    CHECK(model.traits == 0);

    // It can be added to the mesh library and clicked like a built-in mesh.
    MeshLibrary meshes;
    const uint64_t version = meshes.Version();
    const MeshId id = meshes.Add(model.vertices, model.indices, model.traits);
    CHECK(id == MeshId::BuiltinCount && IsImportedMesh(id) && meshes.Version() > version);
    CHECK(meshes.Count() == kBuiltinMeshCount + 1);
    CHECK(meshes.Get(id).range.indexCount == 36 && meshes.Get(id).range.vertexCount == 8);
    CHECK(meshes.Vertices().size() == size_t(meshes.Get(id).range.baseVertex) + 8);
    SceneTree tree;
    const NodeId item = tree.CreateItem("Box", NodeKind::Mesh, Bounds{{0, 0, 0}, model.halfExtents}, kInvalidNode, id);
    Ray ray{{0.0f, -10.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    const PickHit hit = PickItem(tree, meshes, ray);
    CHECK(hit.node == item && Near(hit.distance, 7.0f, 1e-3f)); // front face at y = -3

    // Invalid meshes are refused.
    const uint32_t badIndices[] = {0, 1, 99};
    CHECK(meshes.Add(model.vertices, badIndices) == MeshId::None);
    CHECK(meshes.Add({}, {}) == MeshId::None);

    // Non-ASCII file names work (the loaders get UTF-8 paths; files are opened with wide-char APIs).
    const std::filesystem::path unicode = dir / std::filesystem::path(u8"modèle_ü.obj");
    std::filesystem::copy_file(dir / "box.obj", unicode, std::filesystem::copy_options::overwrite_existing);
    const ImportResult unicodeResult = ImportModel(unicode);
    CHECK(unicodeResult.ok && unicodeResult.model.indices.size() == 36);
    CHECK(unicodeResult.model.name == "mod\xC3\xA8le_\xC3\xBC");

    // Errors are reported, not crashed on.
    WriteText(dir / "empty.obj", "# nothing here\nv 0 0 0\n");
    const ImportResult empty = ImportModel(dir / "empty.obj");
    CHECK(!empty.ok && !empty.error.empty() && empty.model.vertices.empty());
    CHECK(!ImportModel(dir / "missing.obj").ok);
    CHECK(!ImportModel(dir / "box.mtl").ok); // unsupported extension
    CHECK(IsImportableModel("a/b/Model.GLB") && IsImportableModel("x.obj") && !IsImportableModel("x.fbx"));
}

void TestImportGltf()
{
    const std::filesystem::path dir = TestDir();
    // Node 0 scales by (2, 1, 1), its child node 1 by (1, 3, 1) and holds the quad: the baked quad
    // is 2 x 3 in glTF's XY plane, which is the XZ plane after the Z-up conversion.
    const std::string nested = R"([{"children":[1],"scale":[2,1,1]},{"mesh":0,"scale":[1,3,1]}])";
    const std::vector<uint8_t> buffer = QuadBuffer(true);
    const std::string embedded = R"({"byteLength":)" + std::to_string(buffer.size()) +
                                 R"(,"uri":"data:application/octet-stream;base64,)" + Base64(buffer) + R"("})";
    WriteText(dir / "quad.gltf", QuadGltf(embedded, true, nested));
    ImportResult result = ImportModel(dir / "quad.gltf");
    CHECK(result.ok);
    if (result.ok) {
        const ImportedModel& model = result.model;
        CHECK(std::string(model.format) == "glTF" && model.skippedPrimitives == 0);
        CHECK(model.indices.size() == 6 && model.vertices.size() == 4);
        CHECK(Near(model.halfExtents.x, 1.0f) && Near(model.halfExtents.z, 1.5f) && model.halfExtents.y < 0.01f);
        CHECK(InsideUnitBox(model) && WindingMatchesNormals(model));
        for (const MeshVertex& v : model.vertices)
            CHECK(Near3(v.normal, 0.0f, -1.0f, 0.0f)); // glTF +Z (towards the viewer) is -Y in Z-up
        CHECK(model.vertices[0].color == 0xFF0000FFu);  // baseColorFactor red, opaque
        CHECK((model.traits & kMeshTraitDoubleSided) != 0);
    }

    // The same file as binary glTF (.glb) with an external-free BIN chunk.
    {
        const std::string json = QuadGltf(R"({"byteLength":)" + std::to_string(buffer.size()) + "}", true, nested);
        std::string paddedJson = json;
        while (paddedJson.size() % 4)
            paddedJson += ' ';
        std::vector<uint8_t> bin = buffer;
        while (bin.size() % 4)
            bin.push_back(0);
        std::vector<uint8_t> glb;
        const uint32_t header[3] = {0x46546C67u, 2u, uint32_t(12 + 8 + paddedJson.size() + 8 + bin.size())};
        Append(glb, header, 3);
        const uint32_t jsonChunk[2] = {uint32_t(paddedJson.size()), 0x4E4F534Au};
        Append(glb, jsonChunk, 2);
        Append(glb, paddedJson.data(), paddedJson.size());
        const uint32_t binChunk[2] = {uint32_t(bin.size()), 0x004E4942u};
        Append(glb, binChunk, 2);
        Append(glb, bin.data(), bin.size());
        WriteFile(dir / "quad.glb", glb.data(), glb.size());
        const ImportResult glbResult = ImportModel(dir / "quad.glb");
        CHECK(glbResult.ok && glbResult.model.indices.size() == 6 && Near(glbResult.model.halfExtents.z, 1.5f));
    }

    // External .bin buffer next to the .gltf, and a mirrored node (negative scale): winding must be
    // flipped back so the quad still faces the way its normals say.
    WriteFile(dir / "quad.bin", buffer.data(), buffer.size());
    const std::string external = R"({"byteLength":)" + std::to_string(buffer.size()) + R"(,"uri":"quad.bin"})";
    WriteText(dir / "mirrored.gltf", QuadGltf(external, true, R"([{"mesh":0,"scale":[-1,1,1]}])"));
    result = ImportModel(dir / "mirrored.gltf");
    CHECK(result.ok && WindingMatchesNormals(result.model));

    // No normals: flat normals are generated, one vertex per triangle corner.
    const std::vector<uint8_t> bare = QuadBuffer(false);
    const std::string bareBuffer = R"({"byteLength":)" + std::to_string(bare.size()) +
                                   R"(,"uri":"data:application/octet-stream;base64,)" + Base64(bare) + R"("})";
    WriteText(dir / "flat.gltf", QuadGltf(bareBuffer, false, R"([{"mesh":0}])"));
    result = ImportModel(dir / "flat.gltf");
    CHECK(result.ok && result.model.vertices.size() == 6 && WindingMatchesNormals(result.model));

    // Broken files and missing buffers are reported.
    WriteText(dir / "broken.gltf", "{ this is not json");
    result = ImportModel(dir / "broken.gltf");
    CHECK(!result.ok && !result.error.empty());
    WriteText(dir / "nobin.gltf", QuadGltf(R"({"byteLength":108,"uri":"does-not-exist.bin"})", true, R"([{"mesh":0}])"));
    result = ImportModel(dir / "nobin.gltf");
    CHECK(!result.ok && !result.error.empty());
}

void TestAssetCatalog()
{
    AssetCatalog catalog;
    const size_t builtins = catalog.Items().size();
    CHECK(builtins == ItemCatalog().size() && !catalog.IsImported(0));
    const std::filesystem::path source = TestDir() / "box.obj";
    const size_t a = catalog.AddImported("Cube", "imported", "OBJ", MeshId::BuiltinCount, {1, 2, 3}, source);
    CHECK(a == builtins && catalog.IsImported(a));
    CHECK(std::string(catalog.Items()[a].name) == "Cube (2)"); // "Cube" is a built-in item
    const size_t b = catalog.AddImported("Cube", "imported", "OBJ", MeshId::BuiltinCount, {1, 2, 3}, TestDir() / "x.obj");
    CHECK(std::string(catalog.Items()[b].name) == "Cube (3)");
    CHECK(std::string(catalog.Items()[a].name) == "Cube (2)"); // names stay valid as the catalog grows
    CHECK(catalog.FindSource(source) == a);
    CHECK(catalog.FindSource(TestDir() / "BOX.OBJ") == a);           // case-insensitive
    CHECK(catalog.FindSource(TestDir() / "sub" / ".." / "box.obj") == a); // any spelling of the path
    CHECK(catalog.FindSource(TestDir() / "other.obj") == AssetCatalog::npos);
    CHECK(catalog.Source(a) == source && catalog.Source(0).empty());
}

// ---- Map files

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Same structure, names, saved flags, exact transforms and meshes (ids compared through `sameMesh`).
template <typename MeshCompare>
bool SameScene(const SceneTree& a, const SceneTree& b, MeshCompare&& sameMesh)
{
    if (a.Size() != b.Size())
        return false;
    constexpr uint8_t kSaved = SceneTree::kVisible | SceneTree::kEffectivelyVisible | SceneTree::kExpanded;
    for (uint32_t i = 0; i < a.Size(); ++i) {
        const SceneTree::Node& x = a.NodeAt(i);
        const SceneTree::Node& y = b.NodeAt(i);
        if (x.kind != y.kind || x.depth != y.depth || x.subtreeSize != y.subtreeSize || x.parent != y.parent ||
            (x.flags & kSaved) != (y.flags & kSaved) || a.NameAt(i) != b.NameAt(i))
            return false;
        if (!x.IsGroup()) {
            const ItemData& p = a.ItemAt(i);
            const ItemData& q = b.ItemAt(i);
            if (std::memcmp(&p.transform, &q.transform, sizeof(Transform)) != 0 ||
                std::memcmp(&p.halfExtents, &q.halfExtents, sizeof(p.halfExtents)) != 0 || !sameMesh(p.mesh, q.mesh))
                return false;
        }
    }
    return true;
}

bool SameBuiltinMesh(MeshId a, MeshId b)
{
    return a == b;
}

void TestMapRoundTrip()
{
    const std::filesystem::path dir = TestDir() / "maps";
    std::filesystem::create_directories(dir);

    // The sample level plus everything a map must keep: rotations, scales, odd floats, hidden and
    // collapsed groups, an empty group, top-level items, names that need escaping.
    SceneTree scene;
    BuildMockLevel(scene);
    for (uint32_t i = 0, n = 0; i < scene.Size(); ++i) {
        if (scene.NodeAt(i).IsGroup())
            continue;
        Transform t = scene.TransformAt(i);
        DirectX::XMStoreFloat4(&t.rotation, DirectX::XMQuaternionRotationRollPitchYaw(0.1f * float(n), 0.37f * float(n), 1.3f));
        t.scale = {1.0f + 0.013f * float(n), 0.3333333f, 2.0f};
        t.position.x += 0.1f; // not exactly representable
        scene.SetTransformAt(i, t);
        if (++n == 12)
            break;
    }
    const NodeId lighting = scene.NodeAt(scene.SubtreeEnd(1)).id; // a sub-group of World
    scene.SetVisible(lighting, false);
    scene.SetExpanded(scene.NodeAt(1).id, false);
    const NodeId empty = scene.CreateGroup("Empty group");
    const NodeId odd = scene.CreateItem("Quote \"q\" back\\slash\ttab \xC3\xBC \xE6\x97\xA5\xE6\x9C\xAC", NodeKind::Light,
                                        Bounds{{-0.0f, 1e-7f, 123456.789f}, {0.25f, 0.25f, 0.25f}});
    scene.SetVisible(odd, false);
    (void)empty;
    scene.SetSelected(scene.NodeAt(0).id, true); // selection is editor state, not saved

    const AssetCatalog catalog;
    MapCamera camera;
    camera.target = {1.5f, -2.25f, 0.1f};
    camera.yaw = -1.0471976f;
    camera.pitch = 0.5235988f;
    camera.distance = 34.0f;
    std::string error;
    MapSaveInfo info;
    CHECK(SaveMap(dir / "level.lmmap", scene, catalog, camera, error, &info) && error.empty());
    CHECK(info.nodes == scene.Size() && info.bytes > 0 && info.assets == 0);
    const std::string saved = ReadText(dir / "level.lmmap");
    CHECK(saved.size() == info.bytes);
    CHECK(saved.find("\"format\": \"LevelMinded map\"") != std::string::npos);
    CHECK(saved.find("\"visible\": false") != std::string::npos && saved.find("\"expanded\": false") != std::string::npos);
    CHECK(!std::filesystem::exists(dir / "level.lmmap.saving")); // temporary file replaced the target

    // Load into a different editor state: everything is replaced and comes back exactly.
    SceneTree loaded;
    BuildStressLevel(loaded, 1000); // old contents that must disappear
    const uint64_t renderBefore = loaded.RenderVersion();
    AssetCatalog loadedCatalog;
    MeshLibrary meshes;
    MapLoadReport report;
    CHECK(LoadMap(dir / "level.lmmap", loaded, loadedCatalog, meshes, report, error));
    CHECK(loaded.Validate());
    CHECK(SameScene(scene, loaded, SameBuiltinMesh));
    CHECK(loaded.SelectedItemCount() == 0);
    CHECK(loaded.RenderVersion() > renderBefore); // caches of the old scene are invalidated
    CHECK(report.nodes == scene.Size() && report.assets == 0 && report.missingAssets.empty());
    CHECK(report.hasCamera && report.camera.yaw == camera.yaw && report.camera.target.y == camera.target.y &&
          report.camera.distance == camera.distance);

    // Saving what was loaded writes the identical file: nothing is lost or rounded.
    CHECK(WriteMapJson(loaded, loadedCatalog, report.camera, dir) == saved);

    // Hand-written maps: fields in any order, optional fields left out, a UTF-8 BOM, unknown keys.
    WriteText(dir / "hand.lmmap",
              "\xEF\xBB\xBF{\"nodes\": [{\"children\": [{\"halfExtents\": [1, 2, 3], \"name\": \"Box\", \"type\": \"brush\", "
              "\"mesh\": \"cube\", \"position\": [0, 0, 3], \"future\": {\"x\": [1]}}], \"name\": \"G\", \"type\": \"group\"}], "
              "\"version\": 1, \"format\": \"LevelMinded map\", \"comment\": \"hi\"}");
    CHECK(LoadMap(dir / "hand.lmmap", loaded, loadedCatalog, meshes, report, error));
    CHECK(loaded.Size() == 2 && loaded.NameAt(0) == "G" && loaded.NameAt(1) == "Box" && loaded.NodeAt(1).parent == 0);
    CHECK(Near3(loaded.ItemAt(1).halfExtents, 1, 2, 3) && loaded.ItemAt(1).transform.scale.x == 1.0f &&
          loaded.ItemAt(1).mesh == MeshId::Cube && !report.hasCamera);

    // Broken or foreign files are refused and leave the current scene exactly as it was.
    const uint64_t content = loaded.ContentVersion();
    const auto refused = [&](const char* name, const std::string& json) {
        WriteText(dir / name, json);
        std::string why;
        const bool ok = LoadMap(dir / name, loaded, loadedCatalog, meshes, report, why);
        return !ok && !why.empty() && loaded.Size() == 2 && loaded.ContentVersion() == content && loaded.Validate();
    };
    const std::string head = "{\"format\": \"LevelMinded map\", \"version\": 1, ";
    CHECK(refused("broken.lmmap", "{\"format\": \"LevelMinded map\", \"nodes\": ["));
    CHECK(refused("foreign.lmmap", "{\"format\": \"Something else\", \"version\": 1, \"nodes\": []}"));
    CHECK(refused("future.lmmap", "{\"format\": \"LevelMinded map\", \"version\": 99, \"nodes\": []}"));
    CHECK(refused("noversion.lmmap", "{\"format\": \"LevelMinded map\", \"nodes\": []}"));
    CHECK(refused("badasset.lmmap", head + "\"nodes\": [{\"type\": \"mesh\", \"name\": \"x\", \"asset\": 3}]}"));
    CHECK(refused("badtype.lmmap", head + "\"nodes\": [{\"type\": \"spaceship\", \"name\": \"x\"}]}"));
    CHECK(refused("itemkids.lmmap", head + "\"nodes\": [{\"type\": \"brush\", \"name\": \"x\", \"children\": []}]}"));
    CHECK(refused("badnumber.lmmap", head + "\"nodes\": [{\"type\": \"brush\", \"name\": \"x\", \"position\": [1, \"2\", 3]}]}"));
    CHECK(refused("shortvec.lmmap", head + "\"nodes\": [{\"type\": \"brush\", \"name\": \"x\", \"position\": [1, 2]}]}"));
    CHECK(refused("notjson.lmmap", "this is not a map"));
    std::string why;
    CHECK(!LoadMap(dir / "does-not-exist.lmmap", loaded, loadedCatalog, meshes, report, why) && loaded.Size() == 2);

    // Clear really frees the tree's memory (not just its contents).
    loaded.Clear();
    CHECK(loaded.Memory().Total() == 0 && loaded.Size() == 0 && loaded.Validate());
}

void TestMapAssets()
{
    const std::filesystem::path dir = TestDir() / "maps";
    std::filesystem::create_directories(dir / "models");
    std::filesystem::copy_file(TestDir() / "box.obj", dir / "models" / "box.obj", std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(TestDir() / "box.mtl", dir / "models" / "box.mtl", std::filesystem::copy_options::overwrite_existing);

    // An imported model in the palette and two items using it.
    MeshLibrary meshes;
    AssetCatalog catalog;
    const ImportResult box = ImportModel(dir / "models" / "box.obj");
    CHECK(box.ok);
    const MeshId boxMesh = meshes.Add(box.model.vertices, box.model.indices, box.model.traits);
    const size_t boxItem = catalog.AddImported(box.model.name, "box", box.model.format, boxMesh, box.model.halfExtents,
                                               dir / "models" / "box.obj");
    SceneTree scene;
    const NodeId group = scene.CreateGroup("Props");
    SpawnCatalogItem(scene, catalog.Items()[boxItem], group, "Box_01");
    const NodeId second = SpawnCatalogItem(scene, catalog.Items()[boxItem], group, "Box_02");
    Transform t = scene.TransformAt(scene.IndexOf(second));
    t.position = {5.0f, 0.0f, 2.0f};
    scene.SetTransform(second, t);
    scene.CreateItem("Crate", NodeKind::Brush, Bounds{{0, 3, 0.5f}, {0.5f, 0.5f, 0.5f}}, group);

    std::string error;
    CHECK(SaveMap(dir / "assets.lmmap", scene, catalog, MapCamera{}, error));
    const std::string saved = ReadText(dir / "assets.lmmap");
    CHECK(saved.find("\"path\": \"models/box.obj\"") != std::string::npos); // relative to the map
    CHECK(saved.find("\"asset\": 0") != std::string::npos);

    // Loading re-imports the model into a fresh library: same items, now pointing at the new mesh id.
    SceneTree loaded;
    AssetCatalog loadedCatalog;
    MeshLibrary loadedMeshes;
    loadedMeshes.Add(box.model.vertices, box.model.indices); // an old imported mesh that must go away
    const uint64_t generation = loadedMeshes.Generation();
    MapLoadReport report;
    CHECK(LoadMap(dir / "assets.lmmap", loaded, loadedCatalog, loadedMeshes, report, error));
    CHECK(loadedMeshes.Generation() > generation && loadedMeshes.Count() == kBuiltinMeshCount + 1);
    CHECK(report.assets == 1 && report.missingAssets.empty());
    CHECK(loadedCatalog.Items().size() == ItemCatalog().size() + 1);
    const CatalogItem& asset = loadedCatalog.Items().back();
    CHECK(std::string(asset.name) == "box" && std::string(asset.format) == "OBJ" && IsImportedMesh(asset.mesh));
    CHECK(loadedMeshes.Get(asset.mesh).range.indexCount == 36);
    const auto sameAsset = [&](MeshId a, MeshId b) { return a == boxMesh ? b == asset.mesh : a == b; };
    CHECK(SameScene(scene, loaded, sameAsset));
    CHECK(WriteMapJson(loaded, loadedCatalog, report.camera, dir) == saved);

    // Map moved elsewhere: the relative path fails, the absolute path still finds the model.
    std::filesystem::create_directories(dir / "elsewhere");
    std::filesystem::copy_file(dir / "assets.lmmap", dir / "elsewhere" / "assets.lmmap", std::filesystem::copy_options::overwrite_existing);
    CHECK(LoadMap(dir / "elsewhere" / "assets.lmmap", loaded, loadedCatalog, loadedMeshes, report, error));
    CHECK(report.missingAssets.empty() && loadedMeshes.Get(loadedCatalog.Items().back().mesh).range.indexCount == 36);

    // Model file gone: the map still loads, the items become boxes of the recorded size, and the
    // asset entry (with its path) survives a re-save.
    std::filesystem::remove(dir / "models" / "box.obj");
    CHECK(LoadMap(dir / "assets.lmmap", loaded, loadedCatalog, loadedMeshes, report, error));
    CHECK(report.missingAssets.size() == 1 && report.missingAssets[0] == "box");
    CHECK(loaded.Size() == scene.Size() && IsImportedMesh(loaded.ItemAt(1).mesh));
    CHECK(loadedMeshes.Get(loaded.ItemAt(1).mesh).range.indexCount == loadedMeshes.Get(MeshId::Cube).range.indexCount);
    CHECK(Near3(loaded.ItemAt(1).halfExtents, box.model.halfExtents.x, box.model.halfExtents.y, box.model.halfExtents.z));
    const std::string resaved = WriteMapJson(loaded, loadedCatalog, report.camera, dir);
    CHECK(resaved.find("\"path\": \"models/box.obj\"") != std::string::npos && resaved.find("\"asset\": 0") != std::string::npos);
}

// ---- Undo / redo

// Everything a map saves, as text: an exact fingerprint of a scene's state.
std::string SceneState(const SceneTree& scene)
{
    static const AssetCatalog catalog;
    return WriteMapJson(scene, catalog, MapCamera{}, {});
}

std::vector<NodeId> NodeIds(const SceneTree& scene)
{
    std::vector<NodeId> ids;
    for (uint32_t i = 0; i < scene.Size(); ++i)
        ids.push_back(scene.NodeAt(i).id);
    return ids;
}

NodeId FindByName(const SceneTree& scene, std::string_view name)
{
    for (uint32_t i = 0; i < scene.Size(); ++i)
        if (scene.NameAt(i) == name)
            return scene.NodeAt(i).id;
    return kInvalidNode;
}

void TestSnapshotRestore()
{
    SceneTree scene;
    BuildMockLevel(scene);
    const NodeId architecture = FindByName(scene, "Architecture");
    scene.SetVisible(FindByName(scene, "Upper Floor"), false);
    Transform t = scene.TransformAt(scene.IndexOf(FindByName(scene, "Stairs")));
    t.rotation = RotationZ(30.0f);
    scene.SetTransform(FindByName(scene, "Stairs"), t);
    const std::string before = SceneState(scene);
    const std::vector<NodeId> idsBefore = NodeIds(scene);

    // Remove a nested group, put it back at the same place: identical, same ids.
    const uint32_t index = scene.IndexOf(architecture);
    const NodeId parent = scene.NodeAt(scene.NodeAt(index).parent).id;
    const NodeId before_ = scene.NodeAt(scene.NextSibling(index)).id;
    const SubtreeSnapshot snapshot = scene.Snapshot(architecture);
    CHECK(snapshot.nodes.size() == scene.NodeAt(index).subtreeSize && snapshot.MemoryBytes() > 0);
    scene.Remove(architecture);
    CHECK(!scene.Contains(architecture) && scene.Validate());
    CHECK(scene.Restore(snapshot, parent, before_));
    CHECK(scene.Validate() && SceneState(scene) == before && NodeIds(scene) == idsBefore);
    // Hidden children stay hidden under a hidden parent; effective visibility is recomputed.
    CHECK(!Visible(scene, FindByName(scene, "Floor_02")));

    // Restoring over a live node, into an item, or before a node of another parent is refused.
    CHECK(!scene.Restore(snapshot, parent, before_)); // ids are in use again
    const SubtreeSnapshot stairs = scene.Snapshot(FindByName(scene, "Stairs"));
    const NodeId stairsId = FindByName(scene, "Stairs");
    scene.Remove(stairsId);
    CHECK(!scene.Restore(stairs, FindByName(scene, "Floor_01"), kInvalidNode)); // items can't be parents
    CHECK(!scene.Restore(stairs, FindByName(scene, "Lighting"), FindByName(scene, "Floor_01"))); // wrong sibling
    CHECK(scene.Restore(stairs, kInvalidNode, kInvalidNode) && scene.Validate()); // top level works
    CHECK(scene.NameAt(scene.Size() - 1) == "Stairs" && scene.Contains(stairsId));
}

void TestUndoRedo()
{
    SceneTree scene;
    BuildMockLevel(scene);
    CommandHistory history;
    std::vector<std::string> states{SceneState(scene)};
    const auto step = [&](std::unique_ptr<Command> command) {
        const Command* recorded = history.Execute(std::move(command), scene);
        CHECK(recorded != nullptr && scene.Validate());
        states.push_back(SceneState(scene));
        return recorded;
    };

    // Spawn, move it, hide a group, rename, reparent, new group, delete two subtrees at once.
    const NodeId world = FindByName(scene, "World");
    const CatalogItem& cubeItem = ItemCatalog()[0];
    const NodeId cube = static_cast<const SpawnCommand*>(step(std::make_unique<SpawnCommand>(cubeItem, world, "Cube_01")))->Node();
    CHECK(scene.Contains(cube) && scene.NodeAt(scene.NodeAt(scene.IndexOf(cube)).parent).id == world);
    const Transform original = scene.TransformAt(scene.IndexOf(cube));
    Transform moved = original;
    moved.position = {3.25f, -1.5f, 2.0f};
    moved.rotation = RotationZ(45.0f);
    step(std::make_unique<TransformCommand>("Move", std::vector<NodeId>{cube}, std::vector<Transform>{original},
                                            std::vector<Transform>{moved}));
    step(std::make_unique<VisibilityCommand>(FindByName(scene, "Lighting"), false));
    step(std::make_unique<RenameCommand>(cube, "Renamed cube"));
    step(std::make_unique<MoveCommand>(cube, FindByName(scene, "Props"), FindByName(scene, "Crates")));
    const NodeId group = static_cast<const CreateGroupCommand*>(step(std::make_unique<CreateGroupCommand>("Group 1", kInvalidNode)))->Node();
    scene.SetSelected(FindByName(scene, "Architecture"), true);
    scene.SetSelected(FindByName(scene, "Gameplay"), true);
    const std::vector<NodeId> idsBeforeDelete = NodeIds(scene);
    std::unique_ptr<DeleteCommand> deletion = MakeDeleteSelectionCommand(scene);
    CHECK(deletion != nullptr && std::string(deletion->Name()) == "Delete 2 items");
    step(std::move(deletion));
    CHECK(!scene.Contains(FindByName(scene, "Floor_01")) && history.UndoCount() == 7);
    CHECK(std::string(history.UndoName()) == "Delete 2 items");

    // Undo everything: every intermediate state comes back exactly.
    for (size_t i = states.size() - 1; i > 0; --i) {
        CHECK(history.Undo(scene));
        CHECK(scene.Validate() && SceneState(scene) == states[i - 1]);
        if (i == states.size() - 1) // the delete: restored with the same ids, in the same order
            CHECK(NodeIds(scene) == idsBeforeDelete && scene.SelectedItemCount() == 0);
        if (i == 2) // the move undone: back to its exact original transform
            CHECK(scene.TransformAt(scene.IndexOf(cube)) == original);
    }
    CHECK(!history.CanUndo() && !history.Undo(scene) && !scene.Contains(cube));

    // Redo everything: same states again, and the spawned nodes come back with their old ids.
    for (size_t i = 1; i < states.size(); ++i) {
        CHECK(history.Redo(scene));
        CHECK(scene.Validate() && SceneState(scene) == states[i]);
    }
    CHECK(!history.CanRedo() && scene.Contains(cube) && scene.Contains(group));
    CHECK(scene.NameAt(scene.IndexOf(cube)) == "Renamed cube" && scene.TransformAt(scene.IndexOf(cube)) == moved);

    // A new edit after undoing drops the redo steps.
    history.Undo(scene);
    history.Undo(scene);
    CHECK(history.CanRedo());
    step(std::make_unique<VisibilityCommand>(FindByName(scene, "Unsorted"), false));
    CHECK(!history.CanRedo() && history.Size() == 6);

    // Edits that change nothing are not recorded.
    const size_t size = history.Size();
    CHECK(history.Execute(std::make_unique<VisibilityCommand>(FindByName(scene, "Unsorted"), false), scene) == nullptr);
    CHECK(history.Execute(std::make_unique<RenameCommand>(cube, "Renamed cube"), scene) == nullptr);
    CHECK(history.Execute(std::make_unique<MoveCommand>(cube, kInvalidNode, cube), scene) == nullptr);
    CHECK(history.Size() == size);

    // A whole hierarchy drop as one step (MacroCommand).
    const std::string beforeMacro = SceneState(scene);
    std::vector<std::unique_ptr<Command>> moves;
    const NodeId unsorted = FindByName(scene, "Unsorted");
    const uint32_t unsortedSize = scene.NodeAt(scene.IndexOf(unsorted)).subtreeSize;
    moves.push_back(std::make_unique<MoveCommand>(FindByName(scene, "Barrel_01"), unsorted, kInvalidNode));
    moves.push_back(std::make_unique<MoveCommand>(FindByName(scene, "Barrel_02"), unsorted, kInvalidNode));
    CHECK(history.Execute(std::make_unique<MacroCommand>("Reparent", std::move(moves)), scene) != nullptr);
    CHECK(scene.NodeAt(scene.IndexOf(unsorted)).subtreeSize == unsortedSize + 2);
    history.Undo(scene);
    CHECK(SceneState(scene) == beforeMacro && scene.Validate());
}

void TestHistoryLimits()
{
    // Fixed number of steps: the oldest are forgotten.
    SceneTree scene;
    BuildMockLevel(scene);
    const NodeId lighting = FindByName(scene, "Lighting");
    CommandHistory history(100);
    std::string after150;
    for (int i = 0; i < 250; ++i) {
        CHECK(history.Execute(std::make_unique<VisibilityCommand>(lighting, (i % 2) != 0), scene) != nullptr);
        if (i == 149)
            after150 = SceneState(scene);
    }
    CHECK(history.Size() == 100 && history.UndoCount() == 100 && history.MaxSteps() == 100);
    int undone = 0;
    while (history.Undo(scene))
        ++undone;
    CHECK(undone == 100 && SceneState(scene) == after150);
    while (history.Redo(scene)) {
    }
    CHECK(history.UndoCount() == 100);

    // Memory budget: big deletes push old steps out, the newest step always stays.
    SceneTree big;
    BuildStressLevel(big, 20000); // 200 blocks of 100 items
    CommandHistory limited(100, 64 * 1024); // one 100-item block snapshot is ~10 KB
    int deletes = 0;
    for (uint32_t i = 1; i < big.Size() && deletes < 30;) {
        const NodeId block = big.NodeAt(i).id;
        if (!big.NodeAt(i).IsGroup()) {
            ++i;
            continue;
        }
        CHECK(limited.Execute(std::make_unique<DeleteCommand>(std::vector<NodeId>{block}), big) != nullptr);
        CHECK(limited.MemoryBytes() <= 64u * 1024u || limited.Size() == 1);
        ++deletes;
    }
    CHECK(limited.Size() < 30 && limited.Size() >= 2); // old steps were dropped to stay in budget
    while (limited.Undo(big)) {
    }
    CHECK(big.Validate());
}

void TestUndoMemoryStability()
{
    // Hundreds of random edits with undo / redo mixed in: memory stays flat and every step stays
    // reversible.
    SceneTree scene;
    BuildStressLevel(scene, 20000);
    CommandHistory history;
    uint32_t seed = 12345;
    const auto random = [&](uint32_t n) {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) % n;
    };
    const auto anyNode = [&](bool groupsOnly) {
        for (int tries = 0; tries < 64; ++tries) {
            const uint32_t i = random(scene.Size());
            if (!groupsOnly || scene.NodeAt(i).IsGroup())
                return scene.NodeAt(i).id;
        }
        return scene.NodeAt(0).id;
    };
    const auto privateMb = [] {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
        return double(counters.PrivateUsage) / (1024.0 * 1024.0);
    };

    double afterWarmup = 0.0;
    for (int action = 0; action < 1200; ++action) {
        if (action == 300)
            afterWarmup = privateMb();
        const uint32_t kind = random(10);
        if (kind == 0) {
            history.Undo(scene);
        } else if (kind == 1) {
            history.Redo(scene);
        } else if (kind == 2) {
            history.Execute(std::make_unique<SpawnCommand>(ItemCatalog()[random(5)], anyNode(true), "Spawned"), scene);
        } else if (kind == 3) {
            const NodeId node = anyNode(false);
            if (scene.IndexOf(node) != 0) // keep the root group
                history.Execute(std::make_unique<DeleteCommand>(std::vector<NodeId>{node}), scene);
        } else if (kind == 4) {
            history.Execute(std::make_unique<MoveCommand>(anyNode(false), anyNode(true), kInvalidNode), scene);
        } else if (kind == 5) {
            const NodeId node = anyNode(false);
            history.Execute(std::make_unique<VisibilityCommand>(node, !scene.NodeAt(scene.IndexOf(node)).Has(SceneTree::kVisible)), scene);
        } else {
            const NodeId node = anyNode(false);
            const uint32_t index = scene.IndexOf(node);
            if (scene.NodeAt(index).IsGroup())
                continue;
            Transform t = scene.TransformAt(index);
            Transform moved = t;
            moved.position.x += 1.0f;
            history.Execute(std::make_unique<TransformCommand>("Move", std::vector<NodeId>{node}, std::vector<Transform>{t},
                                                               std::vector<Transform>{moved}),
                            scene);
        }
        if (action % 100 == 0)
            CHECK(scene.Validate());
    }
    const double afterAll = privateMb();
    CHECK(history.Size() <= 100 && scene.Validate());

    // Full undo of the remembered steps and redo again reproduces the same scene.
    const std::string end = SceneState(scene);
    while (history.Undo(scene)) {
    }
    CHECK(scene.Validate());
    while (history.Redo(scene)) {
    }
    CHECK(SceneState(scene) == end && scene.Validate());
    CHECK(afterAll - afterWarmup < 8.0);
    std::printf("Undo stress: 1,200 random edits on 20,000 items, history %zu steps / %.2f MB, process %+.2f MB after warm-up\n",
                history.Size(), double(history.MemoryBytes()) / (1024.0 * 1024.0), afterAll - afterWarmup);
}

// ---- Mods

// Records what a mod draws and "clicks" buttons by label.
class FakeModUi final : public ModUi {
public:
    std::vector<std::string> texts;
    std::string press; // label of the button to report as clicked
    void Text(const char* text) override { texts.emplace_back(text); }
    bool Button(const char* label) override { return press == label; }
    bool Checkbox(const char*, bool&) override { return false; }
    bool Slider(const char*, float& value, float min, float max) override
    {
        value = std::clamp(value, min, max);
        return false;
    }
    void Separator() override {}
    void SameLine() override {}
};

struct ModFixture {
    SceneTree scene;
    CommandHistory history;
    AssetCatalog catalog;
    std::vector<std::string> notifications;
    ModContext Context()
    {
        ModContext context;
        context.scene = &scene;
        context.history = &history;
        context.catalog = &catalog;
        context.notify = [this](const std::string& message) { notifications.push_back(message); };
        context.version = "test";
        return context;
    }
};

bool LogHas(const ModInfo& info, std::string_view text)
{
    for (const std::string& line : info.log)
        if (line.find(text) != std::string::npos)
            return true;
    return false;
}

void TestModHelloWorld()
{
    ModFixture f;
    BuildMockLevel(f.scene);
    const uint32_t nodesBefore = f.scene.Size();
    ModRuntime mods(f.Context());
    const size_t mod = mods.LoadFile(std::filesystem::path(LM_SOURCE_DIR) / "examples" / "mods" / "hello_world.lua");
    const ModInfo& info = mods.Info(mod);
    CHECK(info.state == ModState::Running && info.error.empty());
    CHECK(info.name == "Hello World" && info.hasUi);
    CHECK(LogHas(info, "Hello, World! Running in LevelMinded test")); // on_load ran
    CHECK(f.notifications.size() == 1 && f.notifications[0] == "Hello World: Hello, World!");
    CHECK(mods.MenuItems().size() == 1 && mods.MenuItems()[0].label == "Spawn a ring of cubes");
    CHECK(f.scene.Size() == nodesBefore && !f.history.CanUndo()); // loading changed nothing

    // Its button spawns an entity through the API (an undoable step) and selects it.
    FakeModUi ui;
    mods.DrawUi(mod, ui);
    CHECK(f.scene.Size() == nodesBefore && !ui.texts.empty()); // drawn, nothing clicked
    ui.press = "Spawn Hello Cube";
    ui.texts.clear();
    mods.DrawUi(mod, ui);
    const NodeId cube = FindByName(f.scene, "Hello_Cube_1");
    CHECK(cube != kInvalidNode && f.scene.Size() == nodesBefore + 1);
    CHECK(Near3(f.scene.TransformAt(f.scene.IndexOf(cube)).position, 0.0f, -14.0f, 0.5f));
    CHECK(Selected(f.scene, cube) && f.scene.SelectedItemCount() == 1);
    CHECK(std::find(ui.texts.begin(), ui.texts.end(), "1 spawned") != ui.texts.end());
    CHECK(LogHas(mods.Info(mod), "Spawned Hello_Cube_1"));
    CHECK(f.history.UndoCount() == 1 && std::string(f.history.UndoName()) == "Place Item");
    f.history.Undo(f.scene);
    CHECK(!f.scene.Contains(cube) && f.scene.Size() == nodesBefore);

    // Its menu item builds a group of 8 cubes as one undo step.
    mods.RunMenuItem(0);
    const NodeId ring = FindByName(f.scene, "Hello Ring");
    CHECK(ring != kInvalidNode && f.scene.NodeAt(f.scene.IndexOf(ring)).subtreeSize == 9);
    CHECK(std::string(f.history.UndoName()) == "Hello World ring" && f.history.UndoCount() == 1);
    f.history.Undo(f.scene);
    CHECK(!f.scene.Contains(ring) && f.scene.Size() == nodesBefore && f.scene.Validate());
    f.history.Redo(f.scene);
    CHECK(f.scene.Contains(ring) && f.scene.Validate());
    CHECK(mods.Info(mod).state == ModState::Running);
}

void TestModSandbox()
{
    const std::filesystem::path dir = TestDir() / "mods";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "folder_mod");
    WriteText(dir / "a_sandbox.lua",
              "local blocked = { io = io, os = os, debug = debug, package = package, require = require, load = load,\n"
              "                  dofile = dofile, loadfile = loadfile, dump = string.dump }\n"
              "for name, value in pairs(blocked) do error(name .. ' should not exist') end\n"
              "print('sandbox ok', math.floor(2.5), string.format('%d', 7), #table.pack(1, 2, 3))\n");
    WriteText(dir / "b_memory.lua", "function on_load() local t = {} for i = 1, 100000000 do t[i] = {i} end end\n");
    WriteText(dir / "c_loop.lua", "function on_load() while true do end end\n");
    WriteText(dir / "d_badhandle.lua", "function on_load() lm.scene.set_position(424242, 0, 0, 0) end\n");
    WriteText(dir / "e_syntax.lua", "function on_load(\n");
    WriteText(dir / "f_bytecode.lua", std::string("\x1bLua\x55\x00 not really bytecode", 26));
    WriteText(dir / "g_ui_outside.lua", "function on_load() lm.ui.text('too early') end\n");
    WriteText(dir / "folder_mod" / "main.lua",
              "mod_info = { name = 'Folder Mod' }\n"
              "function on_load()\n"
              "  -- a failing undo group rolls back everything it did\n"
              "  local ok, err = pcall(lm.undo_group, 'broken', function()\n"
              "    for i = 1, 3 do lm.scene.spawn('Cube', { name = 'Rolled_back_' .. i }) end\n"
              "    error('boom')\n"
              "  end)\n"
              "  assert(not ok and string.find(err, 'boom'))\n"
              "  assert(lm.scene.find('Rolled_back_1') == nil)\n"
              "  lm.undo_group('two cubes', function()\n"
              "    lm.scene.spawn('Cube', { name = 'Kept_1', position = {1, 2, 3}, scale = 2 })\n"
              "    lm.scene.spawn('Sphere', { name = 'Kept_2', rotation = {0, 0, 90} })\n"
              "  end)\n"
              "  local g = lm.scene.create_group('Modded')\n"
              "  lm.scene.move(lm.scene.find('Kept_1'), g)\n"
              "  lm.scene.set_visible(g, false)\n"
              "  lm.scene.rename(g, 'Modded group')\n"
              "  assert(lm.scene.kind(g) == 'group' and lm.scene.parent(lm.scene.find('Kept_1')) == g)\n"
              "  local x, y, z = lm.scene.position(lm.scene.find('Kept_1'))\n"
              "  assert(x == 1 and y == 2 and z == 3)\n"
              "  assert(#lm.assets.list() >= 9 and #lm.scene.children(g) == 1)\n"
              "  assert(not pcall(lm.scene.spawn, 'No such item'))\n"
              "  assert(not pcall(lm.scene.move, g, lm.scene.find('Kept_2')))  -- items can't hold children\n"
              "end\n"
              "function on_unload() lm.notify('bye') end\n");

    ModFixture f;
    BuildMockLevel(f.scene);
    const std::string before = SceneState(f.scene);
    ModLimits limits;
    limits.memoryBytes = 8u << 20;
    limits.instructionsPerCall = 5'000'000;
    ModRuntime mods(f.Context(), limits);
    CHECK(mods.LoadFolder(dir) == 8 && mods.Count() == 8);
    CHECK(mods.LoadFolder(dir / "missing") == 0);

    const auto info = [&](std::string_view name) -> const ModInfo& {
        for (size_t i = 0; i < mods.Count(); ++i)
            if (mods.Info(i).name == name)
                return mods.Info(i);
        static ModInfo none;
        CHECK(!"mod not found");
        return none;
    };
    CHECK(info("a_sandbox").state == ModState::Running && LogHas(info("a_sandbox"), "sandbox ok\t2\t7\t3"));
    CHECK(info("b_memory").state == ModState::Failed && info("b_memory").error.find("memory") != std::string::npos);
    CHECK(info("b_memory").memoryBytes == 0 && info("b_memory").peakMemoryBytes <= limits.memoryBytes);
    CHECK(info("c_loop").state == ModState::Failed && info("c_loop").error.find("too long") != std::string::npos);
    CHECK(info("d_badhandle").state == ModState::Failed && info("d_badhandle").error.find("not a node") != std::string::npos);
    CHECK(info("e_syntax").state == ModState::Failed && info("e_syntax").error.find("e_syntax.lua:") != std::string::npos);
    CHECK(info("f_bytecode").state == ModState::Failed && info("f_bytecode").error.find("precompiled") != std::string::npos);
    CHECK(info("g_ui_outside").state == ModState::Failed && info("g_ui_outside").error.find("on_ui") != std::string::npos);
    const ModInfo& folderMod = info("Folder Mod");
    CHECK(folderMod.state == ModState::Running);
    if (folderMod.state != ModState::Running)
        std::fprintf(stderr, "Folder Mod: %s\n", folderMod.error.c_str());

    // The failing mods changed nothing; the folder mod's edits are three undo steps (+ group steps).
    CHECK(FindByName(f.scene, "Kept_1") != kInvalidNode && FindByName(f.scene, "Rolled_back_1") == kInvalidNode);
    CHECK(!Visible(f.scene, FindByName(f.scene, "Kept_1")) && FindByName(f.scene, "Modded group") != kInvalidNode);
    CHECK(f.scene.Validate());
    while (f.history.Undo(f.scene)) {
    }
    CHECK(SceneState(f.scene) == before); // every mod edit was undoable

    // Unloading calls on_unload of running mods only.
    mods.UnloadAll();
    CHECK(mods.Count() == 0 && f.notifications.size() == 1 && f.notifications[0] == "Folder Mod: bye");
}

template <typename F>
double TimeMicroseconds(F&& work, int repeats = 20)
{
    double best = 1e30;
    for (int r = 0; r < repeats; ++r) {
        const auto start = std::chrono::steady_clock::now();
        work();
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        best = us < best ? us : best;
    }
    return best;
}

void Benchmark()
{
    // 100 areas x 10 rooms x 100 items = 100,000 items (+1,101 groups).
    SceneTree tree;
    const auto buildStart = std::chrono::steady_clock::now();
    const NodeId world = tree.CreateGroup("World");
    NodeId lastArea = kInvalidNode;
    for (int area = 0; area < 100; ++area) {
        lastArea = tree.CreateGroup("Area", world);
        for (int room = 0; room < 10; ++room) {
            const NodeId roomId = tree.CreateGroup("Room", lastArea);
            for (int item = 0; item < 100; ++item)
                tree.CreateItem("Item", NodeKind::Brush, {}, roomId);
        }
    }
    const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count();
    CHECK(tree.Size() == 101101);
    CHECK(tree.Validate());

    // Every timed call flips the state, so none of them hits the "already in that state" early-out.
    bool visible = true;
    const double hideUs = TimeMicroseconds([&] { visible = !visible; tree.SetVisible(world, visible); });
    CHECK(Visible(tree, lastArea) == visible);
    bool selected = false;
    const double selectUs = TimeMicroseconds([&] { selected = !selected; tree.SetSelected(world, selected); });
    CHECK(tree.SelectedItemCount() == (selected ? 100000u : 0u));
    const NodeId firstArea = tree.NodeAt(1).id;
    const double moveUs = TimeMicroseconds([&] { tree.Move(tree.NodeAt(1).id, world); }, 10); // 1,011-node block to the end
    CHECK(tree.Validate());
    CHECK(tree.Contains(firstArea) && tree.Contains(lastArea));

    std::printf("Benchmark (%u nodes, %zu bytes hot data):\n", tree.Size(), tree.Size() * sizeof(SceneTree::Node));
    std::printf("  build (depth-first inserts)       %8.2f ms\n", buildMs);
    std::printf("  hide/show whole tree (propagate)  %8.1f us\n", hideUs);
    std::printf("  select/deselect whole tree        %8.1f us\n", selectUs);
    std::printf("  move 1,011-node group to the end  %8.1f us\n", moveUs);

    // Click picking over a 100,000-item level (every item's box is tested).
    SceneTree level;
    BuildStressLevel(level, 100000);
    const MeshLibrary meshes;
    const float firstRowY = -317.0f * 2.5f * 0.5f; // BuildStressLevel: 317 x 317 grid, first row
    const Ray ray{{0.0f, firstRowY, 0.5f}, {1.0f, 0.0f, 0.0f}};
    PickHit hit;
    const double pickUs = TimeMicroseconds([&] { hit = PickItem(level, meshes, ray); }, 10);
    CHECK(hit && Near(hit.distance, 29.5f, 1e-3f) && hit.itemsTested == 100000u);
    std::printf("  pick (ray vs 100,000 items)       %8.1f us\n", pickUs);

    // Map files: save and load the 100,000-item level; loading again and again must not grow memory.
    {
        const std::filesystem::path path = TestDir() / "maps" / "stress.lmmap";
        std::filesystem::create_directories(path.parent_path());
        const AssetCatalog catalog;
        std::string error;
        MapSaveInfo info;
        bool saved = false;
        const double saveUs = TimeMicroseconds([&] { saved = SaveMap(path, level, catalog, MapCamera{}, error, &info); }, 3);
        CHECK(saved);

        SceneTree loaded;
        AssetCatalog loadedCatalog;
        MeshLibrary loadedMeshes;
        MapLoadReport report;
        bool ok = false;
        const double loadUs = TimeMicroseconds([&] { ok = LoadMap(path, loaded, loadedCatalog, loadedMeshes, report, error); }, 3);
        CHECK(ok && SameScene(level, loaded, SameBuiltinMesh));

        const auto privateBytes = [] {
            PROCESS_MEMORY_COUNTERS_EX counters{};
            ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
            return double(counters.PrivateUsage) / (1024.0 * 1024.0);
        };
        const double before = privateBytes();
        for (int i = 0; i < 10; ++i)
            ok &= LoadMap(path, loaded, loadedCatalog, loadedMeshes, report, error);
        const double after = privateBytes();
        CHECK(ok && after - before < 4.0); // ten more loads of a 100k-item map: no growth beyond noise

        std::printf("  save map (100,000 items, %.1f MB) %8.1f ms\n", double(info.bytes) / (1024.0 * 1024.0), saveUs / 1000.0);
        std::printf("  load map (100,000 items)          %8.1f ms\n", loadUs / 1000.0);
        std::printf("  memory after 10 more loads        %+8.2f MB\n", after - before);
    }
}

} // namespace

int main()
{
    TestBuild();
    TestInsertInMiddle();
    TestMove();
    TestVisibility();
    TestSelection();
    TestRemove();
    TestManyMoves();
    TestNamesAndVersions();
    TestCatalogSpawn();
    TestMeshWinding();
    TestTransforms();
    TestPicking();
    TestImportObj();
    TestImportGltf();
    TestAssetCatalog();
    TestMapRoundTrip();
    TestMapAssets();
    TestSnapshotRestore();
    TestUndoRedo();
    TestHistoryLimits();
    TestUndoMemoryStability();
    TestModHelloWorld();
    TestModSandbox();
    Benchmark();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
