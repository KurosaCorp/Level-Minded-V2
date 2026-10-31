#pragma once

#include "scene/SceneTree.h"

#include <cstdint>

namespace lm {

// Colors shared by the hierarchy panel and the viewport. Packed with R in the low byte, which
// matches both IM_COL32 and LineBatch's R8G8B8A8_UNORM vertices.
constexpr uint32_t PackColor(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

inline constexpr uint32_t kSelectionColor = PackColor(255, 160, 40);

constexpr uint32_t KindColor(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Group:  return PackColor(222, 178, 92);
    case NodeKind::Brush:  return PackColor(120, 160, 220);
    case NodeKind::Mesh:   return PackColor(190, 190, 196);
    case NodeKind::Light:  return PackColor(250, 220, 90);
    case NodeKind::Entity: return PackColor(110, 210, 140);
    }
    return PackColor(255, 255, 255);
}

// Base color of solid meshes (shaded in the viewport); KindColor is for icons and wireframes.
constexpr uint32_t KindAlbedo(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Group:  return PackColor(200, 200, 200);
    case NodeKind::Brush:  return PackColor(150, 160, 178);
    case NodeKind::Mesh:   return PackColor(196, 186, 170);
    case NodeKind::Light:  return PackColor(255, 226, 120);
    case NodeKind::Entity: return PackColor(110, 205, 140);
    }
    return PackColor(255, 255, 255);
}

constexpr const char* KindName(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Group:  return "Group";
    case NodeKind::Brush:  return "Brush";
    case NodeKind::Mesh:   return "Mesh";
    case NodeKind::Light:  return "Light";
    case NodeKind::Entity: return "Entity";
    }
    return "?";
}

} // namespace lm
