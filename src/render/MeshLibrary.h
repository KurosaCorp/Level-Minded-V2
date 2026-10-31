#pragma once

#include "render/MeshGeometry.h"

#include <cstdint>
#include <span>
#include <vector>

namespace lm {

// Per-mesh render traits.
enum MeshTrait : uint8_t {
    kMeshTraitDoubleSided = 1 << 0, // draw without back-face culling (glTF doubleSided materials)
};

// Every mesh the editor can draw: the built-in shapes followed by imported models, all in one
// contiguous vertex array and one 32-bit index array. The renderer keeps a single vertex/index buffer
// pair for all of them. Meshes are only ever appended, so a consumer that has already uploaded the
// first N vertices and indices only needs to upload the rest.
class MeshLibrary {
public:
    // The renderer's instance handles reserve 12 bits for the mesh.
    static constexpr uint32_t kMaxMeshes = 4096;

    struct Mesh {
        MeshRange range;      // triangle list in Vertices() / Indices()
        uint8_t traits = 0;   // MeshTrait bits
    };

    MeshLibrary(); // the built-in meshes and the wire box

    // Appends a triangle-list mesh modeled in [-1, 1]^3 (indices relative to its first vertex).
    // Returns its id, or MeshId::None if the data is invalid or the library is full.
    MeshId Add(std::span<const MeshVertex> vertices, std::span<const uint32_t> indices, uint8_t traits = 0);

    // Number of mesh ids in use, including MeshId::None (valid ids are below this).
    uint32_t Count() const { return static_cast<uint32_t>(m_meshes.size()); }
    // MeshId::None and unknown ids give an empty mesh.
    const Mesh& Get(MeshId mesh) const
    {
        const auto index = static_cast<uint32_t>(mesh);
        return index < m_meshes.size() ? m_meshes[index] : m_meshes[0];
    }
    const MeshRange& WireBox() const { return m_wireBox; } // unit box edges as a line list

    std::span<const MeshVertex> Vertices() const { return m_vertices; }
    std::span<const uint32_t> Indices() const { return m_indices; }
    // Bumped by every Add.
    uint64_t Version() const { return m_version; }

private:
    std::vector<MeshVertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<Mesh> m_meshes; // indexed by MeshId
    MeshRange m_wireBox;
    uint64_t m_version = 1;
};

} // namespace lm
