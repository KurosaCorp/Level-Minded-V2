#include "render/ViewportRenderer.h"

// Bytecode headers generated at build time by fxc (see cmake/Shaders.cmake).
#include "g_GridPS.h"
#include "g_GridVS.h"
#include "g_LinePS.h"
#include "g_LineVS.h"
#include "g_MeshPS.h"
#include "g_MeshVS.h"
#include "g_WirePS.h"

#include <cstring>
#include <iterator>

using namespace DirectX;

namespace lm {

namespace {

constexpr uint32_t kGridFlagLines = 1u;
constexpr uint32_t kGridFlagAxes = 2u;

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

    return CreateMeshResources(device) && m_sceneLines.Create(device);
}

bool ViewportRenderer::CreateMeshResources(ID3D11Device* device)
{
    if (FAILED(device->CreateVertexShader(g_MeshVS, sizeof(g_MeshVS), nullptr, &m_meshVS)) ||
        FAILED(device->CreatePixelShader(g_MeshPS, sizeof(g_MeshPS), nullptr, &m_meshPS)) ||
        FAILED(device->CreatePixelShader(g_WirePS, sizeof(g_WirePS), nullptr, &m_wirePS)) ||
        FAILED(device->CreateInputLayout(kMeshInputLayout, static_cast<UINT>(std::size(kMeshInputLayout)), g_MeshVS,
                                         sizeof(g_MeshVS), &m_meshLayout)))
        return false;

    const MeshGeometry geometry = BuildBuiltinMeshes();
    std::memcpy(m_meshRanges, geometry.ranges, sizeof(m_meshRanges));
    m_wireBoxRange = geometry.wireBox;

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

void ViewportRenderer::BeginScene()
{
    for (auto& bucket : m_instances)
        bucket.clear();
    m_outlines.clear();
    m_sceneLines.Clear();
}

void ViewportRenderer::AddMeshInstance(MeshId mesh, const XMFLOAT3& position, const XMFLOAT3& scale, uint32_t rgba,
                                       uint32_t flags)
{
    const auto bucket = static_cast<uint32_t>(mesh);
    if (bucket == 0 || bucket >= kMeshCount) // MeshId::None or invalid
        return;
    m_instances[bucket].push_back({position, scale, rgba, flags});
}

void ViewportRenderer::AddOutline(const XMFLOAT3& center, const XMFLOAT3& halfExtents, uint32_t rgba)
{
    m_outlines.push_back({center, halfExtents, rgba, 0});
}

void ViewportRenderer::EndScene()
{
    m_sceneDirty = true;
}

bool ViewportRenderer::UploadScene(ID3D11DeviceContext* context)
{
    // Lay the buckets out back to back so each draw reads one contiguous instance range.
    size_t total = 0;
    for (uint32_t mesh = 0; mesh < kMeshCount; ++mesh) {
        m_firstInstance[mesh] = static_cast<UINT>(total);
        m_instanceCount[mesh] = static_cast<UINT>(m_instances[mesh].size());
        total += m_instances[mesh].size();
    }
    m_firstOutline = static_cast<UINT>(total);
    m_outlineCount = static_cast<UINT>(m_outlines.size());
    total += m_outlines.size();

    if (total > m_instanceCapacity) {
        size_t capacity = m_instanceCapacity ? m_instanceCapacity : 1024;
        while (capacity < total)
            capacity *= 2;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(capacity * sizeof(Instance));
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_instanceBuffer.Reset();
        m_instanceCapacity = 0;
        if (FAILED(m_device->CreateBuffer(&desc, nullptr, &m_instanceBuffer)))
            return false;
        m_instanceCapacity = capacity;
    }

    if (total > 0) {
        // Copy each bucket straight into the mapped GPU buffer (no intermediate staging copy).
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(m_instanceBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
        auto* out = static_cast<Instance*>(mapped.pData);
        for (uint32_t mesh = 0; mesh < kMeshCount; ++mesh)
            std::memcpy(out + m_firstInstance[mesh], m_instances[mesh].data(), m_instances[mesh].size() * sizeof(Instance));
        std::memcpy(out + m_firstOutline, m_outlines.data(), m_outlines.size() * sizeof(Instance));
        context->Unmap(m_instanceBuffer.Get(), 0);
        m_bytesUploaded += total * sizeof(Instance);
    }

    if (!m_sceneLines.Upload(context))
        return false;
    m_bytesUploaded += m_sceneLines.UploadedVertexCount() * size_t(LineBatch::kStride);
    return true;
}

void ViewportRenderer::BindMeshInput(ID3D11DeviceContext* context)
{
    m_state.VertexBuffers(context, m_meshVertices.Get(), sizeof(MeshVertex), m_instanceBuffer.Get(), sizeof(Instance));
    m_state.IndexBuffer(context, m_meshIndices.Get());
    m_state.InputLayout(context, m_meshLayout.Get());
    m_state.VertexShader(context, m_meshVS.Get());
}

uint32_t ViewportRenderer::Render(ID3D11DeviceContext* context, const Rect& rect, const ViewportCamera& camera,
                                  const ViewportDrawSettings& settings)
{
    m_state.Reset(); // UI rendering between frames changes state behind our back
    m_bytesUploaded = 0;
    if (rect.IsEmpty())
        return 0;

    if (m_sceneDirty) {
        if (!UploadScene(context))
            return 0;
        m_sceneDirty = false;
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
    m_bytesUploaded += sizeof(constants);

    // Per-frame bindings shared by every pass.
    const D3D11_VIEWPORT viewport{rect.x, rect.y, rect.width, rect.height, 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    ID3D11Buffer* constantBuffers[] = {m_frameConstants.Get()};
    context->VSSetConstantBuffers(0, 1, constantBuffers);
    context->PSSetConstantBuffers(0, 1, constantBuffers);
    context->GSSetShader(nullptr, nullptr, 0);

    uint32_t drawCalls = 0;

    // 1. Opaque meshes: one instanced draw per mesh type.
    bool anyMesh = false;
    for (uint32_t mesh = 1; mesh < kMeshCount; ++mesh)
        anyMesh |= m_instanceCount[mesh] > 0;
    if (anyMesh) {
        BindMeshInput(context);
        m_state.Topology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_state.PixelShader(context, m_meshPS.Get());
        m_state.Rasterizer(context, m_meshRaster.Get());
        m_state.Blend(context, nullptr);
        m_state.DepthStencil(context, m_depthTestWrite.Get());
        for (uint32_t mesh = 1; mesh < kMeshCount; ++mesh) {
            const MeshRange& range = m_meshRanges[mesh];
            if (m_instanceCount[mesh] == 0 || range.indexCount == 0)
                continue;
            context->DrawIndexedInstanced(range.indexCount, m_instanceCount[mesh], range.firstIndex, range.baseVertex,
                                          m_firstInstance[mesh]);
            ++drawCalls;
        }
    }

    // 2. Grid + X/Y axes: one full-viewport triangle, no vertex buffer, depth-tested against the meshes.
    if (constants.gridFlags != 0) {
        m_state.InputLayout(context, nullptr);
        m_state.Topology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_state.VertexShader(context, m_gridVS.Get());
        m_state.PixelShader(context, m_gridPS.Get());
        m_state.Rasterizer(context, m_solidRaster.Get());
        m_state.Blend(context, m_alphaBlend.Get());
        m_state.DepthStencil(context, m_depthTestNoWrite.Get());
        context->Draw(3, 0);
        ++drawCalls;
    }

    // 3. Depth-tested scene lines (Z axis, volumes).
    if (m_sceneLines.UploadedVertexCount() > 0) {
        m_state.VertexBuffers(context, m_sceneLines.VertexBuffer(), LineBatch::kStride, nullptr, 0);
        m_state.InputLayout(context, m_lineLayout.Get());
        m_state.Topology(context, D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        m_state.VertexShader(context, m_lineVS.Get());
        m_state.PixelShader(context, m_linePS.Get());
        m_state.Rasterizer(context, m_lineRaster.Get());
        m_state.Blend(context, m_alphaBlend.Get());
        m_state.DepthStencil(context, m_depthTestNoWrite.Get());
        context->Draw(m_sceneLines.UploadedVertexCount(), 0);
        ++drawCalls;
    }

    // 4. Selection outlines: the wire box, instanced, always on top. 32 bytes per outline instead of
    //    24 line vertices.
    if (m_outlineCount > 0) {
        BindMeshInput(context);
        m_state.Topology(context, D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        m_state.PixelShader(context, m_wirePS.Get());
        m_state.Rasterizer(context, m_lineRaster.Get());
        m_state.Blend(context, m_alphaBlend.Get());
        m_state.DepthStencil(context, m_depthDisabled.Get());
        context->DrawIndexedInstanced(m_wireBoxRange.indexCount, m_outlineCount, m_wireBoxRange.firstIndex,
                                      m_wireBoxRange.baseVertex, m_firstOutline);
        ++drawCalls;
    }
    return drawCalls;
}

} // namespace lm
