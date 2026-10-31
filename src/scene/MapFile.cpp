#include "scene/MapFile.h"

#include "import/ModelImporter.h"

#include <simdjson.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

using namespace DirectX;
namespace ondemand = simdjson::ondemand;

namespace lm {

namespace {

constexpr const char* kFormatName = "LevelMinded map";

const char* KindToken(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Group:  return "group";
    case NodeKind::Brush:  return "brush";
    case NodeKind::Mesh:   return "mesh";
    case NodeKind::Light:  return "light";
    case NodeKind::Entity: return "entity";
    }
    return "mesh";
}

bool KindFromToken(std::string_view token, NodeKind& kind)
{
    static constexpr std::pair<std::string_view, NodeKind> kKinds[] = {
        {"group", NodeKind::Group}, {"brush", NodeKind::Brush}, {"mesh", NodeKind::Mesh},
        {"light", NodeKind::Light}, {"entity", NodeKind::Entity}};
    for (const auto& [name, value] : kKinds) {
        if (token == name) {
            kind = value;
            return true;
        }
    }
    return false;
}

constexpr std::string_view kBuiltinMeshTokens[kBuiltinMeshCount] = {"none", "cube", "sphere", "cylinder", "spawn"};

bool MeshFromToken(std::string_view token, MeshId& mesh)
{
    for (uint32_t i = 0; i < kBuiltinMeshCount; ++i) {
        if (token == kBuiltinMeshTokens[i]) {
            mesh = static_cast<MeshId>(i);
            return true;
        }
    }
    return false;
}

std::string Utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string GenericUtf8(const std::filesystem::path& path) // forward slashes
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path PathFromUtf8(std::string_view text)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

// ---- Writing

class JsonWriter {
public:
    explicit JsonWriter(size_t reserve) { m_out.reserve(reserve); }
    std::string Take() { return std::move(m_out); }

    void Raw(std::string_view text) { m_out.append(text); }
    void Indent(int depth) { m_out.append(size_t(depth) * 2, ' '); }

    void String(std::string_view text)
    {
        m_out += '"';
        for (const char c : text) {
            switch (c) {
            case '"':  m_out += "\\\""; break;
            case '\\': m_out += "\\\\"; break;
            case '\n': m_out += "\\n"; break;
            case '\r': m_out += "\\r"; break;
            case '\t': m_out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) { // other control characters
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", unsigned(static_cast<unsigned char>(c)));
                    m_out += escaped;
                } else {
                    m_out += c; // UTF-8 passes through unchanged
                }
            }
        }
        m_out += '"';
    }

    // Shortest decimal form that reads back as exactly the same float.
    void Float(float value)
    {
        if (!std::isfinite(value))
            value = 0.0f; // JSON has no inf/nan; the editor never produces them
        char buffer[32];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        m_out.append(buffer, result.ptr);
    }

    void Floats(const float* values, int count)
    {
        m_out += '[';
        for (int i = 0; i < count; ++i) {
            if (i)
                m_out += ", ";
            Float(values[i]);
        }
        m_out += ']';
    }

    void Key(std::string_view key)
    {
        m_out += '"';
        m_out.append(key);
        m_out += "\": ";
    }

private:
    std::string m_out;
};

struct WriteContext {
    const SceneTree& scene;
    const std::vector<int32_t>& assetOfMesh; // MeshId -> index in "assets", -1 for built-ins
    JsonWriter& out;
};

