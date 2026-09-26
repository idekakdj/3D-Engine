// aether/gameplay/debug_ui.h — ready-made Dear ImGui panels for applications built on
// gameplay::Application (samples, runtime, editor). Call from Application::on_imgui().
//
// draw_engine_stats(): frame timing, renderer / device statistics, render-bridge cache state,
// ECS / physics / animation / scripting counters and simulation controls (play / pause / step).
//
// Main thread only; requires AppDesc::imgui.
#pragma once

namespace aether::gameplay {

class Application;

// `open` (optional) receives the window's close button state.
void draw_engine_stats(Application& app, bool* open = nullptr);

} // namespace aether::gameplay
