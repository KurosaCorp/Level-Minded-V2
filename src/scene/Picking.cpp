#include "scene/Picking.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace lm {

bool IntersectUnitBox(FXMVECTOR origin, FXMVECTOR direction, float& tEnter, float& tExit)
{
    XMFLOAT3 o, d;
    XMStoreFloat3(&o, origin);
    XMStoreFloat3(&d, direction);
    const float origins[3] = {o.x, o.y, o.z};
    const float directions[3] = {d.x, d.y, d.z};

    float enter = -INFINITY;
    float exit = INFINITY;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(directions[axis]) < 1e-12f) { // parallel to this slab
            if (origins[axis] < -1.0f || origins[axis] > 1.0f)
                return false;
            continue;
        }
        const float inv = 1.0f / directions[axis];
        float t0 = (-1.0f - origins[axis]) * inv;
        float t1 = (1.0f - origins[axis]) * inv;
        if (t0 > t1)
            std::swap(t0, t1);
        enter = std::max(enter, t0);
        exit = std::min(exit, t1);
        if (enter > exit)
            return false;
    }
    if (exit < 0.0f)
        return false; // box is behind the ray
    tEnter = enter;
    tExit = exit;
    return true;
}

float IntersectTriangle(FXMVECTOR origin, FXMVECTOR direction, FXMVECTOR a, GXMVECTOR b, HXMVECTOR c)
{
    const XMVECTOR edge1 = XMVectorSubtract(b, a);
    const XMVECTOR edge2 = XMVectorSubtract(c, a);
    const XMVECTOR p = XMVector3Cross(direction, edge2);
    const float det = XMVectorGetX(XMVector3Dot(edge1, p));
    if (std::abs(det) < 1e-12f)
        return -1.0f;
    const float invDet = 1.0f / det;
    const XMVECTOR s = XMVectorSubtract(origin, a);
    const float u = XMVectorGetX(XMVector3Dot(s, p)) * invDet;
    if (u < 0.0f || u > 1.0f)
        return -1.0f;
    const XMVECTOR q = XMVector3Cross(s, edge1);
    const float v = XMVectorGetX(XMVector3Dot(direction, q)) * invDet;
    if (v < 0.0f || u + v > 1.0f)
        return -1.0f;
    return XMVectorGetX(XMVector3Dot(edge2, q)) * invDet;
}

PickHit PickItem(const SceneTree& scene, const MeshLibrary& meshes, const Ray& ray)
{
    const std::span<const MeshVertex> vertices = meshes.Vertices();
    const std::span<const uint32_t> indices = meshes.Indices();
    const XMVECTOR worldOrigin = XMLoadFloat3(&ray.origin);
    const XMVECTOR worldDirection = XMLoadFloat3(&ray.direction);

    PickHit solid;
    solid.distance = INFINITY;
    NodeId volume = kInvalidNode;
    float volumeDistance = INFINITY;
    uint32_t itemsTested = 0;
    uint32_t trianglesTested = 0;

    // One linear pass over the node array; hidden subtrees are skipped in one jump.
    for (uint32_t i = 0; i < scene.Size();) {
        const SceneTree::Node& node = scene.NodeAt(i);
        if (!node.Has(SceneTree::kEffectivelyVisible)) {
            i = scene.SubtreeEnd(i);
            continue;
        }
        if (node.IsGroup()) {
            ++i;
            continue;
        }

        // Ray in the item's unit-box space. The direction is not renormalized, so distances along
        // it stay world distances and are comparable between items.
        const ItemData& item = scene.ItemAt(i);
        const XMVECTOR size = XMVectorMultiply(XMLoadFloat3(&item.transform.scale), XMLoadFloat3(&item.halfExtents));
        if (!XMVector3Greater(XMVectorAbs(size), XMVectorReplicate(1e-12f))) {
            ++i; // flat or empty along some axis: nothing to click
            continue;
        }
        ++itemsTested;

        // Cheap reject first: the ray must pass within the box's bounding sphere, ahead of the
        // origin and no farther than the best solid hit so far (solids beat volumes at any distance,
        // so a volume hit can't rule anything out). Most items stop here.
        const XMVECTOR toCenter = XMVectorSubtract(XMLoadFloat3(&item.transform.position), worldOrigin);
        const float along = XMVectorGetX(XMVector3Dot(toCenter, worldDirection));
        const float radiusSq = XMVectorGetX(XMVector3LengthSq(size));
        const float missSq = XMVectorGetX(XMVector3LengthSq(toCenter)) - along * along;
        const float radius = std::sqrt(radiusSq);
        if (missSq > radiusSq || along + radius < 0.0f || along - radius > solid.distance) {
            ++i;
            continue;
        }

        const XMVECTOR inverseRotation = XMQuaternionConjugate(XMLoadFloat4(&item.transform.rotation));
        const XMVECTOR origin = XMVectorDivide(
            XMVector3Rotate(XMVectorSubtract(worldOrigin, XMLoadFloat3(&item.transform.position)), inverseRotation), size);
        const XMVECTOR direction = XMVectorDivide(XMVector3Rotate(worldDirection, inverseRotation), size);

        float tEnter, tExit;
        if (!IntersectUnitBox(origin, direction, tEnter, tExit)) {
            ++i;
            continue;
        }

        const MeshRange& range = meshes.Get(item.mesh).range;
        if (item.mesh == MeshId::None || range.indexCount == 0) {
            // Volume: hit where the ray enters the box (or leaves it, when starting inside).
            const float t = tEnter >= 0.0f ? tEnter : tExit;
            if (t < volumeDistance) {
                volumeDistance = t;
                volume = node.id;
            }
        } else if (tEnter < solid.distance) { // the mesh can't be closer than its box
            const uint32_t* triangle = indices.data() + range.firstIndex;
            const MeshVertex* base = vertices.data() + range.baseVertex;
            for (uint32_t k = 0; k + 2 < range.indexCount; k += 3, triangle += 3) {
                const float t = IntersectTriangle(origin, direction, XMLoadFloat3(&base[triangle[0]].position),
                                                  XMLoadFloat3(&base[triangle[1]].position), XMLoadFloat3(&base[triangle[2]].position));
                if (t >= 0.0f && t < solid.distance) {
                    solid.distance = t;
                    solid.node = node.id;
                }
            }
            trianglesTested += range.indexCount / 3;
        }
        ++i;
    }

    PickHit result = solid.node != kInvalidNode ? solid : PickHit{volume, volumeDistance};
    result.itemsTested = itemsTested;
    result.trianglesTested = trianglesTested;
    return result;
}

} // namespace lm
