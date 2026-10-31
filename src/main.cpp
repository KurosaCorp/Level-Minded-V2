#include "app/App.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    lm::App app;
    if (!app.Init()) {
        ::MessageBoxW(nullptr, L"Failed to initialize the window, Direct3D 11 or Dear ImGui.", L"LevelMinded",
                      MB_OK | MB_ICONERROR);
        return 1;
    }
    return app.Run();
}
