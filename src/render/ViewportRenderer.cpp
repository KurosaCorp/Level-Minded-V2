#include "render/ViewportRenderer.h"

// Bytecode headers generated at build time by fxc (see cmake/Shaders.cmake).
#include "g_GridPS.h"
#include "g_GridVS.h"
#include "g_LinePS.h"
#include "g_LineVS.h"

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

} // namespace

bool ViewportRenderer::Create(ID3D11Device* device)
{
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
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthDesc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    if (FAILED(device->CreateDepthStencilState(&depthDesc, &m_depthTestNoWrite)))
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

    return m_lines.Create(device);
}

uint32_t ViewportRenderer::Render(ID3D11DeviceContext* context, const Rect& rect, const ViewportCamera& camera,
                                  const ViewportDrawSettings& settings)
{
    if (rect.IsEmpty())
        return 0;

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
    context->OMSetBlendState(m_alphaBlend.Get(), nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(m_depthTestNoWrite.Get(), 0);
    context->GSSetShader(nullptr, nullptr, 0);

    uint32_t drawCalls = 0;

    // Grid + X/Y axes: one full-viewport triangle, no vertex buffer.
    if (constants.gridFlags != 0) {
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(m_gridVS.Get(), nullptr, 0);
        context->PSSetShader(m_gridPS.Get(), nullptr, 0);
        context->RSSetState(m_solidRaster.Get());
        context->Draw(3, 0);
        ++drawCalls;
    }

    // Z axis (vertical) goes through the line batch with everything else line-based.
    if (settings.showAxes) {
        const float length = camera.gridFadeDistance;
        m_lines.AddLine({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, length}, kAxisZColor);
        m_lines.AddLine({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -length}, kAxisZBelowColor);
    }

    if (m_lines.LineCount() > 0) {
        context->IASetInputLayout(m_lineLayout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        context->VSSetShader(m_lineVS.Get(), nullptr, 0);
        context->PSSetShader(m_linePS.Get(), nullptr, 0);
        context->RSSetState(m_lineRaster.Get());
        drawCalls += m_lines.Flush(context);
    }

    return drawCalls;
}

} // namespace lm
