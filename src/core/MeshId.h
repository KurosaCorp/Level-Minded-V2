#pragma once

#include <cstdint>

namespace lm {

// Built-in meshes. Shared by the scene model (which mesh an item shows) and the renderer.
// Every mesh is modeled in a unit box [-1, 1]^3 and scaled by the item's half extents.
enum class MeshId : uint8_t {
    None,     // no solid mesh: drawn as a wire box (volumes, triggers)
    Cube,
    Sphere,
    Cylinder, // along +Z
    Spawn,    // capsule-like marker with an arrow pointing along +X
    Count,

    Default = 0xFF, // SceneTree::CreateItem: pick the mesh from the item kind
};

inline constexpr uint32_t kMeshCount = static_cast<uint32_t>(MeshId::Count);

} // namespace lm
