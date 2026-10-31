#include "render/ViewportRenderer.h"

// Bytecode headers generated at build time by fxc (see cmake/Shaders.cmake).
#include "g_GridPS.h"
#include "g_GridVS.h"
#include "g_LinePS.h"
#include "g_LineVS.h"
#include "g_MeshPS.h"
#include "g_MeshVS.h"

#include <cstring>
#include <iterator>

using namespace DirectX;

namespace lm {

namespace {

constexpr uint32_t kGridFlagLines = 1u;
constexpr uint32_t kGridFlagAxes = 2u;

constexpr uint32_t kAxisZColor = PackRgba(66, 133, 244);
constexpr uint32_t kAxisZBelowColor = PackRgba(66, 133, 244, 80);

// Mirrors cbuffer FrameConstants in shaders/Common.hlsli.
struct alignas(16) FrameConstants {
    XMFLOAT4X4 viewProj;
    XMFLOAT4X4 invViewProj;
    XMFLOAT3 cameraPos;
    float gridFadeDistance;
    XMFLOAT2 viewportOrigin;
    XMFLOAT2 viewportSize;
    uint32_t gridFlags;
    float padding[3];
};
static_assert(sizeof(FrameConstants) % 16 == 0);

const D3D11_INPUT_ELEMENT_DESC kMeshInputLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"INSTPOS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    {"INSTSCALE", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 12, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    {"INSTCOLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 24, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    {"INSTFLAGS", 0, DXGI_FORMAT_R32_UINT, 1, 28, D3D11_INPUT_PER_INSTANCE_DATA, 1},
};

} // namespace

bool ViewportRenderer::Create(ID3D11Device* device)
{
    m_device = device;

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(FrameConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &m_frameConstants)))
        return false;

    if (FAILED(device->CreateVertexShader(g_GridVS, sizeof(g_GridVS), nullptr, &m_gridVS)) ||
        FAILED(device->CreatePixelShader(g_GridPS, sizeof(g_GridPS), nullptr, &m_gridPS)) ||
        FAILED(device->CreateVertexShader(g_LineVS, sizeof(g_LineVS), nullptr, &m_lineVS)) ||
        FAILED(device->CreatePixelShader(g_LinePS, sizeof(g_LinePS), nullptr, &m_linePS)) ||
        FAILED(device->CreateInputLayout(LineBatch::kInputLayout, static_cast<UINT>(std::size(LineBatch::kInputLayout)),
                                         g_LineVS, sizeof(g_LineVS), &m_lineLayout)))
        return false;

    D3D11_BLEND_DESC blendDesc{};
    auto& rt = blendDesc.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blendDesc, &m_alphaBlend)))
        return false;

    // Reversed-Z: nearer surfaces have larger depth values.
    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = TRUE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthDesc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    if (FAILED(device->CreateDepthStencilState(&depthDesc, &m_depthTestWrite)))
        return false;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(device->CreateDepthStencilState(&depthDesc, &m_depthTestNoWrite)))
        return false;
    depthDesc.DepthEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&depthDesc, &m_depthDisabled)))
        return false;

    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterDesc, &m_solidRaster)))
        return false;
    rasterDesc.AntialiasedLineEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterDesc, &m_lineRaster)))
        return false;
    // Meshes are counter-clockwise seen from outside (see MeshGeometry.h).
    rasterDesc.AntialiasedLineEnable = FALSE;
    rasterDesc.CullMode = D3D11_CULL_BACK;
    rasterDesc.FrontCounterClockwise = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterDesc, &m_meshRaster)))
        return false;

    return CreateMeshResources(device) && m_lines.Create(device) && m_overlayLines.Create(device);
}

bool ViewportRenderer::CreateMeshResources(ID3D11Device* device)
{
    if (FAILED(device->CreateVertexShader(g_MeshVS, sizeof(g_MeshVS), nullptr, &m_meshVS)) ||
        FAILED(device->CreatePixelShader(g_MeshPS, sizeof(g_MeshPS), nullptr, &m_meshPS)) ||
        FAILED(device->CreateInputLayout(kMeshInputLayout, static_cast<UINT>(std::size(kMeshInputLayout)), g_MeshVS,
                                         sizeof(g_MeshVS), &m_meshLayout)))
        return false;

    const MeshGeometry geometry = BuildBuiltinMeshes();
    std::memcpy(m_meshRanges, geometry.ranges, sizeof(m_meshRanges));

    D3D11_BUFFER_DESC desc{};
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.ByteWidth = static_cast<UINT>(geometry.vertices.size() * sizeof(MeshVertex));
    D3D11_SUBRESOURCE_DATA data{geometry.vertices.data(), 0, 0};
    if (FAILED(device->CreateBuffer(&desc, &data, &m_meshVertices)))
        return false;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    desc.ByteWidth = static_cast<UINT>(geometry.indices.size() * sizeof(uint16_t));
    data.pSysMem = geometry.indices.data();
    return SUCCEEDED(device->CreateBuffer(&desc, &data, &m_meshIndices));
}

void ViewportRenderer::AddMeshInstance(MeshId mesh, const XMFLOAT3& position, const XMFLOAT3& scale, uint32_t rgba,
                                       uint32_t flags)
{
    const auto bucket = static_cast<uint32_t>(mesh);
    if (bucket == 0 || bucket >= kMeshCount) // MeshId::None or invalid
        return;
    m_instances[bucket].push_back({position, scale, rgba, flags});
}