void WriteNodes(const WriteContext& ctx, uint32_t first, uint32_t end, int depth)
{
    const SceneTree& scene = ctx.scene;
    JsonWriter& out = ctx.out;
    for (uint32_t i = first; i < end; i = scene.SubtreeEnd(i)) {
        const SceneTree::Node& node = scene.NodeAt(i);
        out.Indent(depth);
        out.Raw("{\"type\": \"");
        out.Raw(KindToken(node.kind));
        out.Raw("\", \"name\": ");
        out.String(scene.NameAt(i));
        if (!node.Has(SceneTree::kVisible))
            out.Raw(", \"visible\": false");

        if (node.IsGroup()) {
            if (!node.Has(SceneTree::kExpanded))
                out.Raw(", \"expanded\": false");
            if (node.subtreeSize == 1) {
                out.Raw(", \"children\": []}");
            } else {
                out.Raw(", \"children\": [\n");
                WriteNodes(ctx, i + 1, scene.SubtreeEnd(i), depth + 1);
                out.Indent(depth);
                out.Raw("]}");
            }
        } else {
            const ItemData& item = scene.ItemAt(i);
            const Transform& t = item.transform;
            const auto meshIndex = static_cast<uint32_t>(item.mesh);
            const int32_t asset = meshIndex < ctx.assetOfMesh.size() ? ctx.assetOfMesh[meshIndex] : -1;
            if (asset >= 0) {
                out.Raw(", \"asset\": ");
                out.Raw(std::to_string(asset));
            } else {
                out.Raw(", \"mesh\": \"");
                out.Raw(meshIndex < kBuiltinMeshCount ? kBuiltinMeshTokens[meshIndex] : std::string_view("none"));
                out.Raw("\"");
            }
            out.Raw(", \"position\": ");
            out.Floats(&t.position.x, 3);
            if (!(t.rotation.x == 0.0f && t.rotation.y == 0.0f && t.rotation.z == 0.0f && t.rotation.w == 1.0f)) {
                out.Raw(", \"rotation\": ");
                out.Floats(&t.rotation.x, 4);
            }
            if (!(t.scale.x == 1.0f && t.scale.y == 1.0f && t.scale.z == 1.0f)) {
                out.Raw(", \"scale\": ");
                out.Floats(&t.scale.x, 3);
            }
            out.Raw(", \"halfExtents\": ");
            out.Floats(&item.halfExtents.x, 3);
            out.Raw("}");
        }
        out.Raw(scene.SubtreeEnd(i) < end ? ",\n" : "\n");
    }
}

// ---- Reading

struct ParsedAsset {
    std::string_view name, format, path, absolutePath; // views into the parser's string buffer
    XMFLOAT3 halfExtents{1.0f, 1.0f, 1.0f};
};

struct ParsedNode {
    uint32_t parent = UINT32_MAX; // index in ParsedMap::nodes, UINT32_MAX = top level
    std::string_view name;
    NodeKind kind = NodeKind::Group;
    bool hasType = false;
    bool visible = true;
    bool expanded = true;
    bool hasChildren = false;
    MeshId mesh = MeshId::Default;
    int64_t asset = -1;
    Transform transform;
    XMFLOAT3 halfExtents{0.5f, 0.5f, 0.5f};
};

struct ParsedMap {
    std::vector<ParsedAsset> assets;
    std::vector<ParsedNode> nodes;
    size_t nameChars = 0;
    bool hasCamera = false;
    MapCamera camera;
};

class MapParser {
public:
    explicit MapParser(ParsedMap& map) : m_map(map) {}
    const std::string& Error() const { return m_error; }

