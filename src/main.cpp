#include "app/App.h"

#include <shellapi.h>

#include <cwchar>
#include <cwctype>

namespace {

// Command line:
//   --stress N        add N items to the mock level (profiling)
//   --bench N         run the benchmark script, N frames per phase, write a report and exit
//   --bench-out PATH  benchmark report path (default: bench.txt in the working directory)
lm::AppOptions ParseCommandLine()
{
    lm::AppOptions options;
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv)
        return options;
    for (int i = 1; i + 1 < argc; ++i) {
        const wchar_t* key = argv[i];
        const wchar_t* value = argv[i + 1];
        if (std::wcscmp(key, L"--stress") == 0)
            options.stressItems = static_cast<uint32_t>(std::wcstoul(value, nullptr, 10)), ++i;
        else if (std::wcscmp(key, L"--bench") == 0)
            options.benchFrames = static_cast<uint32_t>(std::wcstoul(value, nullptr, 10)), ++i;
        else if (std::wcscmp(key, L"--bench-out") == 0)
            options.benchOutput = value, ++i;
    }
    ::LocalFree(argv);
    return options;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    lm::App app;
    if (!app.Init(ParseCommandLine())) {
        ::MessageBoxW(nullptr, L"Failed to initialize the window, Direct3D 11 or Dear ImGui.", L"LevelMinded",
                      MB_OK | MB_ICONERROR);
        return 1;
    }
    return app.Run();
}
