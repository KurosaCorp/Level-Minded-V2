#pragma once

#include <windows.h>

#include <cstdint>
#include <span>
#include <string>

namespace lm {

// Receives window events. All callbacks run on the main thread, from inside message dispatch.
class WindowEventHandler {
public:
    // Called first for every window message. Return true if the message was fully handled.
    virtual bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) = 0;
    virtual void OnWindowResized(uint32_t width, uint32_t height) = 0;
    virtual void OnWindowDpiChanged(float dpiScale) = 0;
    // Called repeatedly while the user drags the window border (the OS runs a modal loop then).
    virtual void OnWindowLiveResize() = 0;
    // Files dragged from Explorer and dropped onto the window (full paths).
    virtual void OnFilesDropped(std::span<const std::wstring> paths) = 0;

protected:
    virtual ~WindowEventHandler() = default;
};

struct WindowDesc {
    const wchar_t* title = L"";
    int width = 1280; // client size in logical pixels (96 DPI); scaled to the monitor's DPI
    int height = 720;
};

// Native Win32 top-level window.
class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool Create(const WindowDesc& desc, WindowEventHandler* handler);
    void Destroy();
    void Show();

    // Blocks the thread until a message arrives or timeoutMs elapses (INFINITE allowed).
    void WaitForEvents(DWORD timeoutMs) const;
    // Dispatches all queued messages. Returns false once the application should quit.
    bool PumpMessages();

    HWND Handle() const { return m_hwnd; }
    float DpiScale() const;
    bool IsMinimized() const;

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    HINSTANCE m_instance = nullptr;
    WindowEventHandler* m_handler = nullptr;
    bool m_inSizeMove = false;
};

} // namespace lm
