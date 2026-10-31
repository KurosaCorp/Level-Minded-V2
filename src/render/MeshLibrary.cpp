#include "render/MeshLibrary.h"

#include <algorithm>
#include <limits>

namespace lm {

MeshLibrary::MeshLibrary()
{
    MeshGeometry geometry = BuildBuiltinMeshes();
    m_vertices = std::move(geometry.vertices);
    m_indices = std::move(geometry.indices);
    m_wireBox = geometry.wireBox;
    m_meshes.resize(kBuiltinMeshCount);
    for (uint32_t mesh = 0; mesh < kBuiltinMeshCount; ++mesh)
        m_meshes[mesh].range = geometry.ranges[mesh];
}

MeshId MeshLibrary::Add(std::span<const MeshVertex> vertices, std::span<const uint32_t> indices, uint8_t traits)
{
    if (m_meshes.size() >= kMaxMeshes || vertices.empty() || indices.empty() || indices.size() % 3 != 0)
        return MeshId::None;
    if (m_vertices.size() + vertices.size() > size_t(std::numeric_limits<int32_t>::max()) ||
        m_indices.size() + indices.size() > size_t(std::numeric_limits<uint32_t>::max()))
        return MeshId::None;
    const uint32_t maxIndex = *std::max_element(indices.begin(), indices.end());
    if (maxIndex >= vertices.size())
        return MeshId::None;

    Mesh mesh;
    mesh.range.firstIndex = static_cast<uint32_t>(m_indices.size());
    mesh.range.indexCount = static_cast<uint32_t>(indices.size());
    mesh.range.baseVertex = static_cast<int32_t>(m_vertices.size());
    mesh.range.vertexCount = static_cast<uint32_t>(vertices.size());
    mesh.traits = traits;
    m_vertices.insert(m_vertices.end(), vertices.begin(), vertices.end());
    m_indices.insert(m_indices.end(), indices.begin(), indices.end());
    m_meshes.push_back(mesh);
    ++m_version;
    return static_cast<MeshId>(m_meshes.size() - 1);
}

} // namespace lm
