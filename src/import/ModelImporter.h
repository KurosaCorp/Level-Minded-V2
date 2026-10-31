#pragma once

#include "render/MeshGeometry.h"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lm {

// A 3D model file turned into one mesh that fits the editor's conventions:
//   - Z-up (glTF and OBJ are Y-up: (x, y, z) -> (x, -z, y)), right-handed, 1 unit = 1 meter;
//   - glTF node transforms baked into the vertices, so the model keeps the placement, size and
//     orientation its author gave it;
//   - normalized into the unit box [-1, 1]^3 like the built-in meshes, with the real size kept in
//     `halfExtents`, so an item at scale 1 shows the model at its authored size;
//   - triangles counter-clockwise seen from outside (flipped where a node transform mirrors them);
//   - material base colors baked into the vertex colors (sRGB). Textures are not imported.
struct ImportedModel {
    std::string name;                 // file name without extension (UTF-8)
    const char* format = "";          // "glTF" or "OBJ"
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;    // triangle list
    DirectX::XMFLOAT3 halfExtents{};  // meters
    uint8_t traits = 0;               // MeshTrait bits (double-sided materials)
    uint32_t skippedPrimitives = 0;   // glTF primitives that were not triangles or could not be read
};

struct ImportResult {
    bool ok = false;
    std::string error; // why it failed, for the user (UTF-8)
    ImportedModel model;
};

// True for the supported file types: .gltf, .glb, .obj (case-insensitive).
bool IsImportableModel(const std::filesystem::path& path);

// Loads and converts a model file. Runs synchronously; memory is freed before returning except for
// the result's own two arrays.
ImportResult ImportModel(const std::filesystem::path& path);

} // namespace lm