uint32_t ViewportRenderer::DrawMeshes(ID3D11DeviceContext* context)
{
    // Concatenate the per-mesh buckets so every mesh type reads a contiguous instance range.
    m_instanceUpload.clear();
    UINT firstInstance[kMeshCount] = {};
    for (uint32_t mesh = 0; mesh < kMeshCount; ++mesh) {
        firstInstance[mesh] = static_cast<UINT>(m_instanceUpload.size());
        m_instanceUpload.insert(m_instanceUpload.end(), m_instances[mesh].begin(), m_instances[mesh].end());
    }
    if (m_instanceUpload.empty())
        return 0;

    if (m_instanceUpload.size() > m_instanceCapacity) {
        size_t capacity = m_instanceCapacity ? m_instanceCapacity : 1024;
        while (capacity < m_instanceUpload.size())
            capacity *= 2;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(capacity * sizeof(MeshInstance));
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_instanceBuffer.Reset();
        if (FAILED(m_device->CreateBuffer(&desc, nullptr, &m_instanceBuffer)))
            return 0;
        m_instanceCapacity = capacity;
    }

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(m_instanceBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return 0;
    std::memcpy(mapped.pData, m_instanceUpload.data(), m_instanceUpload.size() * sizeof(MeshInstance));
    context->Unmap(m_instanceBuffer.Get(), 0);

    ID3D11Buffer* buffers[] = {m_meshVertices.Get(), m_instanceBuffer.Get()};
    const UINT strides[] = {sizeof(MeshVertex), sizeof(MeshInstance)};
    const UINT offsets[] = {0, 0};
    context->IASetVertexBuffers(0, 2, buffers, strides, offsets);
    context->IASetIndexBuffer(m_meshIndices.Get(), DXGI_FORMAT_R16_UINT, 0);
    context->IASetInputLayout(m_meshLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_meshVS.Get(), nullptr, 0);
    context->PSSetShader(m_meshPS.Get(), nullptr, 0);
    context->RSSetState(m_meshRaster.Get());
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(m_depthTestWrite.Get(), 0);

    uint32_t drawCalls = 0;
    for (uint32_t mesh = 0; mesh < kMeshCount; ++mesh) {
        const auto count = static_cast<UINT>(m_instances[mesh].size());
        const MeshRange& range = m_meshRanges[mesh];
        if (count == 0 || range.indexCount == 0)
            continue;
        context->DrawIndexedInstanced(range.indexCount, count, range.firstIndex, range.baseVertex, firstInstance[mesh]);
        ++drawCalls;
    }
    for (auto& bucket : m_instances)
        bucket.clear();
    return drawCalls;
}

uint32_t ViewportRenderer::DrawLines(ID3D11DeviceContext* context, LineBatch& lines, ID3D11DepthStencilState* depthState)
{
    if (lines.LineCount() == 0)
        return 0;
    context->IASetInputLayout(m_lineLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    context->VSSetShader(m_lineVS.Get(), nullptr, 0);
    context->PSSetShader(m_linePS.Get(), nullptr, 0);
    context->RSSetState(m_lineRaster.Get());
    context->OMSetBlendState(m_alphaBlend.Get(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(depthState, 0);
    return lines.Flush(context);
}

uint32_t ViewportRenderer::Render(ID3D11DeviceContext* context, const Rect& rect, const ViewportCamera& camera,
                                  const ViewportDrawSettings& settings)
{
    if (rect.IsEmpty()) {
        for (auto& bucket : m_instances)
            bucket.clear();
        m_lines.Clear();
        m_overlayLines.Clear();
        return 0;
    }

    const XMMATRIX viewProj = XMMatrixMultiply(XMLoadFloat4x4(&camera.view), XMLoadFloat4x4(&camera.projection));

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(m_frameConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return 0;
    FrameConstants constants{};
    XMStoreFloat4x4(&constants.viewProj, viewProj);
    XMStoreFloat4x4(&constants.invViewProj, XMMatrixInverse(nullptr, viewProj));
    constants.cameraPos = camera.eye;
    constants.gridFadeDistance = camera.gridFadeDistance;
    constants.viewportOrigin = {rect.x, rect.y};
    constants.viewportSize = {rect.width, rect.height};
    constants.gridFlags = (settings.showGrid ? kGridFlagLines : 0u) | (settings.showAxes ? kGridFlagAxes : 0u);
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(m_frameConstants.Get(), 0);

    const D3D11_VIEWPORT viewport{rect.x, rect.y, rect.width, rect.height, 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    ID3D11Buffer* constantBuffers[] = {m_frameConstants.Get()};
    context->VSSetConstantBuffers(0, 1, constantBuffers);
    context->PSSetConstantBuffers(0, 1, constantBuffers);
    context->GSSetShader(nullptr, nullptr, 0);

    // 1. Opaque meshes.
    uint32_t drawCalls = DrawMeshes(context);

    // 2. Grid + X/Y axes: one full-viewport triangle, no vertex buffer, depth-tested against the meshes.
    if (constants.gridFlags != 0) {
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(m_gridVS.Get(), nullptr, 0);
        context->PSSetShader(m_gridPS.Get(), nullptr, 0);
        context->RSSetState(m_solidRaster.Get());
        context->OMSetBlendState(m_alphaBlend.Get(), nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(m_depthTestNoWrite.Get(), 0);
        context->Draw(3, 0);
        ++drawCalls;
    }

    // 3. Depth-tested lines (Z axis, volumes), 4. overlay lines (selection outlines).
    if (settings.showAxes) {
        const float length = camera.gridFadeDistance;
        m_lines.AddLine({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, length}, kAxisZColor);
        m_lines.AddLine({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -length}, kAxisZBelowColor);
    }
    drawCalls += DrawLines(context, m_lines, m_depthTestNoWrite.Get());
    drawCalls += DrawLines(context, m_overlayLines, m_depthDisabled.Get());
    return drawCalls;
}

} // namespace lm
