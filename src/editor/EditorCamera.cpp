#include "editor/EditorCamera.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace lm {

namespace {

constexpr float kMaxPitch = XMConvertToRadians(89.9f);
constexpr float kMinDistance = 0.05f;
constexpr float kMaxDistance = 1.0e6f;

const XMVECTORF32 kWorldUp = {{{0.0f, 0.0f, 1.0f, 0.0f}}};

} // namespace

void EditorCamera::Reset()
{
    m_target = {0.0f, 0.0f, 0.0f};
    m_distance = 34.0f;
    m_yaw = XMConvertToRadians(-60.0f);
    m_pitch = XMConvertToRadians(30.0f);
}

XMVECTOR EditorCamera::OffsetDirection() const
{
    const float cp = std::cos(m_pitch), sp = std::sin(m_pitch);
    const float cy = std::cos(m_yaw), sy = std::sin(m_yaw);
    return XMVectorSet(cp * cy, cp * sy, sp, 0.0f);
}

XMVECTOR EditorCamera::Forward() const
{
    return XMVectorNegate(OffsetDirection());
}

XMVECTOR EditorCamera::Right() const
{
    // cross(forward, worldUp), computed from yaw so it stays stable when looking straight down.
    return XMVectorSet(-std::sin(m_yaw), std::cos(m_yaw), 0.0f, 0.0f);
}

XMVECTOR EditorCamera::Up() const
{
    return XMVector3Cross(Right(), Forward());
}

XMFLOAT3 EditorCamera::Eye() const
{
    XMFLOAT3 eye;
    XMStoreFloat3(&eye, XMVectorMultiplyAdd(OffsetDirection(), XMVectorReplicate(m_distance), XMLoadFloat3(&m_target)));
    return eye;
}

void EditorCamera::SetOrientation(float yaw, float pitch)
{
    m_yaw = XMScalarModAngle(yaw);
    m_pitch = std::clamp(pitch, -kMaxPitch, kMaxPitch);
}

void EditorCamera::Orbit(float yawDelta, float pitchDelta)
{
    SetOrientation(m_yaw + yawDelta, m_pitch + pitchDelta);
}

void EditorCamera::Look(float yawDelta, float pitchDelta)
{
    const XMFLOAT3 eyePosition = Eye();
    const XMVECTOR eye = XMLoadFloat3(&eyePosition);
    SetOrientation(m_yaw + yawDelta, m_pitch + pitchDelta);
    XMStoreFloat3(&m_target, XMVectorMultiplyAdd(Forward(), XMVectorReplicate(m_distance), eye));
}

void EditorCamera::Pan(float dxPixels, float dyPixels, float viewportHeightPixels)
{
    if (viewportHeightPixels <= 0.0f)
        return;
    // World size of one pixel at the target's depth: dragging moves the scene with the cursor.
    const float worldPerPixel = 2.0f * m_distance * std::tan(m_fovY * 0.5f) / viewportHeightPixels;
    const XMVECTOR offset = XMVectorAdd(XMVectorScale(Right(), -dxPixels * worldPerPixel),
                                        XMVectorScale(Up(), dyPixels * worldPerPixel));
    XMStoreFloat3(&m_target, XMVectorAdd(XMLoadFloat3(&m_target), offset));
}

void EditorCamera::SetDistance(float distance)
{
    m_distance = std::clamp(distance, kMinDistance, kMaxDistance);
}

void EditorCamera::FrameSphere(float radius)
{
    // Distance at which the sphere fills ~80% of the vertical field of view.
    const float distance = radius / std::sin(m_fovY * 0.5f) * 1.25f;
    m_distance = std::clamp(distance, kMinDistance, kMaxDistance);
}

void EditorCamera::Dolly(float amount)
{
    float distance = m_distance * std::exp(-amount);
    if (distance < kMinDistance) {
        // Can't get closer to the target: push the target forward instead (dolly through it).
        XMStoreFloat3(&m_target, XMVectorMultiplyAdd(Forward(), XMVectorReplicate(kMinDistance - distance),
                                                     XMLoadFloat3(&m_target)));
        distance = kMinDistance;
    }
    m_distance = std::min(distance, kMaxDistance);
}

void EditorCamera::Move(const XMFLOAT3& localDirection, float distance)
{
    XMVECTOR direction = XMVectorScale(Right(), localDirection.x);
    direction = XMVectorMultiplyAdd(kWorldUp, XMVectorReplicate(localDirection.y), direction);
    direction = XMVectorMultiplyAdd(Forward(), XMVectorReplicate(localDirection.z), direction);
    if (XMVectorGetX(XMVector3LengthSq(direction)) < 1e-12f)
        return;
    direction = XMVector3Normalize(direction);
    XMStoreFloat3(&m_target, XMVectorMultiplyAdd(direction, XMVectorReplicate(distance), XMLoadFloat3(&m_target)));
}

Ray EditorCamera::ScreenRay(float ndcX, float ndcY, float aspect) const
{
    const float tanHalf = std::tan(m_fovY * 0.5f);
    XMVECTOR direction = XMVectorMultiplyAdd(Right(), XMVectorReplicate(ndcX * tanHalf * aspect), Forward());
    direction = XMVectorMultiplyAdd(Up(), XMVectorReplicate(ndcY * tanHalf), direction);
    Ray ray;
    ray.origin = Eye();
    XMStoreFloat3(&ray.direction, XMVector3Normalize(direction));
    return ray;
}

XMMATRIX EditorCamera::ViewMatrix() const
{
    const XMFLOAT3 eye = Eye();
    return XMMatrixLookAtRH(XMLoadFloat3(&eye), XMLoadFloat3(&m_target), kWorldUp);
}

XMMATRIX EditorCamera::ProjectionMatrix(float aspect) const
{
    // Row-vector convention: clip = (x*xs, y*ys, near, -z). Depth = near / -z.
    const float yScale = 1.0f / std::tan(m_fovY * 0.5f);
    const float xScale = yScale / std::max(aspect, 1e-4f);
    return XMMATRIX(xScale, 0.0f, 0.0f, 0.0f,
                    0.0f, yScale, 0.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, -1.0f,
                    0.0f, 0.0f, m_nearZ, 0.0f);
}

} // namespace lm
