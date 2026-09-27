// imgui_panels.cpp — the engine statistics panel (debug_ui.h).
#include "aether/gameplay/debug_ui.h"

#include "aether/gameplay/animation_bridge.h"
#include "aether/gameplay/application.h"
#include "aether/gameplay/asset_hot_reload.h"
#include "aether/gameplay/render_bridge.h"
#include "aether/physics/physics_subsystem.h"
#include "aether/renderer/renderer.h"
#include "aether/rhi/device.h"
#include "aether/scene/world.h"

#if AE_WITH_ANIMATION
#    include "aether/animation/animation_subsystem.h"
#endif
#if AE_WITH_SCRIPTING
#    include "aether/scripting/scripting_subsystem.h"
#endif

#include <imgui.h>

namespace aether::gameplay {

void draw_engine_stats(Application& app, bool* open) {
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Engine", open)) {
        ImGui::End();
        return;
    }
    const FrameTime& t = app.time();
    ImGui::Text("%.1f FPS  (%.2f ms)  frame %llu", static_cast<f64>(t.fps_smoothed),
                t.fps_smoothed > 0.0f ? 1000.0 / static_cast<f64>(t.fps_smoothed) : 0.0,
                static_cast<unsigned long long>(t.frame_index));

    // ---- simulation controls ----
    bool playing = app.simulation_enabled();
    if (ImGui::Button(playing ? "Pause" : "Play")) {
        app.set_simulation_enabled(!playing);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(playing);
    if (ImGui::Button("Step")) {
        app.step_simulation_once();
    }
    ImGui::EndDisabled();

    // ---- rendering ----
    if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
        const renderer::RendererStats& rs = app.renderer().stats();
        const rhi::FrameStats          fs = app.device().last_frame_stats();
        ImGui::Text("GPU %.2f ms   CPU record %.2f ms", fs.gpu_time_ms, rs.cpu_record_ms);
        ImGui::Text("instances %u / %u visible, %u draws, %u lights", rs.instances_visible, rs.instances_submitted,
                    rs.draw_calls, rs.lights);
        ImGui::Text("triangles %u, spot shadow maps %u", rs.triangles, rs.spot_shadow_maps);
        renderer::RendererSettings& settings = app.renderer().settings();
        ImGui::Checkbox("Shadows", &settings.shadows);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Cascaded sun (directional) shadows");
        }
        ImGui::SameLine();
        ImGui::Checkbox("Spot shadows", &settings.spot_shadows);
        ImGui::SameLine();
        ImGui::Checkbox("SSAO", &settings.ssao);
        ImGui::SameLine();
        ImGui::Checkbox("Bloom", &settings.bloom);
        ImGui::SameLine();
        ImGui::Checkbox("TAA", &settings.taa);
        ImGui::Checkbox("Debug lines", &settings.draw_debug_lines);
        if (auto* bridge = app.find_subsystem<RenderBridgeSubsystem>(); bridge != nullptr && bridge->cache() != nullptr) {
            const RenderCacheStats cs = bridge->cache()->stats();
            ImGui::Text("assets: %zu meshes, %zu materials, %zu textures", cs.meshes, cs.materials, cs.textures);
            ImGui::Text("        %zu pending, %zu failed, %zu reloads", cs.pending, cs.failed, cs.reloads);
        }
        if (auto* hot = app.find_subsystem<AssetHotReloadSubsystem>()) {
            if (ImGui::Button("Reload shaders (F5)")) {
                hot->request_shader_reload();
            }
        }
    }

    // ---- simulation ----
    if (ImGui::CollapsingHeader("World", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("entities %zu", app.world().entity_count());
        if (auto* phys = app.find_subsystem<physics::PhysicsSubsystem>()) {
            if (const physics::PhysicsWorld* pw = phys->physics_world()) {
                ImGui::Text("physics: %zu bodies (%zu active), %zu characters, %zu constraints", pw->body_count(),
                            pw->active_body_count(), pw->character_count(), pw->constraint_count());
            }
            bool draw = phys->debug_draw() != physics::DebugDrawFlags::None;
            if (ImGui::Checkbox("Physics debug draw", &draw)) {
                phys->set_debug_draw(draw ? physics::DebugDrawFlags::Default : physics::DebugDrawFlags::None);
            }
        }
#if AE_WITH_ANIMATION
        if (auto* anim = app.find_subsystem<animation::AnimationSubsystem>()) {
            ImGui::Text("animation: %u animators", anim->animated_count());
        }
#endif
#if AE_WITH_SCRIPTING
        if (auto* scripts = app.find_subsystem<scripting::ScriptingSubsystem>(); scripts != nullptr && scripts->vm()) {
            const scripting::ScriptVM& vm = *scripts->vm();
            ImGui::Text("scripts: %zu instances, %llu errors, %.1f MB Lua", vm.instance_count(),
                        static_cast<unsigned long long>(vm.error_count()),
                        static_cast<f64>(vm.memory_used()) / (1024.0 * 1024.0));
        }
#endif
    }
    ImGui::End();
}

} // namespace aether::gameplay
