#include "import/ModelImporter.h"

#include "render/MeshLibrary.h"

#include <cgltf.h>
#include <fast_obj.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace DirectX;

namespace lm {

namespace {

// ---- Paths: the loaders take UTF-8 `char` paths; files are opened with the wide-char CRT API so
// non-ASCII folder and file names work on Windows.

std::string ToUtf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path FromUtf8(const char* text)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text)));
}

FILE* OpenUtf8(const char* path)
{
    FILE* file = nullptr;
    return _wfopen_s(&file, FromUtf8(path).c_str(), L"rb") == 0 ? file : nullptr;
}

// cgltf file callbacks (the .gltf/.glb itself and external .bin buffers).
cgltf_result ReadFileCallback(const cgltf_memory_options*, const cgltf_file_options*, const char* path, cgltf_size* size,
                              void** data)
{
    FILE* file = OpenUtf8(path);
    if (!file)
        return cgltf_result_file_not_found;
    _fseeki64(file, 0, SEEK_END);
    const long long length = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);
    void* buffer = length >= 0 ? std::malloc(size_t(length) + 1) : nullptr;
    const bool ok = buffer && std::fread(buffer, 1, size_t(length), file) == size_t(length);
    std::fclose(file);
    if (!ok) {
        std::free(buffer);
        return cgltf_result_io_error;
    }
    *size = cgltf_size(length);
    *data = buffer;
    return cgltf_result_success;
}

void ReleaseFileCallback(const cgltf_memory_options*, const cgltf_file_options*, void* data)
{
    std::free(data);
}

// fast_obj file callbacks (the .obj and its .mtl libraries).
void* ObjOpen(const char* path, void*) { return OpenUtf8(path); }
void ObjClose(void* file, void*) { std::fclose(static_cast<FILE*>(file)); }
size_t ObjRead(void* file, void* dst, size_t bytes, void*) { return std::fread(dst, 1, bytes, static_cast<FILE*>(file)); }
unsigned long ObjSize(void* file, void*)
{
    FILE* f = static_cast<FILE*>(file);
    const long long position = _ftelli64(f);
    _fseeki64(f, 0, SEEK_END);
    const long long length = _ftelli64(f);
    _fseeki64(f, position, SEEK_SET);
    return length > 0 ? static_cast<unsigned long>(length) : 0ul;
}

// ---- Colors

