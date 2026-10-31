#include "platform/Window.h"

#include <dwmapi.h>

#include <algorithm>

namespace lm {

namespace {

constexpr wchar_t kClassName[] = L"LevelMindedWindow";
constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW;
constexpr int kMinClientWidth = 640;
constexpr int kMinClientHeight = 360;

} // namespace

Window::~Window()
{
    Destroy();
}

bool Window::Create(const WindowDesc& desc, WindowEventHandler* handler)
{
    m_handler = handler;
    m_instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Window::WndProcThunk;
    wc.hInstance = m_instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = nullptr; // we paint everything; avoids flicker on resize
    wc.lpszClassName = kClassName;
    if (!::RegisterClassExW(&wc))
        return false;

    m_hwnd = ::CreateWindowExW(0, kClassName, desc.title, kWindowStyle,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                               nullptr, nullptr, m_instance, this);
    if (!m_hwnd)
        return false;

    // Dark title bar to match the editor theme (ignored on older Windows builds).
    const BOOL useDarkMode = TRUE;
    ::DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));

    // Size for the DPI of the monitor the window landed on, then center it in the work area.
    const UINT dpi = ::GetDpiForWindow(m_hwnd);
    RECT rect{0, 0, ::MulDiv(desc.width, static_cast<int>(dpi), 96), ::MulDiv(desc.height, static_cast<int>(dpi), 96)};
    ::AdjustWindowRectExForDpi(&rect, kWindowStyle, FALSE, 0, dpi);

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    ::GetMonitorInfoW(::MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT& work = monitor.rcWork;
    const int workWidth = work.right - work.left;
    const int workHeight = work.bottom - work.top;
    const int width = std::min(static_cast<int>(rect.right - rect.left), workWidth);
    const int height = std::min(static_cast<int>(rect.bottom - rect.top), workHeight);
    ::SetWindowPos(m_hwnd, nullptr, work.left + (workWidth - width) / 2, work.top + (workHeight - height) / 2,
                   width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

void Window::Destroy()
{
    m_handler = nullptr; // no callbacks into a handler that is shutting down
    if (m_hwnd)
        ::DestroyWindow(m_hwnd);
    if (m_instance) {
        ::UnregisterClassW(kClassName, m_instance);
        m_instance = nullptr;
    }
}

void Window::Show()
{
    ::ShowWindow(m_hwnd, SW_SHOWDEFAULT);
}

void Window::WaitForEvents(DWORD timeoutMs) const
{
    // MWMO_INPUTAVAILABLE: also return if input is already queued but was seen by an earlier peek.
    ::MsgWaitForMultipleObjectsEx(0, nullptr, timeoutMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

bool Window::PumpMessages()
{
    MSG msg;
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            return false;
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return true;
}

float Window::DpiScale() const
{
    return m_hwnd ? static_cast<float>(::GetDpiForWindow(m_hwnd)) / 96.0f : 1.0f;
}

bool Window::IsMinimized() const
{
    return m_hwnd && ::IsIconic(m_hwnd);
}

LRESULT CALLBACK Window::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = static_cast<Window*>(create->lpCreateParams);
        self->m_hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self)
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->WndProc(msg, wParam, lParam);
}

LRESULT Window::WndProc(UINT msg, WPARAM wParam, LPARAM lParam)
{
    const HWND hwnd = m_hwnd;
    if (m_handler && m_handler->OnWindowMessage(hwnd, msg, wParam, lParam))
        return 1;

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && m_handler) {
            m_handler->OnWindowResized(LOWORD(lParam), HIWORD(lParam));
            if (m_inSizeMove)
                m_handler->OnWindowLiveResize();
        }
        return 0;

    case WM_ENTERSIZEMOVE:
        m_inSizeMove = true;
        return 0;

    case WM_EXITSIZEMOVE:
        m_inSizeMove = false;
        return 0;

    case WM_DPICHANGED: {
        if (m_handler)
            m_handler->OnWindowDpiChanged(static_cast<float>(HIWORD(wParam)) / 96.0f);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                       suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        const UINT dpi = ::GetDpiForWindow(hwnd);
        RECT rect{0, 0, ::MulDiv(kMinClientWidth, static_cast<int>(dpi), 96), ::MulDiv(kMinClientHeight, static_cast<int>(dpi), 96)};
        ::AdjustWindowRectExForDpi(&rect, kWindowStyle, FALSE, 0, dpi);
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = {rect.right - rect.left, rect.bottom - rect.top};
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // the swap chain covers the whole client area

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU) // no Alt menu activation / beep
            return 0;
        break;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;

    case WM_NCDESTROY:
        m_hwnd = nullptr;
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace lm
