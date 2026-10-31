#include "render/LineBatch.h"

#include <cstring>

namespace lm {

const D3D11_INPUT_ELEMENT_DESC LineBatch::kInputLayout[2] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
};

bool LineBatch::Create(ID3D11Device* device)
{
    m_device = device;
    m_vertices.reserve(4096);
    return EnsureCapacity(4096);
}

bool LineBatch::EnsureCapacity(size_t vertexCount)
{
    if (vertexCount <= m_capacity)
        return true;

    size_t capacity = m_capacity ? m_capacity : 4096;
    while (capacity < vertexCount)
        capacity *= 2;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(capacity * sizeof(Vertex));
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
    if (FAILED(m_device->CreateBuffer(&desc, nullptr, &buffer)))
        return false;
    m_vertexBuffer = buffer;
    m_capacity = capacity;
    return true;
}

uint32_t LineBatch::Flush(ID3D11DeviceContext* context)
{
    if (m_vertices.empty())
        return 0;
    if (!EnsureCapacity(m_vertices.size())) {
        m_vertices.clear();
        return 0;
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        m_vertices.clear();
        return 0;
    }
    std::memcpy(mapped.pData, m_vertices.data(), m_vertices.size() * sizeof(Vertex));
    context->Unmap(m_vertexBuffer.Get(), 0);

    const UINT stride = sizeof(Vertex);
    const UINT offset = 0;
    ID3D11Buffer* buffers[] = {m_vertexBuffer.Get()};
    context->IASetVertexBuffers(0, 1, buffers, &stride, &offset);
    context->Draw(static_cast<UINT>(m_vertices.size()), 0);

    m_vertices.clear();
    return 1;
}

} // namespace lm
