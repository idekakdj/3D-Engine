// environment.cpp — procedural equirectangular sky (see environment.h).
#include "aether/gameplay/environment.h"

#include <algorithm>
#include <cmath>

namespace aether::gameplay {

std::vector<f32> make_sky_equirect(u32 width, u32 height, const SkySettings& s) {
    width  = std::max(width, 4u);
    height = std::max(height, 2u);
    std::vector<f32> out(static_cast<usize>(width) * height * 4);

    const Vec3 sun_dir  = glm::normalize(s.sun_direction);
    const f32  cos_disc = std::cos(s.sun_angular_radius_deg * kDeg2Rad);
    for (u32 y = 0; y < height; ++y) {
        const f32 v     = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height);
        const f32 theta = v * kPi;
        for (u32 x = 0; x < width; ++x) {
            const f32  u   = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width);
            const f32  phi = (u - 0.5f) * kTwoPi;
            const Vec3 dir(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));

            Vec3 c;
            if (dir.y >= 0.0f) {
                // Sky: horizon -> zenith, with a slightly brighter band right at the horizon.
                const f32 t = std::pow(dir.y, 0.45f);
                c           = glm::mix(s.horizon_color, s.zenith_color, t);
            } else {
                // Ground: quickly fades from the horizon colour to the ground colour.
                const f32 t = 1.0f - std::exp(dir.y * 12.0f);
                c           = glm::mix(s.horizon_color * 0.8f, s.ground_color, t);
            }
            const f32 cos_sun = glm::dot(dir, sun_dir);
            if (s.glow > 0.0f) {
                c += s.sun_color * s.glow * std::pow(std::max(cos_sun, 0.0f), 64.0f);
            }
            if (cos_sun >= cos_disc) {
                // Soft-edged disc (limb darkening approximation).
                const f32 edge = std::clamp((cos_sun - cos_disc) / std::max(1.0f - cos_disc, 1e-6f), 0.0f, 1.0f);
                c += s.sun_color * s.sun_intensity * std::sqrt(edge);
            }
            f32* p = &out[(static_cast<usize>(y) * width + x) * 4];
            p[0]   = c.r;
            p[1]   = c.g;
            p[2]   = c.b;
            p[3]   = 1.0f;
        }
    }
    return out;
}

} // namespace aether::gameplay
