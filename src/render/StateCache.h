#pragma once

#include <d3d11.h>

#include <cstdint>

namespace lm {

// Filters redundant D3D11 pipeline-state calls within a frame: each setter only reaches the
// device context when the value differs from what this cache last bound. Call Reset() whenever
// other code (e.g. the ImGui backend) may have changed state, which forgets everything.
class StateCache {
public:
    void Reset()
    {
        *this = StateCache{};
    }

    void InputLayout(ID3D11DeviceContext* c, ID3D11InputLayout* v)
    {
        if (Changed(m_layout, v))
            c->IASetInputLayout(v);
    }
    void Topology(ID3D11DeviceContext* c, D3D11_PRIMITIVE_TOPOLOGY v)
    {
        if (Changed(m_topology, v))
            c->IASetPrimitiveTopology(v);
    }
    void VertexShader(ID3D11DeviceContext* c, ID3D11VertexShader* v)
    {
        if (Changed(m_vs, v))
            c->VSSetShader(v, nullptr, 0);
    }
    void PixelShader(ID3D11DeviceContext* c, ID3D11PixelShader* v)
    {
        if (Changed(m_ps, v))
            c->PSSetShader(v, nullptr, 0);
    }
    void Rasterizer(ID3D11DeviceContext* c, ID3D11RasterizerState* v)
    {
        if (Changed(m_rasterizer, v))
            c->RSSetState(v);
    }
    void Blend(ID3D11DeviceContext* c, ID3D11BlendState* v)
    {
        if (Changed(m_blend, v))
            c->OMSetBlendState(v, nullptr, 0xFFFFFFFF);
    }
    void DepthStencil(ID3D11DeviceContext* c, ID3D11DepthStencilState* v)
    {
        if (Changed(m_depth, v))
            c->OMSetDepthStencilState(v, 0);
    }
    // Binds vertex buffer slots 0 and 1 (slot 1 may be null).
    void VertexBuffers(ID3D11DeviceContext* c, ID3D11Buffer* slot0, UINT stride0, ID3D11Buffer* slot1, UINT stride1)
    {
        const VertexBufferPair v{slot0, slot1, stride0, stride1};
        if (Changed(m_vertexBuffers, v)) {
            ID3D11Buffer* buffers[] = {slot0, slot1};
            const UINT strides[] = {stride0, stride1};
            const UINT offsets[] = {0, 0};
            c->IASetVertexBuffers(0, 2, buffers, strides, offsets);
        }
    }
    void IndexBuffer(ID3D11DeviceContext* c, ID3D11Buffer* v)
    {
        if (Changed(m_indexBuffer, v))
            c->IASetIndexBuffer(v, DXGI_FORMAT_R16_UINT, 0);
    }

    uint32_t Issued() const { return m_issued; }
    uint32_t Skipped() const { return m_skipped; }

private:
    struct VertexBufferPair {
        ID3D11Buffer* slot0;
        ID3D11Buffer* slot1;
        UINT stride0;
        UINT stride1;
        bool operator==(const VertexBufferPair&) const = default;
    };

    template <typename T>
    struct Slot {
        T value{};
        bool known = false;
    };

    template <typename T>
    bool Changed(Slot<T>& slot, const T& value)
    {
        if (slot.known && slot.value == value) {
            ++m_skipped;
            return false;
        }
        slot.value = value;
        slot.known = true;
        ++m_issued;
        return true;
    }

    Slot<ID3D11InputLayout*> m_layout;
    Slot<D3D11_PRIMITIVE_TOPOLOGY> m_topology;
    Slot<ID3D11VertexShader*> m_vs;
    Slot<ID3D11PixelShader*> m_ps;
    Slot<ID3D11RasterizerState*> m_rasterizer;
    Slot<ID3D11BlendState*> m_blend;
    Slot<ID3D11DepthStencilState*> m_depth;
    Slot<VertexBufferPair> m_vertexBuffers;
    Slot<ID3D11Buffer*> m_indexBuffer;
    uint32_t m_issued = 0;
    uint32_t m_skipped = 0;
};

} // namespace lm
