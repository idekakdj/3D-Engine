// aether/gameplay/camera_controller.h — fly and orbit camera controllers.
//
// Components (add to an entity that also has a CameraComponent):
//   FlyCameraComponent   — free-fly (editor style): hold the right mouse button to look,
//                          WASD move, E/Q up/down, Shift boost, mouse wheel scales speed.
//   OrbitCameraComponent — orbits `target` (or the `follow` entity): right mouse orbits,
//                          middle mouse pans, the wheel zooms.
// Both are driven by CameraControllerSubsystem using the "Camera.*" bindings of an InputMap
// (InputMap::add_default_camera_bindings). The subsystem is NOT a default subsystem: apps
// add it with the kind they want (Engine: the camera keeps working while the simulation is
// paused; Simulation: only during play).
//
// Conventions: right-handed, +Y up, cameras look down their local -Z. Yaw rotates about
// world +Y (0 = looking down -Z), pitch about the local X axis (positive looks up).
//
// Thread-affinity: main thread only.
#pragma once

#include "aether/core/math.h"
#include "aether/core/subsystem.h"
#include "aether/core/types.h"
#include "aether/gameplay/input_map.h"
#include "aether/scene/entity.h"

namespace aether {
class World;
}

namespace aether::gameplay {

class InputSubsystem;

struct FlyCameraComponent {
    f32  move_speed = 5.0f;          // m/s
    f32  boost_multiplier = 4.0f;    // while Camera.Boost is down
    f32  look_sensitivity = 0.15f;   // degrees per pixel of mouse delta
    f32  speed_scroll_factor = 1.2f; // speed *= factor^scroll
    f32  min_speed = 0.1f;
    f32  max_speed = 500.0f;
    bool require_look_button = true; // mouse look only while Camera.Look is down
    // State (derived from the transform on the first update when !initialized).
    f32  yaw_deg = 0.0f;
    f32  pitch_deg = 0.0f;
    bool initialized = false;
};

struct OrbitCameraComponent {
    Vec3   target{ 0.0f };
    Entity follow = kNullEntity;     // when valid, target = follow's world position
    f32    distance = 10.0f;
    f32    min_distance = 0.5f;
    f32    max_distance = 500.0f;
    f32    yaw_deg = 0.0f;
    f32    pitch_deg = -20.0f;
    f32    orbit_sensitivity = 0.25f; // degrees per pixel
    f32    zoom_factor = 0.1f;        // distance *= (1 - zoom_factor)^scroll
    f32    pan_sensitivity = 0.002f;  // world units per pixel per metre of distance
};

// Pure controller steps (unit-tested). They read the "Camera.*" names of `input` and write
// the new local transform (position + rotation; scale untouched).
void update_fly_camera(FlyCameraComponent& cam, Transform& local, const InputMap& input, f32 dt);
void update_orbit_camera(OrbitCameraComponent& cam, Transform& local, const InputMap& input,
                         const Vec3& follow_position, bool has_follow);

// Yaw/pitch (degrees) of a camera rotation (inverse of the controllers' rotation build).
void yaw_pitch_from_rotation(const Quat& rotation, f32& yaw_deg, f32& pitch_deg);
[[nodiscard]] Quat rotation_from_yaw_pitch(f32 yaw_deg, f32 pitch_deg);

class CameraControllerSubsystem final : public ISubsystem {
public:
    // `input` supplies the InputMap; nullptr => the subsystem keeps its own map with the
    // default camera bindings fed from Input::state() (no ImGui masking).
    explicit CameraControllerSubsystem(InputSubsystem* input = nullptr);

    [[nodiscard]] const char* name() const override { return "CameraController"; }
    void on_startup(EngineContext& ctx) override;
    void on_update(FrameContext& ctx) override;

    // Applies the controllers of `world` directly (hosts without a subsystem loop).
    void update_world(World& world, const InputMap& input, f32 dt);

private:
    InputSubsystem* input_ = nullptr;
    EngineContext*  engine_ = nullptr;
    InputMap        own_map_; // used when input_ == nullptr
};

} // namespace aether::gameplay