float LinearToSrgb(float linear)
{
    const float c = std::clamp(linear, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// Material colors are linear (glTF by definition; OBJ exporters such as Blender write linear Kd).
uint32_t PackLinearColor(float r, float g, float b)
{
    const auto channel = [](float linear) { return uint32_t(LinearToSrgb(linear) * 255.0f + 0.5f); };
    return channel(r) | (channel(g) << 8) | (channel(b) << 16) | (255u << 24);
}

XMFLOAT3 FaceNormal(const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& c) // area-weighted, not normalized
{
    XMFLOAT3 n;
    XMStoreFloat3(&n, XMVector3Cross(XMVectorSubtract(XMLoadFloat3(&b), XMLoadFloat3(&a)),
                                     XMVectorSubtract(XMLoadFloat3(&c), XMLoadFloat3(&a))));
    return n;
}

// ---- Shared last step: Y-up -> Z-up, fit into [-1, 1]^3.

bool FinishModel(ImportedModel& model, std::string& error)
{
    if (model.indices.empty() || model.vertices.empty()) {
        error = "The file contains no triangles.";
        return false;
    }
    XMVECTOR lo = XMVectorReplicate(INFINITY);
    XMVECTOR hi = XMVectorReplicate(-INFINITY);
    for (MeshVertex& v : model.vertices) {
        v.position = {v.position.x, -v.position.z, v.position.y};
        v.normal = {v.normal.x, -v.normal.z, v.normal.y};
        const XMVECTOR p = XMLoadFloat3(&v.position);
        lo = XMVectorMin(lo, p);
        hi = XMVectorMax(hi, p);
    }
    if (XMVector3IsNaN(lo) || XMVector3IsNaN(hi) || XMVector3IsInfinite(lo) || XMVector3IsInfinite(hi)) {
        error = "The file contains invalid (non-finite) vertex positions.";
        return false;
    }

    const XMVECTOR center = XMVectorScale(XMVectorAdd(lo, hi), 0.5f);
    XMVECTOR half = XMVectorScale(XMVectorSubtract(hi, lo), 0.5f);
    // A flat model (e.g. a single plane) gets a thin but non-zero size along its flat axis.
    XMFLOAT3 h;
    XMStoreFloat3(&h, half);
    const float minimum = std::max(std::max({h.x, h.y, h.z}) * 1e-3f, 1e-5f);
    h = {std::max(h.x, minimum), std::max(h.y, minimum), std::max(h.z, minimum)};
    half = XMLoadFloat3(&h);
    const XMVECTOR invHalf = XMVectorReciprocal(half);

    for (MeshVertex& v : model.vertices) {
        XMStoreFloat3(&v.position, XMVectorMultiply(XMVectorSubtract(XMLoadFloat3(&v.position), center), invHalf));
        // Normals transform by the inverse transpose of the scale (1 / half), i.e. by half. The
        // shader divides by the item size again, which gives back the original direction.
        const XMVECTOR n = XMVectorMultiply(XMLoadFloat3(&v.normal), half);
        XMStoreFloat3(&v.normal, XMVectorGetX(XMVector3LengthSq(n)) > 1e-20f ? XMVector3Normalize(n) : g_XMIdentityR2);
    }
    model.halfExtents = h;
    return true;
}

// ---- glTF

const char* CgltfError(cgltf_result result)
{
    switch (result) {
    case cgltf_result_file_not_found: return "File not found.";
    case cgltf_result_io_error: return "The file could not be read.";
    case cgltf_result_invalid_json: return "Not a valid glTF file (broken JSON).";
    case cgltf_result_invalid_gltf: return "Not a valid glTF file.";
    case cgltf_result_unknown_format: return "Not a glTF file.";
    case cgltf_result_data_too_short: return "The file is truncated.";
    case cgltf_result_out_of_memory: return "Out of memory.";
    case cgltf_result_legacy_gltf: return "glTF 1.0 files are not supported (glTF 2.0 only).";
    default: return "The glTF file could not be loaded.";
    }
}

const cgltf_accessor* FindAttribute(const cgltf_primitive& primitive, cgltf_attribute_type type)
{
    for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
        if (primitive.attributes[i].type == type && primitive.attributes[i].index == 0)
            return primitive.attributes[i].data;
    return nullptr;
}

struct GltfScratch {
    std::vector<float> positions, normals, colors;
    std::vector<uint32_t> indices, triangles;
};

// Appends one primitive, transformed by `world`. Returns false if it was skipped.
bool AppendPrimitive(const cgltf_primitive& primitive, FXMMATRIX world, CXMMATRIX normalMatrix, bool mirrored,
                     ImportedModel& model, GltfScratch& s)
{
    if (primitive.has_draco_mesh_compression)
        return false;
    if (primitive.type != cgltf_primitive_type_triangles && primitive.type != cgltf_primitive_type_triangle_strip &&
        primitive.type != cgltf_primitive_type_triangle_fan)
        return false; // points and lines have no surface

    const cgltf_accessor* position = FindAttribute(primitive, cgltf_attribute_type_position);
    if (!position || position->type != cgltf_type_vec3 || position->count == 0)
        return false;
    const cgltf_size count = position->count;
    s.positions.resize(count * 3);
    if (cgltf_accessor_unpack_floats(position, s.positions.data(), count * 3) != count * 3)
        return false; // missing or compressed buffer data

    const cgltf_accessor* normal = FindAttribute(primitive, cgltf_attribute_type_normal);
    bool hasNormals = normal && normal->type == cgltf_type_vec3 && normal->count == count;
    if (hasNormals) {
        s.normals.resize(count * 3);
        hasNormals = cgltf_accessor_unpack_floats(normal, s.normals.data(), count * 3) == count * 3;
    }
    const cgltf_accessor* color = FindAttribute(primitive, cgltf_attribute_type_color);
    const cgltf_size colorComponents = color ? cgltf_num_components(color->type) : 0;
    bool hasColors = color && (colorComponents == 3 || colorComponents == 4) && color->count == count;
    if (hasColors) {
        s.colors.resize(count * colorComponents);
        hasColors = cgltf_accessor_unpack_floats(color, s.colors.data(), count * colorComponents) == count * colorComponents;
    }

    float base[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (const cgltf_material* material = primitive.material) {
        if (material->has_pbr_metallic_roughness)
            std::memcpy(base, material->pbr_metallic_roughness.base_color_factor, sizeof(base));
        else if (material->has_pbr_specular_glossiness)
            std::memcpy(base, material->pbr_specular_glossiness.diffuse_factor, sizeof(base));
        if (material->double_sided)
            model.traits |= kMeshTraitDoubleSided;
    }

    // Indices of this primitive's vertex list, as a triangle list.
    s.indices.clear();
    if (primitive.indices) {
        s.indices.resize(primitive.indices->count);
        if (cgltf_accessor_unpack_indices(primitive.indices, s.indices.data(), sizeof(uint32_t), s.indices.size()) !=
            s.indices.size())
            return false;
    } else {
        s.indices.resize(count);
        for (cgltf_size i = 0; i < count; ++i)
            s.indices[i] = uint32_t(i);
    }
    s.triangles.clear();
    if (primitive.type == cgltf_primitive_type_triangles) {
        s.triangles.assign(s.indices.begin(), s.indices.end() - s.indices.size() % 3);
    } else if (primitive.type == cgltf_primitive_type_triangle_strip) {
        for (size_t i = 2; i < s.indices.size(); ++i) {
            const bool odd = (i & 1) != 0; // every other strip triangle is reversed to keep CCW
            s.triangles.insert(s.triangles.end(), {s.indices[i - 2], s.indices[odd ? i : i - 1], s.indices[odd ? i - 1 : i]});
        }
    } else {
        for (size_t i = 2; i < s.indices.size(); ++i)
            s.triangles.insert(s.triangles.end(), {s.indices[0], s.indices[i - 1], s.indices[i]});
    }
    for (uint32_t index : s.triangles)
        if (index >= count)
            return false;
    if (s.triangles.empty())
        return false;

    const auto vertexAt = [&](uint32_t i) {
        MeshVertex v;
        XMStoreFloat3(&v.position, XMVector3TransformCoord(XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(&s.positions[i * 3])), world));
        if (hasNormals) {
            const XMVECTOR n = XMVector3TransformNormal(XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(&s.normals[i * 3])), normalMatrix);
            XMStoreFloat3(&v.normal, XMVector3Normalize(n));
        } else {
            v.normal = {0.0f, 0.0f, 0.0f};
        }
        float r = base[0], g = base[1], b = base[2];
        if (hasColors) {
            r *= s.colors[i * colorComponents];
            g *= s.colors[i * colorComponents + 1];
            b *= s.colors[i * colorComponents + 2];
        }
        v.color = PackLinearColor(r, g, b);
        return v;
    };

    const auto first = static_cast<uint32_t>(model.vertices.size());
    if (hasNormals) { // shared vertices, as authored
        for (cgltf_size i = 0; i < count; ++i)
            model.vertices.push_back(vertexAt(uint32_t(i)));
        for (size_t t = 0; t < s.triangles.size(); t += 3) {
            const uint32_t a = first + s.triangles[t], b = first + s.triangles[t + 1], c = first + s.triangles[t + 2];
            model.indices.insert(model.indices.end(), {a, mirrored ? c : b, mirrored ? b : c});
        }
    } else { // no normals: glTF asks for flat shading, so every triangle gets its own corners
        for (size_t t = 0; t < s.triangles.size(); t += 3) {
            MeshVertex corner[3] = {vertexAt(s.triangles[t]), vertexAt(s.triangles[t + 1]), vertexAt(s.triangles[t + 2])};
            if (mirrored)
                std::swap(corner[1], corner[2]);
            XMFLOAT3 n = FaceNormal(corner[0].position, corner[1].position, corner[2].position);
            XMStoreFloat3(&n, XMVector3Normalize(XMLoadFloat3(&n)));
            const auto base0 = static_cast<uint32_t>(model.vertices.size());
            for (MeshVertex& v : corner) {
                v.normal = n;
                model.vertices.push_back(v);
            }
            model.indices.insert(model.indices.end(), {base0, base0 + 1, base0 + 2});
        }
    }
    return true;
}

bool LoadGltf(const std::filesystem::path& path, ImportedModel& model, std::string& error)
{
    const std::string utf8 = ToUtf8(path);
    cgltf_options options{};
    options.file.read = ReadFileCallback;
    options.file.release = ReleaseFileCallback;

    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse_file(&options, utf8.c_str(), &data);
    if (result == cgltf_result_success)
        result = cgltf_load_buffers(&options, data, utf8.c_str());
    if (result == cgltf_result_success)
        result = cgltf_validate(data);
    if (result != cgltf_result_success) {
        error = result == cgltf_result_file_not_found && data ? "A buffer file (.bin) it refers to is missing."
                                                              : CgltfError(result);
        cgltf_free(data);
        return false;
    }

    GltfScratch scratch;
    bool usesDraco = false;
    const auto appendMesh = [&](const cgltf_mesh& mesh, FXMMATRIX world) {
        const XMMATRIX normalMatrix = XMMatrixTranspose(XMMatrixInverse(nullptr, world));
        const bool mirrored = XMVectorGetX(XMMatrixDeterminant(world)) < 0.0f;
        for (cgltf_size p = 0; p < mesh.primitives_count; ++p) {
            usesDraco |= mesh.primitives[p].has_draco_mesh_compression != 0;
            if (!AppendPrimitive(mesh.primitives[p], world, normalMatrix, mirrored, model, scratch))
                ++model.skippedPrimitives;
        }
    };

    if (data->nodes_count == 0) {
        for (cgltf_size m = 0; m < data->meshes_count; ++m) // meshes without a node tree
            appendMesh(data->meshes[m], XMMatrixIdentity());
    } else {
        // Walk the default scene (or the first one, or every root node), depth first.
        std::vector<const cgltf_node*> stack;
        const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
        if (scene) {
            for (cgltf_size i = scene->nodes_count; i-- > 0;)
                stack.push_back(scene->nodes[i]);
        } else {
            for (cgltf_size i = data->nodes_count; i-- > 0;)
                if (!data->nodes[i].parent)
                    stack.push_back(&data->nodes[i]);
        }
        size_t visited = 0;
        while (!stack.empty() && visited++ <= data->nodes_count) {
            const cgltf_node* node = stack.back();
            stack.pop_back();
            if (node->mesh) {
                XMFLOAT4X4 world; // cgltf: column-major for column vectors == row-major for row vectors
                cgltf_node_transform_world(node, &world.m[0][0]);
                appendMesh(*node->mesh, XMLoadFloat4x4(&world));
            }
            for (cgltf_size c = node->children_count; c-- > 0;)
                stack.push_back(node->children[c]);
        }
    }
    cgltf_free(data);

    if (model.indices.empty() && usesDraco) {
        error = "The model uses Draco mesh compression, which is not supported yet.";
        return false;
    }
    return true;
}

// ---- OBJ

bool LoadObj(const std::filesystem::path& path, ImportedModel& model, std::string& error)
{
    const std::string utf8 = ToUtf8(path);
    const fastObjCallbacks callbacks{ObjOpen, ObjClose, ObjRead, ObjSize};
    fastObjMesh* obj = fast_obj_read_with_callbacks(utf8.c_str(), &callbacks, nullptr);
    if (!obj) {
        error = std::filesystem::exists(path) ? "The OBJ file could not be read." : "File not found.";
        return false;
    }

    // Weld face corners that share position, normal and material into one vertex (open addressing).
    struct Slot {
        uint32_t p, n, material, vertex;
    };
    size_t capacity = 16;
    while (capacity < size_t(obj->index_count) * 2)
        capacity *= 2;
    std::vector<Slot> slots(capacity, Slot{0, 0, 0, UINT32_MAX});
    std::vector<uint8_t> needsNormal; // per vertex: no normal in the file, accumulate face normals
    model.vertices.reserve(obj->index_count);
    model.indices.reserve(size_t(obj->index_count) * 2);

    const auto vertexFor = [&](const fastObjIndex& index, uint32_t material) -> uint32_t {
        size_t h = (size_t(index.p) * 73856093u ^ size_t(index.n) * 19349663u ^ size_t(material) * 83492791u) & (capacity - 1);
        for (;; h = (h + 1) & (capacity - 1)) {
            Slot& slot = slots[h];
            if (slot.vertex == UINT32_MAX) {
                MeshVertex v;
                v.position = {obj->positions[index.p * 3], obj->positions[index.p * 3 + 1], obj->positions[index.p * 3 + 2]};
                const bool hasNormal = index.n > 0 && index.n < obj->normal_count;
                v.normal = hasNormal ? XMFLOAT3{obj->normals[index.n * 3], obj->normals[index.n * 3 + 1], obj->normals[index.n * 3 + 2]}
                                     : XMFLOAT3{0.0f, 0.0f, 0.0f};
                const float* kd = material < obj->material_count ? obj->materials[material].Kd : nullptr;
                v.color = kd ? PackLinearColor(kd[0], kd[1], kd[2]) : 0xFFFFFFFFu;
                slot = {index.p, index.n, material, static_cast<uint32_t>(model.vertices.size())};
                model.vertices.push_back(v);
                needsNormal.push_back(hasNormal ? 0 : 1);
                return slot.vertex;
            }
            if (slot.p == index.p && slot.n == index.n && slot.material == material)
                return slot.vertex;
        }
    };

    uint32_t corner = 0;
    uint32_t polygon[256];
    for (unsigned int f = 0; f < obj->face_count; ++f) {
        const unsigned int count = obj->face_vertices[f];
        const uint32_t material = obj->face_materials ? obj->face_materials[f] : 0;
        bool valid = count >= 3 && count <= 256;
        for (unsigned int k = 0; valid && k < count; ++k) {
            const fastObjIndex& index = obj->indices[corner + k];
            valid = index.p > 0 && index.p < obj->position_count;
        }
        if (valid) {
            for (unsigned int k = 0; k < count; ++k)
                polygon[k] = vertexFor(obj->indices[corner + k], material);
            for (unsigned int k = 1; k + 1 < count; ++k) { // fan; OBJ faces are counter-clockwise
                const uint32_t a = polygon[0], b = polygon[k], c = polygon[k + 1];
                model.indices.insert(model.indices.end(), {a, b, c});
                if (needsNormal[a] | needsNormal[b] | needsNormal[c]) {
                    const XMFLOAT3 n = FaceNormal(model.vertices[a].position, model.vertices[b].position, model.vertices[c].position);
                    for (uint32_t v : {a, b, c}) {
                        if (needsNormal[v]) {
                            XMFLOAT3& sum = model.vertices[v].normal;
                            sum = {sum.x + n.x, sum.y + n.y, sum.z + n.z};
                        }
                    }
                }
            }
        }
        corner += count;
    }
    fast_obj_destroy(obj);

    // Smooth normals for corners the file gave none.
    for (size_t v = 0; v < model.vertices.size(); ++v) {
        if (!needsNormal[v])
            continue;
        XMFLOAT3& n = model.vertices[v].normal;
        const XMVECTOR sum = XMLoadFloat3(&n);
        XMStoreFloat3(&n, XMVectorGetX(XMVector3LengthSq(sum)) > 1e-30f ? XMVector3Normalize(sum) : g_XMIdentityR1);
    }
    (void)error;
    return true;
}

std::string LowerExtension(const std::filesystem::path& path)
{
    std::string extension = ToUtf8(path.extension());
    for (char& c : extension)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return extension;
}

} // namespace

