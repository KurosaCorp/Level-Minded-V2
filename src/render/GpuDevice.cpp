#include "render/GpuDevice.h"

#include <dxgi1_6.h>

#include <iterator>

namespace lm {

namespace {

constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr UINT kBackBufferCount = 2;
constexpr UINT kSwapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

std::string ToUtf8(const wchar_t* text)
{
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string result(static_cast<size_t>(size - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

HRESULT CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, UINT flags,
                     ID3D11Device** device, ID3D11DeviceContext** context)
{
    static const D3D_FEATURE_LEVEL kFeatureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    return ::D3D11CreateDevice(adapter, driverType, nullptr, flags, kFeatureLevels,
                               static_cast<UINT>(std::size(kFeatureLevels)), D3D11_SDK_VERSION,
                               device, nullptr, context);
}

} // namespace

GpuDevice::~GpuDevice()
{
    Destroy();
}

bool GpuDevice::Create(HWND hwnd)
{
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        return false;

    // Prefer the discrete GPU on hybrid laptops.
    ComPtr<IDXGIAdapter1> adapter;
    factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED;
#ifndef NDEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_DRIVER_TYPE driverType = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = CreateDevice(adapter.Get(), driverType, flags, &m_device, &m_context);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        // Debug layer missing (Windows "Graphics Tools" optional feature): continue without it.
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = CreateDevice(adapter.Get(), driverType, flags, &m_device, &m_context);
    }
    if (FAILED(hr)) // no usable GPU: fall back to the WARP software rasterizer
        hr = CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, flags, &m_device, &m_context);
    if (FAILED(hr))
        return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> deviceAdapter;
    DXGI_ADAPTER_DESC adapterDesc{};
    if (SUCCEEDED(m_device.As(&dxgiDevice)) && SUCCEEDED(dxgiDevice->GetAdapter(&deviceAdapter)) &&
        SUCCEEDED(deviceAdapter->GetDesc(&adapterDesc)))
        m_adapterName = ToUtf8(adapterDesc.Description);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Format = kBackBufferFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBackBufferCount;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = kSwapChainFlags;

    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(factory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &desc, nullptr, nullptr, &swapChain1)) ||
        FAILED(swapChain1.As(&m_swapChain)))
        return false;
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    m_swapChain->SetMaximumFrameLatency(1);
    m_frameLatencyWaitable = m_swapChain->GetFrameLatencyWaitableObject();

    m_swapChain->GetDesc1(&desc);
    m_width = desc.Width;
    m_height = desc.Height;
    return CreateSizeDependentResources();
}

void GpuDevice::Destroy()
{
    if (m_context)
        m_context->ClearState();
    if (m_frameLatencyWaitable) {
        ::CloseHandle(m_frameLatencyWaitable);
        m_frameLatencyWaitable = nullptr;
    }
    m_depthView.Reset();
    m_backBufferView.Reset();
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();
}

bool GpuDevice::CreateSizeDependentResources()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
        FAILED(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_backBufferView)))
        return false;

    D3D11_TEXTURE2D_DESC depthDesc{};
    depthDesc.Width = m_width;
    depthDesc.Height = m_height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;
    return SUCCEEDED(m_device->CreateTexture2D(&depthDesc, nullptr, &depth)) &&
           SUCCEEDED(m_device->CreateDepthStencilView(depth.Get(), nullptr, &m_depthView));
}

void GpuDevice::Resize(uint32_t width, uint32_t height)
{
    if (!m_swapChain || width == 0 || height == 0 || (width == m_width && height == m_height))
        return;

    // All references to the back buffers must be released before ResizeBuffers.
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_backBufferView.Reset();
    m_depthView.Reset();
    if (FAILED(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, kSwapChainFlags)))
        return;
    m_width = width;
    m_height = height;
    CreateSizeDependentResources();
}

void GpuDevice::WaitForNextFrame() const
{
    if (m_frameLatencyWaitable)
        ::WaitForSingleObjectEx(m_frameLatencyWaitable, 1000, TRUE);
}

void GpuDevice::BeginFrame(const float clearColor[4])
{
    ID3D11RenderTargetView* views[] = {m_backBufferView.Get()};
    m_context->OMSetRenderTargets(1, views, m_depthView.Get());
    m_context->ClearRenderTargetView(m_backBufferView.Get(), clearColor);
    m_context->ClearDepthStencilView(m_depthView.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
}

PresentResult GpuDevice::Present()
{
    const HRESULT hr = m_swapChain->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        return PresentResult::DeviceLost;
    m_occluded = (hr == DXGI_STATUS_OCCLUDED);
    return m_occluded ? PresentResult::Occluded : PresentResult::Ok;
}

bool GpuDevice::IsOccluded()
{
    if (m_occluded)
        m_occluded = m_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED;
    return m_occluded;
}

} // namespace lm
