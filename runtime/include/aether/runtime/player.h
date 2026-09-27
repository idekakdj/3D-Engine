// aether/runtime/player.h — the runtime player: runs a project's startup scene as a game.
//
// PlayerApp is a slim gameplay::Application (no ImGui, no editor camera): it loads the
// project's input map (or the default camera bindings), optionally drives Fly/Orbit camera
// components, renders through the scene's primary camera and exits on Player.Quit (Escape by
// default). Player.ToggleFullscreen (F11 by default) switches between windowed and borderless
// fullscreen on the primary monitor.
//
// With `check` set, the player verifies the run on shutdown (the scene loaded, a camera
// rendered it, every mesh/material resolved, scripts ran without errors, physics created
// its bodies) and check_passed() reports the result: the headless smoke test for packaged
// builds (`aether-player --frames 120 --check`).
//
// Thread-affinity: main thread only.
#pragma once

#include "aether/gameplay/application.h"
#include "aether/runtime/project.h"

#include <string>
#include <vector>

namespace aether::runtime {

inline constexpr const char* kQuitAction       = "Player.Quit";
inline constexpr const char* kFullscreenAction = "Player.ToggleFullscreen";

struct PlayerOptions {
    ProjectDesc project;
    bool        check = false;
};

class PlayerApp final : public gameplay::Application {
public:
    PlayerApp(gameplay::AppDesc desc, PlayerOptions options);
    ~PlayerApp() override;

    [[nodiscard]] bool                            check_passed() const noexcept { return check_passed_; }
    [[nodiscard]] const std::vector<std::string>& check_failures() const noexcept { return failures_; }

    void               set_fullscreen(bool fullscreen);
    [[nodiscard]] bool fullscreen() const noexcept { return fullscreen_; }

protected:
    Result<void> on_init() override;
    void         on_shutdown() override;
    void         on_update(const FrameTime& time) override;

private:
    void run_checks();
    void fail(std::string message);

    PlayerOptions            options_;
    bool                     fullscreen_ = false;
    int                      windowed_[4]{ 0, 0, 1280, 720 }; // x, y, w, h before fullscreen
    bool                     check_passed_ = true;
    std::vector<std::string> failures_;
    u64                      frames_ = 0;
    u32                      max_instances_ = 0;
};

} // namespace aether::runtime
