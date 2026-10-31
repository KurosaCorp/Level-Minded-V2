// Unit tests and a small benchmark for lm::SceneTree. Run: build\<preset>\bin\LevelMindedTests.exe
// (or `ctest` in the build directory). Exit code is non-zero on failure.

#include "render/MeshGeometry.h"
#include "scene/ItemCatalog.h"
#include "scene/MockLevel.h"
#include "scene/SceneTree.h"

#include <chrono>
#include <cmath>
#include <set>
#include <cstdio>
#include <string>
#include <vector>

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
    const Bounds& b = f.tree.BoundsAt(index);
    CHECK(b.center.x == 0.0f && b.center.y == 0.0f && b.center.z == spawnPoint.halfExtents.z);
    CHECK(b.halfExtents.x == spawnPoint.halfExtents.x && b.halfExtents.z == spawnPoint.halfExtents.z);

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
    for (uint32_t m = 1; m < kMeshCount; ++m) {
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
        const uint16_t i0 = geometry.indices[wire.firstIndex + e];
        const uint16_t i1 = geometry.indices[wire.firstIndex + e + 1];
        CHECK(i0 < 8 && i1 < 8);
        const auto& p0 = geometry.vertices[static_cast<size_t>(wire.baseVertex) + i0].position;
        const auto& p1 = geometry.vertices[static_cast<size_t>(wire.baseVertex) + i1].position;
        const float d = std::fabs(p0.x - p1.x) + std::fabs(p0.y - p1.y) + std::fabs(p0.z - p1.z);
        CHECK(d == 2.0f);
    }
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
    Benchmark();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
