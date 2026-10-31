#pragma once

#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace lm {

enum class PresentResult { Ok, Occluded, DeviceLost };

// Direct3D 11 device plus a flip-model swap chain for one window.
// The device is created single-threaded: only use it from the main thread.
class GpuDevice {
public:
    GpuDevice() = default;
    ~GpuDevice();
    GpuDevice(const GpuDevice&) = delete;
    GpuDevice& operator=(const GpuDevice&) = delete;

    bool Create(HWND hwnd);
    void Destroy();

    // Resizes the back buffers. Zero sizes and unchanged sizes are ignored.
    void Resize(uint32_t width, uint32_t height);

    // Blocks until the swap chain can accept a new frame (keeps input latency to one frame).
    void WaitForNextFrame() const;
    // Binds and clears the back buffer and depth buffer (reversed-Z: cleared to 0 = infinitely far).
    void BeginFrame(const float clearColor[4]);
    // Presents with vsync.
    PresentResult Present();
    // True while the window is fully hidden (minimized, screen locked). Cheap when not occluded.
    bool IsOccluded();

    ID3D11Device* Device() const { return m_device.Get(); }
    ID3D11DeviceContext* Context() const { return m_context.Get(); }
    const std::string& AdapterName() const { return m_adapterName; }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

private:
    bool CreateSizeDependentResources();

    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain2> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_backBufferView;
    ComPtr<ID3D11DepthStencilView> m_depthView;
    HANDLE m_frameLatencyWaitable = nullptr;
    std::string m_adapterName;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_occluded = false;
};

} // namespace lm