    bool Parse(ondemand::document& doc)
    {
        ondemand::object root;
        if (Check(doc.get_object().get(root), "the file is not a JSON object"))
            return false;
        bool sawFormat = false;
        int64_t version = -1;
        for (auto fieldResult : root) {
            ondemand::field field;
            std::string_view key;
            if (Check(std::move(fieldResult).get(field), "broken JSON") || Check(field.unescaped_key().get(key), "broken JSON"))
                return false;
            ondemand::value value = field.value();
            if (key == "format") {
                std::string_view format;
                if (Check(value.get_string().get(format), "\"format\" must be a string"))
                    return false;
                if (format != kFormatName)
                    return Fail("it is not a LevelMinded map");
                sawFormat = true;
            } else if (key == "version") {
                if (Check(value.get_int64().get(version), "\"version\" must be a whole number"))
                    return false;
                if (version > kMapFormatVersion)
                    return Fail("it was saved by a newer LevelMinded (map version " + std::to_string(version) + ")");
            } else if (key == "camera") {
                if (!ParseCamera(value))
                    return false;
            } else if (key == "assets") {
                if (!ParseAssets(value))
                    return false;
            } else if (key == "nodes") {
                if (!ParseChildren(value, UINT32_MAX, 0))
                    return false;
            } // unknown keys are ignored (forward compatibility)
        }
        if (!sawFormat)
            return Fail("it is not a LevelMinded map (no \"format\" field)");
        if (version < 1)
            return Fail("missing or invalid \"version\"");
        // Asset references must point at listed assets.
        for (const ParsedNode& node : m_map.nodes)
            if (node.asset >= int64_t(m_map.assets.size()))
                return Fail("item \"" + std::string(node.name) + "\" refers to asset " + std::to_string(node.asset) +
                            ", but the map lists only " + std::to_string(m_map.assets.size()));
        return true;
    }

private:
    bool Fail(std::string message)
    {
        if (m_error.empty())
            m_error = std::move(message);
        return false;
    }
    // True (and records the error) if `error` is set.
    bool Check(simdjson::error_code error, const char* what)
    {
        if (!error)
            return false;
        Fail(std::string(what) + " (" + simdjson::error_message(error) + ")");
        return true;
    }

    // Floats are read from their text with from_chars(float), so a saved value comes back bit for bit
    // (parsing as double and narrowing could round differently in rare cases).
    bool ParseFloat(ondemand::value& value, float& out, const char* what)
    {
        std::string_view token = value.raw_json_token();
        while (!token.empty() && (token.back() == ' ' || token.back() == '\n' || token.back() == '\r' || token.back() == '\t'))
            token.remove_suffix(1);
        const auto result = std::from_chars(token.data(), token.data() + token.size(), out);
        if (result.ec != std::errc() || result.ptr != token.data() + token.size() || !std::isfinite(out))
            return Fail(std::string(what) + " must be a number");
        return true;
    }

    bool ParseFloats(ondemand::value& value, float* out, int count, const char* what)
    {
        ondemand::array array;
        if (Check(value.get_array().get(array), what))
            return false;
        int n = 0;
        for (auto elementResult : array) {
            ondemand::value element;
            if (Check(elementResult.get(element), what))
                return false;
            if (n >= count)
                return Fail(std::string(what) + " has too many numbers");
            if (!ParseFloat(element, out[n], what))
                return false;
            ++n;
        }
        if (n != count)
            return Fail(std::string(what) + " needs " + std::to_string(count) + " numbers");
        return true;
    }

    bool ParseCamera(ondemand::value& value)
    {
        ondemand::object object;
        if (Check(value.get_object().get(object), "\"camera\" must be an object"))
            return false;
        MapCamera& camera = m_map.camera;
        for (auto fieldResult : object) {
            ondemand::field field;
            std::string_view key;
            if (Check(std::move(fieldResult).get(field), "broken camera") || Check(field.unescaped_key().get(key), "broken camera"))
                return false;
            ondemand::value v = field.value();
            bool ok = true;
            if (key == "target")
                ok = ParseFloats(v, &camera.target.x, 3, "camera target");
            else if (key == "yaw")
                ok = ParseFloat(v, camera.yaw, "camera yaw");
            else if (key == "pitch")
                ok = ParseFloat(v, camera.pitch, "camera pitch");
            else if (key == "distance")
                ok = ParseFloat(v, camera.distance, "camera distance");
            if (!ok)
                return false;
        }
        m_map.hasCamera = true;
        return true;
    }

