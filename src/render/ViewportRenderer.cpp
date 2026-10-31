#include "render/ViewportRenderer.h"

// Bytecode headers generated at build time by fxc (see cmake/Shaders.cmake).
#include "g_GridPS.h"
#include "g_GridVS.h"
#include "g_LinePS.h"
#include "g_LineVS.h"
#include "g_MeshPS.h"
#include "g_MeshVS.h"
#include "g_WirePS.h"

#include <algorithm>
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
    {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    {"INSTPOS", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    {"INSTROT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 12, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    {"INSTSCALE", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 28, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    {"INSTCOLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 40, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    {"INSTFLAGS", 0, DXGI_FORMAT_R32_UINT, 1, 44, D3D11_INPUT_PER_INSTANCE_DATA, 1},
};

} // namespace

bool ViewportRenderer::Create(ID3D11Device* device, const MeshLibrary* meshes)
{
    m_device = device;
    m_meshes = meshes;
    if (!m_meshes)
        return false;

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
    return true; // the mesh buffers are created by the first SyncMeshes()
}

namespace {

// Next power of two >= value (at least `minimum`).
size_t GrowCapacity(size_t current, size_t needed, size_t minimum)
{
    size_t capacity = current ? current : minimum;
    while (capacity < needed)
        capacity *= 2;
    return capacity;
}

} // namespace

bool ViewportRenderer::SyncMeshes(ID3D11DeviceContext* context)
{
    if (m_meshes->Version() == m_meshVersion)
        return true;
    const std::span<const MeshVertex> vertices = m_meshes->Vertices();
    const std::span<const uint32_t> indices = m_meshes->Indices();

    // DEFAULT buffers that grow by doubling. Appended meshes upload only their own range; a buffer is
    // recreated (and fully re-uploaded) only when it runs out of room.
    const auto sync = [&](ComPtr<ID3D11Buffer>& buffer, size_t& capacity, size_t& uploaded, const void* data, size_t count,
                          size_t stride, UINT bind, size_t minimum) {
        if (count > capacity) {
            const size_t newCapacity = GrowCapacity(capacity, count, minimum);
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = static_cast<UINT>(newCapacity * stride);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = bind;
            ComPtr<ID3D11Buffer> grown;
            if (FAILED(m_device->CreateBuffer(&desc, nullptr, &grown)))
                return false;
            buffer = grown;
            capacity = newCapacity;
            uploaded = 0;
        }
        if (count > uploaded) {
            const D3D11_BOX box{static_cast<UINT>(uploaded * stride), 0, 0, static_cast<UINT>(count * stride), 1, 1};
            context->UpdateSubresource(buffer.Get(), 0, &box, static_cast<const uint8_t*>(data) + uploaded * stride, 0, 0);
            m_bytesUploaded += (count - uploaded) * stride;
            uploaded = count;
        }
        return true;
    };
    if (!sync(m_meshVertices, m_vertexCapacity, m_uploadedVertices, vertices.data(), vertices.size(), sizeof(MeshVertex),
              D3D11_BIND_VERTEX_BUFFER, 16 * 1024) ||
        !sync(m_meshIndices, m_indexCapacity, m_uploadedIndices, indices.data(), indices.size(), sizeof(uint32_t),
              D3D11_BIND_INDEX_BUFFER, 32 * 1024))
        return false;
    m_meshVersion = m_meshes->Version();
    return true;
}

namespace {

// InstanceHandle = (bucket << 20) | index: up to 4096 mesh buckets (MeshLibrary::kMaxMeshes) with
// up to ~1M instances each.
constexpr uint32_t kHandleIndexBits = 20;
constexpr uint32_t kHandleIndexMask = (1u << kHandleIndexBits) - 1;
static_assert(MeshLibrary::kMaxMeshes <= (1u << (32 - kHandleIndexBits)));

} // namespace

void ViewportRenderer::BeginScene()
{
    const uint32_t buckets = m_meshes->Count();
    if (m_instances.size() < buckets) { // a model was imported since the last scene
        m_instances.resize(buckets);
        m_dirtyBegin.resize(buckets, 0);
        m_dirtyEnd.resize(buckets, 0);
        m_firstInstance.resize(buckets, 0);
        m_instanceCount.resize(buckets, 0);
    }
    for (auto& bucket : m_instances)
        bucket.clear();
    m_outlines.clear();
    m_sceneLines.Clear();
}

InstanceHandle ViewportRenderer::AddMeshInstance(MeshId mesh, const XMFLOAT3& position, const XMFLOAT4& rotation,
                                                 const XMFLOAT3& size, uint32_t rgba, uint32_t flags)
{
    const auto bucket = static_cast<uint32_t>(mesh);
    if (bucket == 0 || bucket >= m_instances.size() || m_instances[bucket].size() >= kHandleIndexMask) // None or unknown (index never all ones: no clash with kNoInstance)
        return kNoInstance;
    m_instances[bucket].push_back({position, rotation, size, rgba, flags});
    return (bucket << kHandleIndexBits) | static_cast<uint32_t>(m_instances[bucket].size() - 1);
}

InstanceHandle ViewportRenderer::AddOutline(const XMFLOAT3& position, const XMFLOAT4& rotation, const XMFLOAT3& size,
                                            uint32_t rgba)
{
    if (m_outlines.size() >= kHandleIndexMask)
        return kNoInstance;
    m_outlines.push_back({position, rotation, size, rgba, 0});
    return static_cast<uint32_t>(m_outlines.size() - 1); // bucket 0
}

void ViewportRenderer::UpdateInstance(InstanceHandle handle, const XMFLOAT3& position, const XMFLOAT4& rotation,
                                      const XMFLOAT3& size)
{
    const uint32_t bucket = handle >> kHandleIndexBits;
    const uint32_t index = handle & kHandleIndexMask;
    if (handle == kNoInstance || bucket >= m_instances.size() || index >= Bucket(bucket).size())
        return;
    Instance& instance = Bucket(bucket)[index];
    instance.position = position;
    instance.rotation = rotation;
    instance.scale = size;
    if (m_dirtyEnd[bucket] == 0) {
        m_dirtyBegin[bucket] = index;
        m_dirtyEnd[bucket] = index + 1;
    } else {
        m_dirtyBegin[bucket] = std::min(m_dirtyBegin[bucket], index);
        m_dirtyEnd[bucket] = std::max(m_dirtyEnd[bucket], index + 1);
    }
}

void ViewportRenderer::EndScene()
{
    m_sceneDirty = true;
}

void ViewportRenderer::UploadInstanceRange(ID3D11DeviceContext* context, const Instance* data, UINT first, UINT count)
{
    if (count == 0)
        return;
    const D3D11_BOX box{first * UINT(sizeof(Instance)), 0, 0, (first + count) * UINT(sizeof(Instance)), 1, 1};
    context->UpdateSubresource(m_instanceBuffer.Get(), 0, &box, data, 0, 0);
    m_bytesUploaded += size_t(count) * sizeof(Instance);
}

bool ViewportRenderer::UploadScene(ID3D11DeviceContext* context)
{
    // Lay the buckets out back to back so each draw reads one contiguous instance range.
    size_t total = 0;
    for (size_t mesh = 0; mesh < m_instances.size(); ++mesh) {
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
        // DEFAULT usage + UpdateSubresource: lets a moved item upload just its own 48 bytes (a DYNAMIC
        // buffer can only be rewritten whole with WRITE_DISCARD while the GPU may still read it).
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = static_cast<UINT>(capacity * sizeof(Instance));
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        m_instanceBuffer.Reset();
        m_instanceCapacity = 0;
        if (FAILED(m_device->CreateBuffer(&desc, nullptr, &m_instanceBuffer)))
            return false;
        m_instanceCapacity = capacity;
    }

    for (size_t mesh = 1; mesh < m_instances.size(); ++mesh)
        UploadInstanceRange(context, m_instances[mesh].data(), m_firstInstance[mesh], m_instanceCount[mesh]);
    UploadInstanceRange(context, m_outlines.data(), m_firstOutline, m_outlineCount);
    for (uint32_t& end : m_dirtyEnd)
        end = 0; // everything was just uploaded

    if (!m_sceneLines.Upload(context))
        return false;
    m_bytesUploaded += m_sceneLines.UploadedVertexCount() * size_t(LineBatch::kStride);
    return true;
}

void ViewportRenderer::BindMeshInput(ID3D11DeviceContext* context)
{
    m_state.VertexBuffers(context, m_meshVertices.Get(), sizeof(MeshVertex), m_instanceBuffer.Get(), sizeof(Instance));
    // 32-bit indices: imported models can have more than 65,536 vertices.
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
    if (!SyncMeshes(context))
        return 0;

    if (m_sceneDirty) {
        if (!UploadScene(context))
            return 0;
        m_sceneDirty = false;
        m_linesDirty = false;
    } else {
        // Moved items: upload only the changed range of each bucket.
        for (uint32_t bucket = 0; bucket < m_dirtyEnd.size(); ++bucket) {
            if (m_dirtyEnd[bucket] == 0)
                continue;
            const UINT first = bucket == 0 ? m_firstOutline : m_firstInstance[bucket];
            UploadInstanceRange(context, Bucket(bucket).data() + m_dirtyBegin[bucket], first + m_dirtyBegin[bucket],
                                m_dirtyEnd[bucket] - m_dirtyBegin[bucket]);
            m_dirtyEnd[bucket] = 0;
        }
        if (m_linesDirty) {
            if (!m_sceneLines.Upload(context))
                return 0;
            m_bytesUploaded += m_sceneLines.UploadedVertexCount() * size_t(LineBatch::kStride);
            m_linesDirty = false;
        }
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

    // 1. Opaque meshes: one instanced draw per mesh type in use. Double-sided meshes (imported
    //    models whose materials ask for it) are drawn without back-face culling.
    const uint32_t buckets = static_cast<uint32_t>(m_instanceCount.size());
    bool anyMesh = false;
    for (uint32_t mesh = 1; mesh < buckets; ++mesh)
        anyMesh |= m_instanceCount[mesh] > 0;
    if (anyMesh) {
        BindMeshInput(context);
        m_state.Topology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_state.PixelShader(context, m_meshPS.Get());
        m_state.Blend(context, nullptr);
        m_state.DepthStencil(context, m_depthTestWrite.Get());
        for (uint32_t mesh = 1; mesh < buckets; ++mesh) {
            const MeshLibrary::Mesh& info = m_meshes->Get(static_cast<MeshId>(mesh));
            const MeshRange& range = info.range;
            if (m_instanceCount[mesh] == 0 || range.indexCount == 0)
                continue;
            m_state.Rasterizer(context, (info.traits & kMeshTraitDoubleSided) ? m_solidRaster.Get() : m_meshRaster.Get());
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

    // 4. Selection outlines: the wire box, instanced, always on top. 48 bytes per outline instead of
    //    24 line vertices.
    if (m_outlineCount > 0) {
        const MeshRange& wireBox = m_meshes->WireBox();
        BindMeshInput(context);
        m_state.Topology(context, D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        m_state.PixelShader(context, m_wirePS.Get());
        m_state.Rasterizer(context, m_lineRaster.Get());
        m_state.Blend(context, m_alphaBlend.Get());
        m_state.DepthStencil(context, m_depthDisabled.Get());
        context->DrawIndexedInstanced(wireBox.indexCount, m_outlineCount, wireBox.firstIndex, wireBox.baseVertex,
                                      m_firstOutline);
        ++drawCalls;
    }
    return drawCalls;
}

} // namespace lm
