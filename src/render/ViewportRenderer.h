#pragma once

#include "core/MeshId.h"
#include "core/Rect.h"
#include "render/LineBatch.h"
#include "render/MeshGeometry.h"
#include "render/StateCache.h"

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

// Draws the 3D scene of one viewport.
//
// Scene content is retained: it is queued between BeginScene() and EndScene() only when the scene
// changed, uploaded to the GPU once, and redrawn from GPU buffers every frame after that. A frame
// where only the camera moves uploads nothing but one small constant buffer.
//
// Draw order (one draw call each, skipped when empty):
//   1. solid meshes   - instanced, one draw per mesh type in use (writes depth)
//   2. ground grid    - one full-viewport triangle (depth-tested against the meshes)
//   3. scene lines    - depth-tested (axes, volumes)
//   4. outlines       - instanced wire boxes, always on top (selection)
class ViewportRenderer {
public:
    bool Create(ID3D11Device* device);

    // ---- Scene layer (retained until the next BeginScene)
    void BeginScene();
    // Built-in `mesh` (modeled in [-1, 1]^3) scaled by `scale` (half extents) and placed at `position`.
    void AddMeshInstance(MeshId mesh, const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& scale, uint32_t rgba,
                         uint32_t flags = 0);
    // Always-on-top wireframe box.
    void AddOutline(const DirectX::XMFLOAT3& center, const DirectX::XMFLOAT3& halfExtents, uint32_t rgba);
    LineBatch& SceneLines() { return m_sceneLines; }
    void EndScene(); // the queued scene is uploaded by the next Render()

    // Renders into the currently bound render target + depth buffer, restricted to `rect`.
    // Returns the number of draw calls issued.
    uint32_t Render(ID3D11DeviceContext* context, const Rect& rect, const ViewportCamera& camera,
                    const ViewportDrawSettings& settings);

    // Pipeline-state calls of the last Render(): sent to D3D11, and filtered out as redundant.
    uint32_t StateChangesIssued() const { return m_state.Issued(); }
    uint32_t StateChangesSkipped() const { return m_state.Skipped(); }
    // Bytes uploaded to the GPU by the last Render() (scene data + per-frame constants).
    size_t BytesUploadedLastFrame() const { return m_bytesUploaded; }

private:
    struct Instance {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 scale;
        uint32_t color;
        uint32_t flags;
    };
    static_assert(sizeof(Instance) == 32);

    bool CreateMeshResources(ID3D11Device* device);
    bool UploadScene(ID3D11DeviceContext* context);
    void BindMeshInput(ID3D11DeviceContext* context);

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
    ComPtr<ID3D11PixelShader> m_wirePS;
    ComPtr<ID3D11InputLayout> m_meshLayout;

    ComPtr<ID3D11BlendState> m_alphaBlend;
    ComPtr<ID3D11DepthStencilState> m_depthTestWrite;   // opaque meshes
    ComPtr<ID3D11DepthStencilState> m_depthTestNoWrite; // grid, lines
    ComPtr<ID3D11DepthStencilState> m_depthDisabled;    // outlines
    ComPtr<ID3D11RasterizerState> m_solidRaster;        // no culling (grid)
    ComPtr<ID3D11RasterizerState> m_meshRaster;         // back-face culling
    ComPtr<ID3D11RasterizerState> m_lineRaster;

    // Built-in meshes and the wire box share one vertex + index buffer.
    ComPtr<ID3D11Buffer> m_meshVertices;
    ComPtr<ID3D11Buffer> m_meshIndices;
    MeshRange m_meshRanges[kMeshCount];
    MeshRange m_wireBoxRange;

    // Scene layer, CPU side: per-mesh buckets + outlines; GPU side: one instance buffer laid out as
    // [mesh 1 instances][mesh 2 instances]...[outline instances].
    std::vector<Instance> m_instances[kMeshCount];
    std::vector<Instance> m_outlines;
    LineBatch m_sceneLines;
    bool m_sceneDirty = true;
    ComPtr<ID3D11Buffer> m_instanceBuffer;
    size_t m_instanceCapacity = 0;
    UINT m_firstInstance[kMeshCount] = {};
    UINT m_instanceCount[kMeshCount] = {};
    UINT m_firstOutline = 0;
    UINT m_outlineCount = 0;

    StateCache m_state;
    size_t m_bytesUploaded = 0;
};

} // namespace lm