    bool ParseAssets(ondemand::value& value)
    {
        ondemand::array array;
        if (Check(value.get_array().get(array), "\"assets\" must be an array"))
            return false;
        for (auto elementResult : array) {
            ondemand::object object;
            if (Check(elementResult.get_object().get(object), "each asset must be an object"))
                return false;
            ParsedAsset& asset = m_map.assets.emplace_back();
            for (auto fieldResult : object) {
                ondemand::field field;
                std::string_view key;
                if (Check(std::move(fieldResult).get(field), "broken asset") || Check(field.unescaped_key().get(key), "broken asset"))
                    return false;
                ondemand::value v = field.value();
                bool bad = false;
                if (key == "name")
                    bad = Check(v.get_string().get(asset.name), "asset name must be a string");
                else if (key == "format")
                    bad = Check(v.get_string().get(asset.format), "asset format must be a string");
                else if (key == "path")
                    bad = Check(v.get_string().get(asset.path), "asset path must be a string");
                else if (key == "absolutePath")
                    bad = Check(v.get_string().get(asset.absolutePath), "asset path must be a string");
                else if (key == "halfExtents")
                    bad = !ParseFloats(v, &asset.halfExtents.x, 3, "asset halfExtents");
                if (bad)
                    return false;
            }
            if (asset.path.empty() && asset.absolutePath.empty())
                return Fail("an asset has no file path");
        }
        return true;
    }

    bool ParseChildren(ondemand::value& value, uint32_t parent, int depth)
    {
        ondemand::array array;
        if (Check(value.get_array().get(array), "\"nodes\" / \"children\" must be an array"))
            return false;
        for (auto elementResult : array) {
            ondemand::value element;
            if (Check(elementResult.get(element), "broken node list") || !ParseNode(element, parent, depth))
                return false;
        }
        return true;
    }

    bool ParseNode(ondemand::value& value, uint32_t parent, int depth)
    {
        if (depth > 1000)
            return Fail("groups are nested too deeply");
        ondemand::object object;
        if (Check(value.get_object().get(object), "each node must be an object"))
            return false;
        // Pre-order: the node is added before its children, whatever the order of its fields.
        const auto index = static_cast<uint32_t>(m_map.nodes.size());
        m_map.nodes.emplace_back().parent = parent;

        for (auto fieldResult : object) {
            ondemand::field field;
            std::string_view key;
            if (Check(std::move(fieldResult).get(field), "broken node") || Check(field.unescaped_key().get(key), "broken node"))
                return false;
            ondemand::value v = field.value();
            ParsedNode& node = m_map.nodes[index]; // re-fetched: children below may grow the vector
            bool ok = true;
            if (key == "type") {
                std::string_view type;
                ok = !Check(v.get_string().get(type), "node type must be a string");
                if (ok && !KindFromToken(type, node.kind))
                    return Fail("unknown node type \"" + std::string(type) + "\"");
                node.hasType = true;
            } else if (key == "name") {
                ok = !Check(v.get_string().get(node.name), "node name must be a string");
            } else if (key == "visible") {
                ok = !Check(v.get_bool().get(node.visible), "\"visible\" must be true or false");
            } else if (key == "expanded") {
                ok = !Check(v.get_bool().get(node.expanded), "\"expanded\" must be true or false");
            } else if (key == "mesh") {
                std::string_view mesh;
                ok = !Check(v.get_string().get(mesh), "\"mesh\" must be a string");
                if (ok && !MeshFromToken(mesh, node.mesh))
                    return Fail("unknown mesh \"" + std::string(mesh) + "\"");
            } else if (key == "asset") {
                ok = !Check(v.get_int64().get(node.asset), "\"asset\" must be a whole number");
                if (ok && node.asset < 0)
                    return Fail("\"asset\" must not be negative");
            } else if (key == "position") {
                ok = ParseFloats(v, &node.transform.position.x, 3, "position");
            } else if (key == "rotation") {
                ok = ParseFloats(v, &node.transform.rotation.x, 4, "rotation");
            } else if (key == "scale") {
                ok = ParseFloats(v, &node.transform.scale.x, 3, "scale");
            } else if (key == "halfExtents") {
                ok = ParseFloats(v, &node.halfExtents.x, 3, "halfExtents");
            } else if (key == "children") {
                node.hasChildren = true;
                ok = ParseChildren(v, index, depth + 1);
            }
            if (!ok)
                return false;
        }

        ParsedNode& node = m_map.nodes[index];
        if (!node.hasType)
            return Fail("a node has no \"type\"");
        if (node.hasChildren && node.kind != NodeKind::Group)
            return Fail("item \"" + std::string(node.name) + "\" has children (only groups can)");
        m_map.nameChars += node.name.size();
        if (node.kind != NodeKind::Group) {
            // Keep authored values exactly; only repair what would break rendering.
            XMFLOAT4& q = node.transform.rotation;
            const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            if (length < 1e-6f)
                q = {0.0f, 0.0f, 0.0f, 1.0f};
            else if (std::abs(length - 1.0f) > 1e-3f)
                q = {q.x / length, q.y / length, q.z / length, q.w / length};
            for (float* s : {&node.transform.scale.x, &node.transform.scale.y, &node.transform.scale.z})
                if (std::abs(*s) < kMinScale)
                    *s = *s < 0.0f ? -kMinScale : kMinScale;
            for (float* h : {&node.halfExtents.x, &node.halfExtents.y, &node.halfExtents.z})
                *h = std::abs(*h);
        }
        return true;
    }

