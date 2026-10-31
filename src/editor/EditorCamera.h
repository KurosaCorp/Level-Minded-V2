#pragma once

#include "scene/Picking.h"

#include <DirectXMath.h>

namespace lm {

// Orbit-style editor camera in a right-handed, Z-up world.
//
// The camera looks at `target` from `distance` away. Yaw rotates around +Z (0 = camera on the +X
// side of the target), pitch is the elevation above the XY plane. Orbit/pan/dolly move the camera
// around the target; Look/Move (fly mode) move the target along with the eye.
class EditorCamera {
public:
    EditorCamera() { Reset(); }

    void Reset();
    void Orbit(float yawDelta, float pitchDelta);           // radians, around the target
    void Look(float yawDelta, float pitchDelta);            // radians, around the eye
    void Pan(float dxPixels, float dyPixels, float viewportHeightPixels); // keeps the target plane under the cursor
    void Dolly(float amount);                               // log scale, > 0 moves closer
    void Move(const DirectX::XMFLOAT3& localDirection, float distance); // x right, y world-up, z forward
    void SetOrientation(float yaw, float pitch);
    void SetTarget(const DirectX::XMFLOAT3& target) { m_target = target; }
    // Moves the camera back along its view direction until a sphere of `radius` around the target fits.
    void FrameSphere(float radius);

    DirectX::XMFLOAT3 Eye() const;
    const DirectX::XMFLOAT3& Target() const { return m_target; }
    float Distance() const { return m_distance; }
    float Yaw() const { return m_yaw; }
    float Pitch() const { return m_pitch; }
    float FovY() const { return m_fovY; }
    float NearZ() const { return m_nearZ; }
    DirectX::XMVECTOR ForwardVector() const { return Forward(); }

    // World-space ray from the eye through a point of the image. ndcX/ndcY in [-1, 1], +y up.
    Ray ScreenRay(float ndcX, float ndcY, float aspect) const;

    DirectX::XMMATRIX ViewMatrix() const;
    // Reversed-Z perspective with an infinite far plane (depth 1 at the near plane, 0 at infinity).
    DirectX::XMMATRIX ProjectionMatrix(float aspect) const;

private:
    DirectX::XMVECTOR OffsetDirection() const; // unit vector from target to eye
    DirectX::XMVECTOR Forward() const;
    DirectX::XMVECTOR Right() const;
    DirectX::XMVECTOR Up() const;

    DirectX::XMFLOAT3 m_target{};
    float m_distance = 0.0f;
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;
    float m_fovY = DirectX::XMConvertToRadians(55.0f);
    float m_nearZ = 0.05f;
};

} // namespace lm
