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

// Colored line segments collected on the CPU and drawn with a single draw call. Upload() copies
// them to a GPU buffer; the uploaded lines can then be drawn every frame without re-uploading.
class LineBatch {
public:
    bool Create(ID3D11Device* device);

    void Clear() { m_vertices.clear(); }
    void AddLine(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, uint32_t rgba)
    {
        m_vertices.push_back({a, rgba});
        m_vertices.push_back({b, rgba});
    }
    // Wireframe axis-aligned box (12 lines).
    void AddBox(const DirectX::XMFLOAT3& center, const DirectX::XMFLOAT3& halfExtents, uint32_t rgba);
    // Wireframe box: the unit box [-1, 1]^3 transformed by `unitToWorld` (12 lines). Returns the index
    // of its first vertex, for SetBox().
    uint32_t AddBox(DirectX::FXMMATRIX unitToWorld, uint32_t rgba);
    // Moves a box added by AddBox (keeps its color). Upload() again to see it.
    void SetBox(uint32_t firstVertex, DirectX::FXMMATRIX unitToWorld);
    size_t LineCount() const { return m_vertices.size() / 2; }

    // Copies the queued lines to the GPU (grows the buffer if needed). Call when the lines changed.
    bool Upload(ID3D11DeviceContext* context);
    ID3D11Buffer* VertexBuffer() const { return m_vertexBuffer.Get(); }
    uint32_t UploadedVertexCount() const { return m_uploadedVertices; }

    static constexpr UINT kStride = 16;
    static const D3D11_INPUT_ELEMENT_DESC kInputLayout[2];

private:
    struct Vertex {
        DirectX::XMFLOAT3 position;
        uint32_t color;
    };
    static_assert(sizeof(Vertex) == kStride);

    bool EnsureCapacity(size_t vertexCount);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_vertexBuffer;
    size_t m_capacity = 0; // vertices
    uint32_t m_uploadedVertices = 0;
    std::vector<Vertex> m_vertices;
};

} // namespace lm