    ParsedMap& m_map;
    std::string m_error;
};

bool ReadWholeFile(const std::filesystem::path& path, simdjson::padded_string& out, std::string& error)
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) {
        error = std::filesystem::exists(path) ? "the file could not be opened" : "file not found";
        return false;
    }
    _fseeki64(file, 0, SEEK_END);
    const long long length = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);
    if (length < 0) {
        std::fclose(file);
        error = "the file could not be read";
        return false;
    }
    simdjson::padded_string buffer(static_cast<size_t>(length)); // one allocation, with simdjson's padding
    const bool ok = std::fread(buffer.data(), 1, size_t(length), file) == size_t(length);
    std::fclose(file);
    if (!ok) {
        error = "the file could not be read";
        return false;
    }
    out = std::move(buffer);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------------- save

std::string WriteMapJson(const SceneTree& scene, const AssetCatalog& catalog, const MapCamera& camera,
                         const std::filesystem::path& mapFolder, MapSaveInfo* info)
{
    // Imported palette items become "assets"; items refer to them by index.
    const std::span<const CatalogItem> items = catalog.Items();
    std::vector<int32_t> assetOfMesh;
    std::vector<size_t> assetItems;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!catalog.IsImported(i))
            continue;
        const auto mesh = static_cast<uint32_t>(items[i].mesh);
        if (assetOfMesh.size() <= mesh)
            assetOfMesh.resize(mesh + 1, -1);
        assetOfMesh[mesh] = static_cast<int32_t>(assetItems.size());
        assetItems.push_back(i);
    }

    JsonWriter out(256 + size_t(scene.Size()) * 160);
    out.Raw("{\n  \"format\": \"LevelMinded map\",\n  \"version\": ");
    out.Raw(std::to_string(kMapFormatVersion));
    out.Raw(",\n  \"camera\": {\"target\": ");
    out.Floats(&camera.target.x, 3);
    out.Raw(", \"yaw\": ");
    out.Float(camera.yaw);
    out.Raw(", \"pitch\": ");
    out.Float(camera.pitch);
    out.Raw(", \"distance\": ");
    out.Float(camera.distance);
    out.Raw("},\n  \"assets\": [");

    std::error_code ec;
    const std::filesystem::path folder = mapFolder.empty() ? std::filesystem::path() : std::filesystem::absolute(mapFolder, ec).lexically_normal();
    for (size_t a = 0; a < assetItems.size(); ++a) {
        const CatalogItem& item = items[assetItems[a]];
        const std::filesystem::path absolute = std::filesystem::absolute(catalog.Source(assetItems[a]), ec).lexically_normal();
        std::filesystem::path relative;
        if (!folder.empty() && absolute.root_name() == folder.root_name())
            relative = absolute.lexically_relative(folder);
        out.Raw(a ? ",\n    {" : "\n    {");
        out.Key("name");
        out.String(item.name);
        out.Raw(", ");
        out.Key("format");
        out.String(item.format ? item.format : "");
        out.Raw(", ");
        out.Key("path");
        out.String(relative.empty() ? GenericUtf8(absolute) : GenericUtf8(relative));
        out.Raw(", ");
        out.Key("absolutePath");
        out.String(GenericUtf8(absolute));
        out.Raw(", ");
        out.Key("halfExtents");
        out.Floats(&item.halfExtents.x, 3);
        out.Raw("}");
    }
    out.Raw(assetItems.empty() ? "],\n  \"nodes\": [" : "\n  ],\n  \"nodes\": [");
    if (scene.Size() > 0) {
        out.Raw("\n");
        const std::vector<int32_t>& lookup = assetOfMesh;
        WriteNodes(WriteContext{scene, lookup, out}, 0, scene.Size(), 2);
        out.Raw("  ");
    }
    out.Raw("]\n}\n");

    std::string json = out.Take();
    if (info) {
        info->bytes = json.size();
        info->nodes = scene.Size();
        info->assets = static_cast<uint32_t>(assetItems.size());
    }
    return json;
}

