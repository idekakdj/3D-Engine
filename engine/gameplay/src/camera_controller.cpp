// camera_controller.cpp — fly / orbit camera controllers and their subsystem.
#include "aether/gameplay/camera_controller.h"

#include "aether/core/input.h"
#include "aether/gameplay/input_subsystem.h"
#include "aether/scene/components.h"
#include "aether/scene/hierarchy_utils.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"

#include <algorithm>
#include <cmath>

namespace aether::gameplay {

namespace {

constexpr f32 kMaxPitch = 89.0f;

f32 wrap_degrees(f32 deg) {
    deg = std::fmod(deg, 360.0f);
    if (deg > 180.0f) {
        deg -= 360.0f;
    } else if (deg < -180.0f) {
        deg += 360.0f;
    }
    return deg;
}

} // namespace

Quat rotation_from_yaw_pitch(f32 yaw_deg, f32 pitch_deg) {
    const Quat yaw   = glm::angleAxis(yaw_deg * kDeg2Rad, Vec3(0, 1, 0));
    const Quat pitch = glm::angleAxis(pitch_deg * kDeg2Rad, Vec3(1, 0, 0));
    return glm::normalize(yaw * pitch);
}

void yaw_pitch_from_rotation(const Quat& rotation, f32& yaw_deg, f32& pitch_deg) {
    const Vec3 f = glm::normalize(rotation * Vec3(0, 0, -1));
    pitch_deg    = std::asin(std::clamp(f.y, -1.0f, 1.0f)) * kRad2Deg;
    // Looking (almost) straight up/down: yaw comes from the camera's right vector instead.
    if (std::abs(f.y) > 0.9999f) {
        const Vec3 r = rotation * Vec3(1, 0, 0);
        yaw_deg      = std::atan2(-r.z, r.x) * kRad2Deg;
    } else {
        yaw_deg = std::atan2(-f.x, -f.z) * kRad2Deg;
    }
}

void update_fly_camera(FlyCameraComponent& cam, Transform& local, const InputMap& input, f32 dt) {
    if (!cam.initialized) {
        yaw_pitch_from_rotation(local.rotation, cam.yaw_deg, cam.pitch_deg);
        cam.initialized = true;
    }
    const bool looking = !cam.require_look_button || input.action_down("Camera.Look");
    if (looking) {
        cam.yaw_deg   = wrap_degrees(cam.yaw_deg - input.axis("Camera.LookX") * cam.look_sensitivity);
        cam.pitch_deg = std::clamp(cam.pitch_deg - input.axis("Camera.LookY") * cam.look_sensitivity,
                                   -kMaxPitch, kMaxPitch);
        const f32 scroll = input.axis("Camera.Zoom");
        if (scroll != 0.0f) {
            cam.move_speed = std::clamp(cam.move_speed * std::pow(cam.speed_scroll_factor, scroll),
                                        cam.min_speed, cam.max_speed);
        }
    }
    local.rotation = rotation_from_yaw_pitch(cam.yaw_deg, cam.pitch_deg);

    const Vec3 forward = local.rotation * Vec3(0, 0, -1);
    const Vec3 right   = local.rotation * Vec3(1, 0, 0);
    Vec3       move    = forward * input.axis("Camera.MoveForward") + right * input.axis("Camera.MoveRight") +
                   Vec3(0, 1, 0) * input.axis("Camera.MoveUp");
    const f32 len = glm::length(move);
    if (len > 1.0f) {
        move /= len;
    }
    if (len > 0.0f) {
        const f32 speed = cam.move_speed * (input.action_down("Camera.Boost") ? cam.boost_multiplier : 1.0f);
        local.position += move * speed * std::max(dt, 0.0f);
    }
}

void update_orbit_camera(OrbitCameraComponent& cam, Transform& local, const InputMap& input,
                         const Vec3& follow_position, bool has_follow) {
    if (has_follow) {
        cam.target = follow_position;
    }
    const f32 dx = input.axis("Camera.LookX");
    const f32 dy = input.axis("Camera.LookY");
    if (input.action_down("Camera.Look")) {
        cam.yaw_deg   = wrap_degrees(cam.yaw_deg - dx * cam.orbit_sensitivity);
        cam.pitch_deg = std::clamp(cam.pitch_deg - dy * cam.orbit_sensitivity, -kMaxPitch, kMaxPitch);
    } else if (input.action_down("Camera.Pan") && !has_follow) {
        const Quat r     = rotation_from_yaw_pitch(cam.yaw_deg, cam.pitch_deg);
        const f32  scale = cam.pan_sensitivity * cam.distance;
        cam.target += (r * Vec3(-1, 0, 0)) * dx * scale + (r * Vec3(0, 1, 0)) * dy * scale;
    }
    const f32 zoom = input.axis("Camera.Zoom");
    if (zoom != 0.0f) {
        cam.distance *= std::pow(1.0f - cam.zoom_factor, zoom);
    }
    cam.distance   = std::clamp(cam.distance, cam.min_distance, cam.max_distance);
    local.rotation = rotation_from_yaw_pitch(cam.yaw_deg, cam.pitch_deg);
    local.position = cam.target + local.rotation * Vec3(0, 0, cam.distance);
}

// =================================================================================================
// subsystem
// =================================================================================================
CameraControllerSubsystem::CameraControllerSubsystem(InputSubsystem* input) : input_(input) {
    own_map_.add_default_camera_bindings();
}

void CameraControllerSubsystem::on_startup(EngineContext& ctx) { engine_ = &ctx; }

void CameraControllerSubsystem::on_update(FrameContext& ctx) {
    EngineContext* engine = ctx.engine != nullptr ? ctx.engine : engine_;
    if (engine == nullptr || engine->world == nullptr) {
        return;
    }
    const InputMap* map = nullptr;
    if (input_ != nullptr) {
        map = &input_->input_map();
    } else {
        own_map_.update(engine->window != nullptr ? Input::state() : InputState{});
        map = &own_map_;
    }
    update_world(*engine->world, *map, ctx.time.delta);
}

void CameraControllerSubsystem::update_world(World& world, const InputMap& input, f32 dt) {
    for (const Entity e : world.view<FlyCameraComponent, TransformComponent>()) {
        Transform t = world.get<TransformComponent>(e).local;
        update_fly_camera(world.get<FlyCameraComponent>(e), t, input, dt);
        scene::set_local_transform(world, e, t);
    }
    for (const Entity e : world.view<OrbitCameraComponent, TransformComponent>()) {
        OrbitCameraComponent& cam    = world.get<OrbitCameraComponent>(e);
        const bool            follow = cam.follow != kNullEntity && world.valid(cam.follow);
        const Vec3 follow_pos        = follow ? scene::world_position(world, cam.follow) : Vec3(0.0f);
        Transform  t                 = world.get<TransformComponent>(e).local;
        update_orbit_camera(cam, t, input, follow_pos, follow);
        // The controller computes a WORLD pose; express it in the parent's space.
        if (scene::parent_of(world, e) == kNullEntity) {
            scene::set_local_transform(world, e, t);
        } else {
            scene::set_world_matrix(world, e, t.to_matrix());
        }
    }
}

} // namespace aether::gameplay
