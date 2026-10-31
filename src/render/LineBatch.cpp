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
    return EnsureCapacity(1024);
}

namespace {

// Corner i has bit 0 = +x, bit 1 = +y, bit 2 = +z.
constexpr uint8_t kBoxEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                      {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

} // namespace

void LineBatch::AddBox(const DirectX::XMFLOAT3& c, const DirectX::XMFLOAT3& h, uint32_t rgba)
{
    DirectX::XMFLOAT3 corners[8];
    for (int i = 0; i < 8; ++i)
        corners[i] = {c.x + ((i & 1) ? h.x : -h.x), c.y + ((i & 2) ? h.y : -h.y), c.z + ((i & 4) ? h.z : -h.z)};
    for (const auto& edge : kBoxEdges)
        AddLine(corners[edge[0]], corners[edge[1]], rgba);
}

namespace {

void BoxCorners(DirectX::FXMMATRIX unitToWorld, DirectX::XMFLOAT3 (&corners)[8])
{
    using namespace DirectX;
    for (int i = 0; i < 8; ++i) {
        const XMVECTOR local = XMVectorSet((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f, 1.0f);
        XMStoreFloat3(&corners[i], XMVector3TransformCoord(local, unitToWorld));
    }
}

} // namespace

uint32_t LineBatch::AddBox(DirectX::FXMMATRIX unitToWorld, uint32_t rgba)
{
    const auto first = static_cast<uint32_t>(m_vertices.size());
    DirectX::XMFLOAT3 corners[8];
    BoxCorners(unitToWorld, corners);
    for (const auto& edge : kBoxEdges)
        AddLine(corners[edge[0]], corners[edge[1]], rgba);
    return first;
}

void LineBatch::SetBox(uint32_t firstVertex, DirectX::FXMMATRIX unitToWorld)
{
    if (size_t(firstVertex) + 24 > m_vertices.size())
        return;
    DirectX::XMFLOAT3 corners[8];
    BoxCorners(unitToWorld, corners);
    Vertex* vertex = m_vertices.data() + firstVertex;
    for (const auto& edge : kBoxEdges) {
        (vertex++)->position = corners[edge[0]];
        (vertex++)->position = corners[edge[1]];
    }
}

bool LineBatch::EnsureCapacity(size_t vertexCount)
{
    if (vertexCount <= m_capacity)
        return true;

    size_t capacity = m_capacity ? m_capacity : 1024;
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

bool LineBatch::Upload(ID3D11DeviceContext* context)
{
    m_uploadedVertices = 0;
    if (m_vertices.empty())
        return true;
    if (!EnsureCapacity(m_vertices.size()))
        return false;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    std::memcpy(mapped.pData, m_vertices.data(), m_vertices.size() * sizeof(Vertex));
    context->Unmap(m_vertexBuffer.Get(), 0);
    m_uploadedVertices = static_cast<uint32_t>(m_vertices.size());
    return true;
}

} // namespace lm
