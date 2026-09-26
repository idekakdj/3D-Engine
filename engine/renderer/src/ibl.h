// ibl.h — image-based lighting precomputation (GPU, via Device::immediate_submit).
//
// Private header. From an equirectangular HDR image:
//   environment cube  : RGBA16F, full mip chain, each mip resampled from the (mipmapped)
//                       equirect so no intra-texture read/write hazard exists
//   irradiance cube   : 32^2 cosine-convolved radiance, stored pre-divided by pi
//                       (diffuse = albedo * irradiance(N))
//   prefiltered cube  : GGX split-sum prefilter, roughness = mip / (mips - 1), filtered
//                       importance sampling (Krivanek & Colbert) against the env mips
// plus the split-sum BRDF LUT (RG16F, x = NdotV, y = roughness), generated once.
// Main thread only.
#pragma once

#include "pipelines.h"

#include "aether/core/error.h"
#include "aether/renderer/renderer.h"
#include "aether/rhi/device.h"

namespace aether::renderer {

struct IblSamplers {
    rhi::SamplerHandle linear_clamp; // cubes + LUT (trilinear)
    rhi::SamplerHandle linear_repeat; // equirect source (wraps horizontally)
};

struct IblTexture {
    rhi::TextureHandle    texture;
    rhi::DescriptorHandle sampled;
    u32                   mips = 1;
    u32                   size = 0;
};

struct EnvironmentMaps {
    IblTexture environment;
    IblTexture irradiance;
    IblTexture prefiltered;
};

inline constexpr u32 kBrdfLutSize = 128;
inline constexpr u32 kIrradianceSize = 32;
inline constexpr u32 kPrefilteredSize = 256;
inline constexpr u32 kPrefilteredMips = 6; // 256 .. 8

Result<IblTexture> create_brdf_lut(rhi::Device& device, PipelineLibrary& pipelines,
                                   const IblSamplers& samplers);

Result<EnvironmentMaps> create_environment_maps(rhi::Device& device, PipelineLibrary& pipelines,
                                                const IblSamplers& samplers,
                                                const EnvironmentUpload& upload);

void destroy_ibl_texture(rhi::Device& device, IblTexture& t);
void destroy_environment_maps(rhi::Device& device, EnvironmentMaps& maps);

// Face edge for an equirect of the given width (power of two, clamped to [32, 1024]).
[[nodiscard]] u32 environment_face_size(u32 equirect_width);

} // namespace aether::renderer
