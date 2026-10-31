#include "scene/ItemCatalog.h"

#include <cwctype>

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

AssetCatalog::AssetCatalog()
{
    const std::span<const CatalogItem> builtins = ItemCatalog();
    m_items.assign(builtins.begin(), builtins.end());
    m_sources.resize(m_items.size());
    m_sourceKeys.resize(m_items.size());
}

std::wstring AssetCatalog::SourceKey(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::weakly_canonical(path, error);
    if (error)
        absolute = std::filesystem::absolute(path, error);
    std::wstring key = (error ? path : absolute).lexically_normal().wstring();
    for (wchar_t& c : key)
        c = static_cast<wchar_t>(std::towlower(c)); // Windows paths are case-insensitive
    return key;
}

size_t AssetCatalog::AddImported(std::string_view name, std::string_view description, const char* format, MeshId mesh,
                                 const DirectX::XMFLOAT3& halfExtents, const std::filesystem::path& source)
{
    // Unique display name: it also prefixes the names of spawned nodes.
    std::string unique(name.empty() ? std::string_view("Model") : name);
    const auto taken = [&](const std::string& candidate) {
        for (const CatalogItem& item : m_items)
            if (candidate == item.name)
                return true;
        return false;
    };
    for (int n = 2; taken(unique); ++n)
        unique = std::string(name.empty() ? std::string_view("Model") : name) + " (" + std::to_string(n) + ")";

    const char* storedName = m_text.emplace_back(std::move(unique)).c_str();
    const char* storedDescription = m_text.emplace_back(description).c_str();
    m_items.push_back({storedName, storedDescription, NodeKind::Mesh, mesh, halfExtents, format});
    m_sources.push_back(source);
    m_sourceKeys.push_back(SourceKey(source));
    ++m_importVersion;
    return m_items.size() - 1;
}

size_t AssetCatalog::FindMesh(MeshId mesh) const
{
    for (size_t i = 0; i < m_items.size(); ++i)
        if (IsImported(i) && m_items[i].mesh == mesh)
            return i;
    return npos;
}

void AssetCatalog::ResetImported()
{
    const size_t builtins = ItemCatalog().size();
    m_items.resize(builtins);
    m_sources.resize(builtins);
    m_sourceKeys.resize(builtins);
    m_items.shrink_to_fit();
    m_sources.shrink_to_fit();
    m_sourceKeys.shrink_to_fit();
    std::deque<std::string>().swap(m_text); // only imported items keep text here
    ++m_importVersion;
}

size_t AssetCatalog::FindSource(const std::filesystem::path& source) const
{
    const std::wstring key = SourceKey(source);
    for (size_t i = 0; i < m_sourceKeys.size(); ++i)
        if (!m_sourceKeys[i].empty() && m_sourceKeys[i] == key)
            return i;
    return npos;
}

NodeId SpawnCatalogItem(SceneTree& tree, const CatalogItem& item, NodeId parent, std::string_view name)
{
    // The pivot is the box center; it is lifted so the item rests on the ground plane at the origin.
    const Bounds bounds{{0.0f, 0.0f, item.halfExtents.z}, item.halfExtents};
    return tree.CreateItem(name, item.kind, bounds, parent, item.mesh);
}

} // namespace lm