std::string WithThousandsSeparators(size_t value)
{
    std::string digits = std::to_string(value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
        digits.insert(static_cast<size_t>(i), 1, ',');
    return digits;
}

std::string DescribeImportedModel(const ImportedModel& model, const std::string& fileName)
{
    return std::string(model.format) + " model, " + WithThousandsSeparators(model.indices.size() / 3) + " triangles (" +
           fileName + ")";
}

bool IsImportableModel(const std::filesystem::path& path)
{
    const std::string extension = LowerExtension(path);
    return extension == ".gltf" || extension == ".glb" || extension == ".obj";
}

ImportResult ImportModel(const std::filesystem::path& path)
{
    ImportResult result;
    ImportedModel& model = result.model;
    model.name = ToUtf8(path.stem());
    const std::string extension = LowerExtension(path);

    bool loaded = false;
    if (extension == ".gltf" || extension == ".glb") {
        model.format = "glTF";
        loaded = LoadGltf(path, model, result.error);
    } else if (extension == ".obj") {
        model.format = "OBJ";
        loaded = LoadObj(path, model, result.error);
    } else {
        result.error = "Unsupported file type (use .gltf, .glb or .obj).";
    }
    if (loaded)
        loaded = FinishModel(model, result.error);
    if (!loaded) {
        model.vertices = {};
        model.indices = {};
        return result;
    }
    model.vertices.shrink_to_fit();
    model.indices.shrink_to_fit();
    result.ok = true;
    return result;
}

} // namespace lm
