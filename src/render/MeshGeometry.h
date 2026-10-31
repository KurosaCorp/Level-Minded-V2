#pragma once

#include "core/MeshId.h"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

namespace lm {

// 28 bytes. `color` (RGBA8, R in the low byte) is multiplied with the instance color: white for the
// built-in meshes, the material color for imported models.
struct MeshVertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    uint32_t color;
};
static_assert(sizeof(MeshVertex) == 28);

// Where one mesh lives inside the shared vertex/index arrays.
struct MeshRange {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int32_t baseVertex = 0;
    uint32_t vertexCount = 0;
};

// All built-in meshes packed into one vertex array and one 16-bit index array, so the renderer
// binds a single vertex/index buffer pair for every mesh draw.
//
// Meshes are modeled in the unit box [-1, 1]^3 (right-handed, Z-up) with flat or smooth normals.
// Triangles are counter-clockwise when seen from outside (the side the normals point to).
struct MeshGeometry {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;       // relative to MeshRange::baseVertex
    MeshRange ranges[kBuiltinMeshCount]; // MeshId::None stays empty (triangle lists)
    MeshRange wireBox;              // unit box edges as a LINE list, for instanced outlines

    const MeshRange& Range(MeshId mesh) const { return ranges[static_cast<uint32_t>(mesh)]; }
};

MeshGeometry BuildBuiltinMeshes();

} // namespace lm
