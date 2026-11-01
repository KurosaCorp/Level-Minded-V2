#pragma once

#include "mods/ModRuntime.h"

#include <filesystem>

namespace lm {

// Dock panel listing the loaded mods: status, memory, log, and each mod's own UI (its on_ui()).
class ModsPanel {
public:
    static constexpr const char* kWindowName = "Mods";

    struct Actions {
        bool reload = false;     // "Reload" clicked: unload and load the mods folder again
        bool openFolder = false; // "Open folder" clicked
    };
    Actions Draw(ModRuntime& mods, const std::filesystem::path& folder, float dpiScale);
};

} // namespace lm
