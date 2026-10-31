#pragma once

#include "render/MeshLibrary.h"
#include "scene/SceneTree.h"

#include <DirectXMath.h>

namespace lm {

struct Ray {
    DirectX::XMFLOAT3 origin;
    DirectX::XMFLOAT3 direction; // unit length
};

// Ray vs. the box [-1, 1]^3. On a hit returns true with the entry and exit distances (tEnter may be
// negative when the origin is inside). `direction` need not be unit length; distances are in its units.
bool IntersectUnitBox(DirectX::FXMVECTOR origin, DirectX::FXMVECTOR direction, float& tEnter, float& tExit);

// Two-sided ray/triangle test (Moller-Trumbore). Returns the distance along `direction`, or a
// negative value on a miss.
float IntersectTriangle(DirectX::FXMVECTOR origin, DirectX::FXMVECTOR direction, DirectX::FXMVECTOR a,
                        DirectX::GXMVECTOR b, DirectX::HXMVECTOR c);

struct PickHit {
    NodeId node = kInvalidNode;
    float distance = 0.0f;
    uint32_t itemsTested = 0;     // items considered (bounding-sphere test)
    uint32_t trianglesTested = 0; // triangles tested after the item's box test passed
    explicit operator bool() const { return node != kInvalidNode; }
};

// Finds the item under a ray (viewport click). Tests the exact triangles of each item's mesh (built-in
// or imported, read in place from `meshes`), after cheap bounding-sphere and box rejections. Only
// effectively visible items are hit. Volumes without a mesh (triggers) are hit on their box, but any
// solid item along the ray wins, since volumes usually enclose other things.
PickHit PickItem(const SceneTree& scene, const MeshLibrary& meshes, const Ray& ray);

} // namespace lm