bool SaveMap(const std::filesystem::path& path, const SceneTree& scene, const AssetCatalog& catalog,
             const MapCamera& camera, std::string& error, MapSaveInfo* info)
{
    const std::string json = WriteMapJson(scene, catalog, camera, path.parent_path(), info);

    // Write next to the target, then swap it in: a failed or interrupted save leaves the old map intact.
    std::filesystem::path temporary = path;
    temporary += L".saving";
    FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || !file) {
        error = "the file could not be created (is the folder read-only?)";
        return false;
    }
    const bool written = std::fwrite(json.data(), 1, json.size(), file) == json.size();
    const bool flushed = std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    std::error_code ec;
    if (!written || !flushed || !closed) {
        std::filesystem::remove(temporary, ec);
        error = "writing the file failed (is the disk full?)";
        return false;
    }
    std::filesystem::rename(temporary, path, ec); // replaces an existing map
    if (ec) {
        std::filesystem::remove(temporary, ec);
        error = "the map file could not be replaced (is it open in another program?)";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------- load

bool LoadMap(const std::filesystem::path& path, SceneTree& scene, AssetCatalog& catalog, MeshLibrary& meshes,
             MapLoadReport& report, std::string& error)
{
    report = MapLoadReport{};

    // 1. Read and parse everything. Nothing in the editor changes until this succeeded.
    simdjson::padded_string json;
    if (!ReadWholeFile(path, json, error))
        return false;
    size_t skip = 0;
    if (json.size() >= 3 && std::memcmp(json.data(), "\xEF\xBB\xBF", 3) == 0)
        skip = 3; // UTF-8 byte order mark from an external editor
    // The parser must outlive step 3: parsed names are views into its string buffer.
    const auto parser = std::make_unique<ondemand::parser>();
    ondemand::document doc;
    if (const auto e = parser->iterate(json.data() + skip, json.size() - skip, json.size() + simdjson::SIMDJSON_PADDING - skip).get(doc)) {
        error = std::string("not a valid map file (") + simdjson::error_message(e) + ")";
        return false;
    }
    ParsedMap map;
    MapParser mapParser(map);
    if (!mapParser.Parse(doc)) {
        error = "this map can't be loaded: " + mapParser.Error();
        return false;
    }

    // 2. Re-import the models, still without touching the editor.
    const std::filesystem::path folder = path.parent_path();
    struct LoadedAsset {
        ImportResult result;
        std::filesystem::path source;
    };
    std::vector<LoadedAsset> loaded(map.assets.size());
    for (size_t a = 0; a < map.assets.size(); ++a) {
        const ParsedAsset& asset = map.assets[a];
        std::vector<std::filesystem::path> candidates;
        if (!asset.path.empty()) {
            const std::filesystem::path p = PathFromUtf8(asset.path);
            candidates.push_back(p.is_absolute() ? p : (folder / p).lexically_normal());
        }
        if (!asset.absolutePath.empty())
            candidates.push_back(PathFromUtf8(asset.absolutePath));
        std::error_code ec;
        for (const std::filesystem::path& candidate : candidates) {
            if (!std::filesystem::exists(candidate, ec))
                continue;
            loaded[a].result = ImportModel(candidate);
            loaded[a].source = candidate;
            if (loaded[a].result.ok)
                break;
        }
        if (loaded[a].source.empty())
            loaded[a].source = candidates.front(); // remembered, so saving again keeps the reference
    }

    // 3. Replace the editor's contents, releasing the old scene's memory.
    meshes.Reset();
    catalog.ResetImported();
    scene.Clear();

    std::vector<MeshId> assetMesh(map.assets.size(), MeshId::None);
    for (size_t a = 0; a < map.assets.size(); ++a) {
        const ParsedAsset& asset = map.assets[a];
        ImportResult& result = loaded[a].result;
        const std::string fileName = Utf8(loaded[a].source.filename());
        const char* format = asset.format == "glTF" ? "glTF" : asset.format == "OBJ" ? "OBJ" : (result.ok ? result.model.format : "glTF");
        std::string name = asset.name.empty() ? (result.ok ? result.model.name : Utf8(loaded[a].source.stem())) : std::string(asset.name);
        if (result.ok) {
            assetMesh[a] = meshes.Add(result.model.vertices, result.model.indices, result.model.traits);
            catalog.AddImported(name, DescribeImportedModel(result.model, fileName), format, assetMesh[a],
                                result.model.halfExtents, loaded[a].source);
        } else {
            // Missing file: a box of the recorded size stands in, and the entry keeps its path.
            assetMesh[a] = meshes.AddCopy(MeshId::Cube);
            catalog.AddImported(name, "Missing model file: " + fileName + " (shown as a box)", format, assetMesh[a],
                                asset.halfExtents, loaded[a].source);
            report.missingAssets.push_back(name);
        }
        result = ImportResult{}; // free the imported arrays as soon as they are copied into the library
    }

    scene.Reserve(static_cast<uint32_t>(map.nodes.size()), map.nameChars);
    std::vector<NodeId> ids(map.nodes.size(), kInvalidNode);
    for (size_t n = 0; n < map.nodes.size(); ++n) {
        const ParsedNode& node = map.nodes[n];
        const NodeId parent = node.parent == UINT32_MAX ? kInvalidNode : ids[node.parent];
        NodeId id;
        if (node.kind == NodeKind::Group) {
            id = scene.CreateGroup(node.name, parent);
            if (!node.expanded)
                scene.SetExpanded(id, false);
        } else {
            const MeshId mesh = node.asset >= 0 ? assetMesh[size_t(node.asset)] : node.mesh;
            id = scene.CreateItem(node.name, node.kind, node.transform, node.halfExtents, parent, mesh);
            ++report.items;
        }
        // Children are created after this, so hiding here is O(1) and they inherit it as they arrive.
        if (!node.visible)
            scene.SetVisible(id, false);
        ids[n] = id;
    }

    report.nodes = scene.Size();
    report.assets = static_cast<uint32_t>(map.assets.size());
    report.hasCamera = map.hasCamera;
    report.camera = map.camera;
    return true;
}

} // namespace lm
