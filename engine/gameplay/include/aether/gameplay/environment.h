// aether/gameplay/environment.h — procedural HDR sky for image-based lighting.
//
// make_sky_equirect() renders an analytic sky (zenith/horizon gradient, darker ground, a soft
// sun disc with a glow) into an equirectangular RGBA32F image using the renderer's mapping
// (u = atan2(z, x) / 2pi + 0.5, v = acos(y) / pi; v = 0 is straight up). Hand the pixels to
// renderer::Renderer::register_environment() and put the handle in
// EnvironmentSettings::skybox (RenderBridgeSubsystem::set_environment) to get a skybox plus
// diffuse/specular IBL without any asset on disk.
//
// Thread-safety: pure function; thread-safe.
#pragma once

#include "aether/core/math.h"
#include "aether/core/types.h"

#include <vector>

namespace aether::gameplay {

struct SkySettings {
    Vec3 zenith_color{ 0.12f, 0.25f, 0.55f };  // linear HDR radiance
    Vec3 horizon_color{ 0.5f, 0.56f, 0.64f };
    Vec3 ground_color{ 0.12f, 0.11f, 0.1f };
    Vec3 sun_direction{ -0.4f, 0.75f, 0.5f };  // towards the sun (normalised internally)
    Vec3 sun_color{ 1.0f, 0.95f, 0.85f };
    f32  sun_intensity = 40.0f;                // radiance multiplier of the disc
    f32  sun_angular_radius_deg = 1.5f;
    f32  glow = 0.6f;                          // strength of the halo around the sun
};

// width x height x 4 floats (alpha = 1). width >= 4, height >= 2.
[[nodiscard]] std::vector<f32> make_sky_equirect(u32 width, u32 height, const SkySettings& settings = {});

} // namespace aether::gameplay
