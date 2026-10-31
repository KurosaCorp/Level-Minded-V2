#pragma once

#include "core/Rect.h"
#include "render/LineBatch.h"

#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>

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

// Draws the 3D scene of one viewport: the infinite ground grid, world axes, and batched lines.
class ViewportRenderer {
public:
    bool Create(ID3D11Device* device);

    // Lines queued here are drawn (in one draw call) by the next Render() call.
    LineBatch& Lines() { return m_lines; }

    // Renders into the currently bound render target + depth buffer, restricted to `rect`.
    // Returns the number of draw calls issued.
    uint32_t Render(ID3D11DeviceContext* context, const Rect& rect, const ViewportCamera& camera,
                    const ViewportDrawSettings& settings);

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D11Buffer> m_frameConstants;
    ComPtr<ID3D11VertexShader> m_gridVS;
    ComPtr<ID3D11PixelShader> m_gridPS;
    ComPtr<ID3D11VertexShader> m_lineVS;
    ComPtr<ID3D11PixelShader> m_linePS;
    ComPtr<ID3D11InputLayout> m_lineLayout;
    ComPtr<ID3D11BlendState> m_alphaBlend;
    ComPtr<ID3D11DepthStencilState> m_depthTestNoWrite;
    ComPtr<ID3D11RasterizerState> m_solidRaster;
    ComPtr<ID3D11RasterizerState> m_lineRaster;
    LineBatch m_lines;
};

} // namespace lm
