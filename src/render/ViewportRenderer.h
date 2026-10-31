#pragma once

#include "core/MeshId.h"
#include "core/Rect.h"
#include "render/LineBatch.h"
#include "render/MeshLibrary.h"
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

// Identifies an instance queued since the last BeginScene(), for UpdateInstance().
using InstanceHandle = uint32_t;
inline constexpr InstanceHandle kNoInstance = 0xFFFFFFFFu;

// Flags for AddMeshInstance.
inline constexpr uint32_t kMeshFlagSelected = 1u;
inline constexpr uint32_t kMeshFlagEmissive = 2u;

// Draws the 3D scene of one viewport.
//
// Scene content is retained: it is queued between BeginScene() and EndScene() only when the scene
// changed, uploaded to the GPU once, and redrawn from GPU buffers every frame after that. A frame
// where only the camera moves uploads nothing but one small constant buffer. Moving items
// (UpdateInstance) re-uploads just the changed instance ranges.
//
// Meshes come from a MeshLibrary (built-in + imported, one contiguous vertex/index array). The
// renderer mirrors it in one GPU vertex buffer and one 32-bit index buffer that grow by doubling;
// when a model is imported only the new vertices and indices are uploaded.
//
// Draw order (one draw call each, skipped when empty):
//   1. solid meshes   - instanced, one draw per mesh type in use (writes depth)
//   2. ground grid    - one full-viewport triangle (depth-tested against the meshes)
//   3. scene lines    - depth-tested (axes, volumes)
//   4. outlines       - instanced wire boxes, always on top (selection)
class ViewportRenderer {
public:
    // `meshes` must outlive the renderer; meshes appended to it later are picked up by Render().
    bool Create(ID3D11Device* device, const MeshLibrary* meshes);

    // ---- Scene layer (retained until the next BeginScene)
    void BeginScene();
    // Library `mesh` (modeled in [-1, 1]^3) scaled by `size` (half extents), rotated by `rotation`
    // (unit quaternion) and placed at `position`.
    InstanceHandle AddMeshInstance(MeshId mesh, const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT4& rotation,
                                   const DirectX::XMFLOAT3& size, uint32_t rgba, uint32_t flags = 0);
    // Always-on-top wireframe box, placed like a mesh instance.
    InstanceHandle AddOutline(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT4& rotation,
                              const DirectX::XMFLOAT3& size, uint32_t rgba);
    LineBatch& SceneLines() { return m_sceneLines; }
    void EndScene(); // the queued scene is uploaded by the next Render()

    // Moves an instance of the retained scene. Only the changed range is uploaded by the next Render().
    void UpdateInstance(InstanceHandle handle, const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT4& rotation,
                        const DirectX::XMFLOAT3& size);
    // Call after changing SceneLines() outside BeginScene/EndScene: the next Render() re-uploads them.
    void SceneLinesChanged() { m_linesDirty = true; }

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
        DirectX::XMFLOAT4 rotation; // unit quaternion
        DirectX::XMFLOAT3 scale;
        uint32_t color;
        uint32_t flags;
    };
    static_assert(sizeof(Instance) == 48);

    bool CreateMeshResources(ID3D11Device* device);
    bool SyncMeshes(ID3D11DeviceContext* context);
    bool UploadScene(ID3D11DeviceContext* context);
    void UploadInstanceRange(ID3D11DeviceContext* context, const Instance* data, UINT first, UINT count);
    std::vector<Instance>& Bucket(uint32_t bucket) { return bucket == 0 ? m_outlines : m_instances[bucket]; }
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

    // GPU copy of the mesh library: every mesh and the wire box in one vertex + index buffer.
    const MeshLibrary* m_meshes = nullptr;
    ComPtr<ID3D11Buffer> m_meshVertices;
    ComPtr<ID3D11Buffer> m_meshIndices;
    size_t m_vertexCapacity = 0;   // vertices / indices the GPU buffers can hold
    size_t m_indexCapacity = 0;
    size_t m_uploadedVertices = 0; // library prefix already on the GPU
    size_t m_uploadedIndices = 0;
    uint64_t m_meshVersion = 0;
    uint64_t m_meshGeneration = 0;

    // Scene layer, CPU side: per-mesh buckets + outlines; GPU side: one instance buffer laid out as
    // [mesh 1 instances][mesh 2 instances]...[outline instances]. An InstanceHandle is
    // (bucket << 20) | index, with bucket 0 (MeshId::None, never drawn as a mesh) meaning outlines.
    // The per-bucket vectors are indexed by MeshId and grow with the library (at BeginScene).
    std::vector<std::vector<Instance>> m_instances;
    std::vector<Instance> m_outlines;
    LineBatch m_sceneLines;
    bool m_sceneDirty = true;
    bool m_linesDirty = false;
    std::vector<uint32_t> m_dirtyBegin; // per bucket: instances changed by UpdateInstance, [begin, end)
    std::vector<uint32_t> m_dirtyEnd;
    ComPtr<ID3D11Buffer> m_instanceBuffer;
    size_t m_instanceCapacity = 0;
    std::vector<UINT> m_firstInstance;
    std::vector<UINT> m_instanceCount;
    UINT m_firstOutline = 0;
    UINT m_outlineCount = 0;

    StateCache m_state;
    size_t m_bytesUploaded = 0;
};

} // namespace lm
