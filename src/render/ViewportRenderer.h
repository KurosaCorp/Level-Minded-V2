#pragma once

#include "core/MeshId.h"
#include "core/Rect.h"
#include "render/LineBatch.h"
#include "render/MeshGeometry.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <vector>

namespace lm {

struct ViewportDrawSettings {
    bool showGrid = true;
    bool showAxes = true;
};

// What the renderer needs from a camera for one frame.
struct ViewportCamera {
    DirectX::XMFLOAT4X4 view;
    DirectX::XMFLOAT4X4 projection; // reversed-Z, infinite far plane
    DirectX::XMFLOAT3 eye;
    float gridFadeDistance;
};

// Flags for AddMeshInstance.
inline constexpr uint32_t kMeshFlagSelected = 1u;
inline constexpr uint32_t kMeshFlagEmissive = 2u;

// Draws the 3D scene of one viewport, in this order:
//   1. solid meshes   - instanced, one draw call per mesh type in use (writes depth)
//   2. ground grid    - one full-viewport triangle (depth-tested against the meshes)
//   3. lines          - one draw call, depth-tested (axes, volumes)
//   4. overlay lines  - one draw call, always on top (selection outlines)
class ViewportRenderer {
public:
    bool Create(ID3D11Device* device);

    // Queues one mesh instance for the next Render(): built-in `mesh` scaled by `scale` (the mesh is
    // modeled in [-1, 1]^3, so scale = half extents) and placed at `position`.
    void AddMeshInstance(MeshId mesh, const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& scale, uint32_t rgba,
                         uint32_t flags = 0);
    // Lines queued here are drawn by the next Render() call.
    LineBatch& Lines() { return m_lines; }
    LineBatch& OverlayLines() { return m_overlayLines; }

    // Renders into the currently bound render target + depth buffer, restricted to `rect`, then clears
    // all queued instances and lines. Returns the number of draw calls issued.
    uint32_t Render(ID3D11DeviceContext* context, const Rect& rect, const ViewportCamera& camera,
                    const ViewportDrawSettings& settings);

private:
    struct MeshInstance {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 scale;
        uint32_t color;
        uint32_t flags;
    };
    static_assert(sizeof(MeshInstance) == 32);

    bool CreateMeshResources(ID3D11Device* device);
    uint32_t DrawMeshes(ID3D11DeviceContext* context);
    uint32_t DrawLines(ID3D11DeviceContext* context, LineBatch& lines, ID3D11DepthStencilState* depthState);

    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11Buffer> m_frameConstants;
    ComPtr<ID3D11VertexShader> m_gridVS;
    ComPtr<ID3D11PixelShader> m_gridPS;
    ComPtr<ID3D11VertexShader> m_lineVS;
    ComPtr<ID3D11PixelShader> m_linePS;
    ComPtr<ID3D11InputLayout> m_lineLayout;
    ComPtr<ID3D11VertexShader> m_meshVS;
    ComPtr<ID3D11PixelShader> m_meshPS;
    ComPtr<ID3D11InputLayout> m_meshLayout;

    ComPtr<ID3D11BlendState> m_alphaBlend;
    ComPtr<ID3D11DepthStencilState> m_depthTestWrite;   // opaque meshes
    ComPtr<ID3D11DepthStencilState> m_depthTestNoWrite; // grid, lines
    ComPtr<ID3D11DepthStencilState> m_depthDisabled;    // overlay lines
    ComPtr<ID3D11RasterizerState> m_solidRaster;        // no culling (grid)
    ComPtr<ID3D11RasterizerState> m_meshRaster;         // back-face culling
    ComPtr<ID3D11RasterizerState> m_lineRaster;

    // Built-in meshes: one shared vertex + index buffer.
    ComPtr<ID3D11Buffer> m_meshVertices;
    ComPtr<ID3D11Buffer> m_meshIndices;
    MeshRange m_meshRanges[kMeshCount];

    // Instances are bucketed per mesh on the CPU, then uploaded in one Map per frame.
    std::vector<MeshInstance> m_instances[kMeshCount];
    std::vector<MeshInstance> m_instanceUpload;
    ComPtr<ID3D11Buffer> m_instanceBuffer;
    size_t m_instanceCapacity = 0;

    LineBatch m_lines;
    LineBatch m_overlayLines;
};

} // namespace lm
