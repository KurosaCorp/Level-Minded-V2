#pragma once

#include <cstdint>

namespace lm {

// Which mesh an item shows. Shared by the scene model and the renderer. The built-in meshes come
// first; imported models get the following ids at run time (see MeshLibrary).
// Every mesh is modeled in a unit box [-1, 1]^3 and scaled by the item's half extents.
enum class MeshId : uint16_t {
    None,     // no solid mesh: drawn as a wire box (volumes, triggers)
    Cube,
    Sphere,
    Cylinder, // along +Z
    Spawn,    // capsule-like marker with an arrow pointing along +X
    BuiltinCount,

    Default = 0xFFFF, // SceneTree::CreateItem: pick the mesh from the item kind
};

inline constexpr uint32_t kBuiltinMeshCount = static_cast<uint32_t>(MeshId::BuiltinCount);

constexpr bool IsImportedMesh(MeshId mesh)
{
    return static_cast<uint32_t>(mesh) >= kBuiltinMeshCount && mesh != MeshId::Default;
}

} // namespace lm
