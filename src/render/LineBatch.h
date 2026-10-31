#pragma once

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace lm {

// Packs a color for LineBatch (R8G8B8A8_UNORM, R in the low byte).
constexpr uint32_t PackRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
}

// Collects colored line segments during a frame and draws them all with a single draw call.
class LineBatch {
public:
    bool Create(ID3D11Device* device);

    void AddLine(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, uint32_t rgba)
    {
        m_vertices.push_back({a, rgba});
        m_vertices.push_back({b, rgba});
    }
    size_t LineCount() const { return m_vertices.size() / 2; }

    // Uploads and draws all queued lines, then clears the batch. The caller binds shaders, input
    // layout, states and constants. Returns the number of draw calls issued (0 or 1).
    uint32_t Flush(ID3D11DeviceContext* context);

    static const D3D11_INPUT_ELEMENT_DESC kInputLayout[2];

private:
    struct Vertex {
        DirectX::XMFLOAT3 position;
        uint32_t color;
    };

    bool EnsureCapacity(size_t vertexCount);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vertexBuffer;
    size_t m_capacity = 0; // vertices
    std::vector<Vertex> m_vertices;
};

} // namespace lm
