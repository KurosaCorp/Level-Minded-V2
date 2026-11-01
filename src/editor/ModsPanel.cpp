#include "editor/ModsPanel.h"

#include <imgui.h>

namespace lm {

namespace {

// Mod UI drawn with ImGui. Mods only reach these calls (never Begin/End or ID stack functions),
// so a failing mod can't leave ImGui's window or ID stacks unbalanced.
class ImGuiModUi final : public ModUi {
public:
    void Text(const char* text) override { ImGui::TextWrapped("%s", text); }
    bool Button(const char* label) override { return ImGui::Button(label); }
    bool Checkbox(const char* label, bool& value) override { return ImGui::Checkbox(label, &value); }
    bool Slider(const char* label, float& value, float min, float max) override
    {
        return ImGui::SliderFloat(label, &value, min, max, "%.2f");
    }
    void Separator() override { ImGui::Separator(); }
    void SameLine() override { ImGui::SameLine(); }
};

} // namespace

ModsPanel::Actions ModsPanel::Draw(ModRuntime& mods, const std::filesystem::path& folder, float dpiScale)
{
    Actions actions;
    if (!ImGui::Begin(kWindowName)) {
        ImGui::End();
        return actions;
    }

    actions.reload = ImGui::Button("Reload");
    ImGui::SetItemTooltip("Unload every mod and load the mods folder again\n(after editing or adding mod files).");
    ImGui::SameLine();
    actions.openFolder = ImGui::Button("Open folder");
    ImGui::SetItemTooltip("%ls", folder.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%zu mod%s", mods.Count(), mods.Count() == 1 ? "" : "s");
    ImGui::Separator();

    if (mods.Count() == 0) {
        ImGui::TextWrapped("No mods loaded. Put Lua mods (.lua files, or folders with a main.lua) in the mods folder "
                           "next to LevelMinded.exe and click Reload. See docs/MODDING.md for the API.");
        ImGui::End();
        return actions;
    }

    ImGuiModUi ui;
    for (size_t i = 0; i < mods.Count(); ++i) {
        const ModInfo& info = mods.Info(i);
        ImGui::PushID(static_cast<int>(i));
        const bool failed = info.state == ModState::Failed;
        if (failed)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
        const bool open = ImGui::CollapsingHeader(info.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        if (failed)
            ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%ls", info.file.c_str());
        if (open) {
            ImGui::Indent(6.0f * dpiScale);
            if (failed) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
                ImGui::TextWrapped("Stopped: %s", info.error.c_str());
                ImGui::PopStyleColor();
                ImGui::TextDisabled("Fix the mod file and click Reload.");
            } else {
                ImGui::TextDisabled("Running, %.0f KB of %.0f MB", double(info.memoryBytes) / 1024.0,
                                    double(mods.Limits().memoryBytes) / (1024.0 * 1024.0));
                if (info.hasUi)
                    mods.DrawUi(i, ui); // the mod's own controls
            }
            if (!info.log.empty() && ImGui::TreeNode("##log", "Log (%zu)", info.log.size())) {
                const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
                const float height = lineHeight * float(info.log.size() < 8 ? info.log.size() : 8) + 4.0f * dpiScale;
                if (ImGui::BeginChild("##lines", ImVec2(0.0f, height), ImGuiChildFlags_Borders)) {
                    for (const std::string& line : info.log)
                        ImGui::TextUnformatted(line.c_str(), line.c_str() + line.size());
                    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                        ImGui::SetScrollHereY(1.0f); // follow new lines
                }
                ImGui::EndChild();
                ImGui::TreePop();
            }
            ImGui::Unindent(6.0f * dpiScale);
        }
        ImGui::PopID();
    }
    ImGui::End();
    return actions;
}

} // namespace lm
