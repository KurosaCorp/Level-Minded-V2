#pragma once

#include <DirectXMath.h>

namespace lm {

// Placement of an item: scale, then rotate, then translate (TRS), in world space.
//
// The item's shape is its mesh (modeled in [-1, 1]^3) scaled by the item's half extents, so
// scale = (1, 1, 1) means "catalog size". The pivot (position) is the center of that box.
struct Transform {
    DirectX::XMFLOAT3 position{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT4 rotation{0.0f, 0.0f, 0.0f, 1.0f}; // unit quaternion (x, y, z, w)
    DirectX::XMFLOAT3 scale{1.0f, 1.0f, 1.0f};
};

inline bool operator==(const Transform& a, const Transform& b)
{
    return a.position.x == b.position.x && a.position.y == b.position.y && a.position.z == b.position.z &&
           a.rotation.x == b.rotation.x && a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z &&
           a.rotation.w == b.rotation.w && a.scale.x == b.scale.x && a.scale.y == b.scale.y && a.scale.z == b.scale.z;
}

// Smallest |scale| a transform tool may produce; avoids degenerate (non-invertible) items.
inline constexpr float kMinScale = 1e-3f;

// Matrix from the unit box [-1, 1]^3 to world space for an item of the given half extents.
inline DirectX::XMMATRIX ItemMatrix(const Transform& t, const DirectX::XMFLOAT3& halfExtents)
{
    using namespace DirectX;
    const XMVECTOR size = XMVectorMultiply(XMLoadFloat3(&t.scale), XMLoadFloat3(&halfExtents));
    return XMMatrixAffineTransformation(size, XMVectorZero(), XMLoadFloat4(&t.rotation), XMLoadFloat3(&t.position));
}

// ---- Transform-tool operations. Each maps a transform captured when the drag started to its new
// value for the drag's total delta, so long drags never accumulate error.

// Moves by `delta` (world space).
inline Transform Translated(const Transform& start, DirectX::FXMVECTOR delta)
{
    using namespace DirectX;
    Transform t = start;
    XMStoreFloat3(&t.position, XMVectorAdd(XMLoadFloat3(&start.position), delta));
    return t;
}

// Rotates by `rotation` (world-space quaternion) around `pivot`: both the orientation and the
// position relative to the pivot turn.
inline Transform RotatedAbout(const Transform& start, DirectX::FXMVECTOR pivot, DirectX::FXMVECTOR rotation)
{
    using namespace DirectX;
    Transform t = start;
    const XMVECTOR offset = XMVectorSubtract(XMLoadFloat3(&start.position), pivot);
    XMStoreFloat3(&t.position, XMVectorAdd(pivot, XMVector3Rotate(offset, rotation)));
    // XMQuaternionMultiply(a, b) applies a first, then b.
    XMStoreFloat4(&t.rotation, XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&start.rotation), rotation)));
    return t;
}

// Scales by `factor` along the axes of `frame` (a world-space quaternion; identity = world axes)
// around `pivot`. The item's offset from the pivot is scaled in that frame. Its own scale is scaled
// along its local axes, taking from `factor` the component of whichever frame axis each local axis
// lines up with best (rotated items cannot shear, so this is exact when the axes are aligned).
inline Transform ScaledAbout(const Transform& start, DirectX::FXMVECTOR pivot, DirectX::FXMVECTOR frame,
                             DirectX::GXMVECTOR factor)
{
    using namespace DirectX;
    Transform t = start;

    // Offset from the pivot, expressed in the frame, scaled, and turned back to world.
    const XMVECTOR inverseFrame = XMQuaternionInverse(frame);
    const XMVECTOR offset = XMVector3Rotate(XMVectorSubtract(XMLoadFloat3(&start.position), pivot), inverseFrame);
    XMStoreFloat3(&t.position, XMVectorAdd(pivot, XMVector3Rotate(XMVectorMultiply(offset, factor), frame)));

    // Item axis i in frame space; pick the frame axis it is most aligned with.
    XMFLOAT3 f;
    XMStoreFloat3(&f, factor);
    const float factors[3] = {f.x, f.y, f.z};
    const XMVECTOR toFrame = XMQuaternionMultiply(XMLoadFloat4(&start.rotation), inverseFrame);
    float* const scale[3] = {&t.scale.x, &t.scale.y, &t.scale.z};
    const XMVECTOR axes[3] = {g_XMIdentityR0, g_XMIdentityR1, g_XMIdentityR2};
    for (int i = 0; i < 3; ++i) {
        XMFLOAT3 a;
        XMStoreFloat3(&a, XMVectorAbs(XMVector3Rotate(axes[i], toFrame)));
        const int best = (a.x >= a.y && a.x >= a.z) ? 0 : (a.y >= a.z ? 1 : 2);
        float s = *scale[i] * factors[best];
        if (s > -kMinScale && s < kMinScale)
            s = s < 0.0f ? -kMinScale : kMinScale;
        *scale[i] = s;
    }
    return t;
}

} // namespace lm
