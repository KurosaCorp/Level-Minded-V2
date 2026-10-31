#pragma once

#include "render/MeshLibrary.h"
#include "scene/ItemCatalog.h"
#include "scene/SceneTree.h"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lm {

// Map files (.lmmap): UTF-8 JSON, one line per item, readable and diffable.
//
//   {
//     "format": "LevelMinded map", "version": 1,
//     "camera": {"target": [x, y, z], "yaw": r, "pitch": r, "distance": m},
//     "assets": [{"name": "Chair", "format": "glTF", "path": "models/chair.glb",
//                 "absolutePath": "C:/maps/models/chair.glb", "halfExtents": [x, y, z]}],
//     "nodes": [
//       {"type": "group", "name": "World", "children": [
//         {"type": "brush", "name": "Floor", "mesh": "cube", "position": [0, 0, -0.1],
//          "rotation": [x, y, z, w], "scale": [x, y, z], "halfExtents": [12, 10, 0.1]},
//         {"type": "mesh", "name": "Chair_01", "asset": 0, "position": [2, 0, 0.45], "halfExtents": [...]}
//       ]}
//     ]
//   }
//
// Node types: group, brush, mesh, light, entity. "mesh" is a built-in shape (none, cube, sphere,
// cylinder, spawn); "asset" instead indexes "assets" (imported models, re-imported from their files on
// load, path relative to the map first). Optional fields are written only when they differ from
// their defaults: "visible": false, "expanded": false (groups), "rotation" (identity), "scale" (1).
// Floats are written in their shortest exact form and read back bit for bit.

inline constexpr int kMapFormatVersion = 1;
inline constexpr const wchar_t* kMapFileExtension = L".lmmap";

struct MapCamera {
    DirectX::XMFLOAT3 target{};
    float yaw = 0.0f;
    float pitch = 0.0f;
    float distance = 0.0f;
};

struct MapSaveInfo {
    size_t bytes = 0;
    uint32_t nodes = 0;
    uint32_t assets = 0;
};

// The JSON text of a map. Asset paths are written relative to `mapFolder` when possible.
std::string WriteMapJson(const SceneTree& scene, const AssetCatalog& catalog, const MapCamera& camera,
                         const std::filesystem::path& mapFolder, MapSaveInfo* info = nullptr);

// Writes the map to `path` safely: to a temporary file next to it first, which then replaces the
// target, so a failed save never leaves a half-written map behind. Returns false with `error` set.
bool SaveMap(const std::filesystem::path& path, const SceneTree& scene, const AssetCatalog& catalog,
             const MapCamera& camera, std::string& error, MapSaveInfo* info = nullptr);

struct MapLoadReport {
    uint32_t nodes = 0;
    uint32_t items = 0;
    uint32_t assets = 0;
    std::vector<std::string> missingAssets; // models whose files could not be imported (UTF-8 names)
    bool hasCamera = false;
    MapCamera camera;
};

// Replaces the scene, the imported palette items and the imported meshes with the map's contents.
// The file is parsed and its models re-imported before anything is touched, so if loading fails the
// current scene stays exactly as it was. The old scene's memory is released (SceneTree::Clear,
// MeshLibrary::Reset, AssetCatalog::ResetImported). Models whose files are missing are replaced by
// box-shaped stand-ins that keep their asset entry, so saving again does not lose them.
bool LoadMap(const std::filesystem::path& path, SceneTree& scene, AssetCatalog& catalog, MeshLibrary& meshes,
             MapLoadReport& report, std::string& error);

} // namespace lm
