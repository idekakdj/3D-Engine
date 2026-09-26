// ibl.cpp — IBL precomputation (see ibl.h).
#include "ibl.h"

#include "format_utils.h"
#include "gpu_data.h"

#include "aether/core/log.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

namespace aether::renderer {

namespace {
constexpr const char* kLogCat = "Renderer";
constexpr u32         kGroup = 8;

u32 groups(u32 n) { return (n + kGroup - 1) / kGroup; }

u32 idx(rhi::DescriptorHandle h) { return h.is_valid() ? h.index() : kGpuInvalidIndex; }

rhi::TextureHandle make_cube(rhi::Device& device, u32 size, u32 mips, const char* name) {
    rhi::TextureDesc d;
    d.type = rhi::TextureType::Cube;
    d.format = rhi::Format::RGBA16F;
    d.width = size;
    d.height = size;
    d.mip_levels = mips;
    d.array_layers = 6;
    d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
    d.debug_name = name;
    return device.create_texture(d);
}

// Registers a storage view for every mip of `tex`.
std::vector<rhi::DescriptorHandle> storage_views(rhi::Device& device, rhi::TextureHandle tex, u32 mips) {
    std::vector<rhi::DescriptorHandle> v;
    v.reserve(mips);
    for (u32 m = 0; m < mips; ++m) {
        v.push_back(device.register_storage_texture(tex, m));
    }
    return v;
}

void release_storage_views(rhi::Device& device, std::vector<rhi::DescriptorHandle>& views) {
    for (rhi::DescriptorHandle h : views) {
        if (h.is_valid()) {
            device.unregister_storage_texture(h);
        }
    }
    views.clear();
}

bool all_valid(const std::vector<rhi::DescriptorHandle>& v) {
    return std::all_of(v.begin(), v.end(), [](rhi::DescriptorHandle h) { return h.is_valid(); });
}
} // namespace

u32 environment_face_size(u32 equirect_width) {
    const u32 target = std::max(1u, equirect_width / 4);
    return std::clamp(std::bit_ceil(target), 32u, 1024u);
}

void destroy_ibl_texture(rhi::Device& device, IblTexture& t) {
    if (t.sampled.is_valid()) {
        device.unregister_texture(t.sampled);
    }
    if (t.texture.is_valid()) {
        device.destroy(t.texture);
    }
    t = IblTexture{};
}

void destroy_environment_maps(rhi::Device& device, EnvironmentMaps& maps) {
    destroy_ibl_texture(device, maps.environment);
    destroy_ibl_texture(device, maps.irradiance);
    destroy_ibl_texture(device, maps.prefiltered);
}

Result<IblTexture> create_brdf_lut(rhi::Device& device, PipelineLibrary& pipelines,
                                   const IblSamplers& samplers) {
    const rhi::PipelineHandle pso = pipelines.get(PipelineId::BrdfLut);
    if (!pso.is_valid()) {
        return make_error<IblTexture>(ErrorCode::NotInitialized, "BRDF LUT pipeline unavailable");
    }
    IblTexture lut;
    rhi::TextureDesc d;
    d.type = rhi::TextureType::Tex2D;
    d.format = rhi::Format::RG16F;
    d.width = kBrdfLutSize;
    d.height = kBrdfLutSize;
    d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
    d.debug_name = "IBL.BrdfLut";
    lut.texture = device.create_texture(d);
    lut.size = kBrdfLutSize;
    if (!lut.texture.is_valid()) {
        return make_error<IblTexture>(ErrorCode::OutOfMemory, "BRDF LUT texture creation failed");
    }
    const rhi::DescriptorHandle storage = device.register_storage_texture(lut.texture, 0);
    if (!storage.is_valid()) {
        device.destroy(lut.texture);
        return make_error<IblTexture>(ErrorCode::Internal, "BRDF LUT storage registration failed");
    }
    device.immediate_submit([&](rhi::CommandList& cmd) {
        cmd.push_debug_group("IBL.BrdfLut");
        cmd.barrier(lut.texture, rhi::ResourceState::Undefined, rhi::ResourceState::ShaderWrite);
        cmd.bind_pipeline(pso);
        IblPush push;
        push.dst_img = storage.index();
        push.dst_size = kBrdfLutSize;
        push.sample_count = 1024;
        cmd.push_constants(rhi::ShaderStage::Compute, 0, sizeof(push), &push);
        cmd.dispatch(groups(kBrdfLutSize), groups(kBrdfLutSize), 1);
        cmd.barrier(lut.texture, rhi::ResourceState::ShaderWrite, rhi::ResourceState::ShaderRead);
        cmd.pop_debug_group();
    });
    device.unregister_storage_texture(storage);
    lut.sampled = device.register_texture(lut.texture, samplers.linear_clamp);
    return lut;
}

Result<EnvironmentMaps> create_environment_maps(rhi::Device& device, PipelineLibrary& pipelines,
                                                const IblSamplers& samplers,
                                                const EnvironmentUpload& upload) {
    const rhi::PipelineHandle to_cube = pipelines.get(PipelineId::EquirectToCube);
    const rhi::PipelineHandle irradiance = pipelines.get(PipelineId::Irradiance);
    const rhi::PipelineHandle prefilter = pipelines.get(PipelineId::Prefilter);
    if (!to_cube.is_valid() || !irradiance.is_valid() || !prefilter.is_valid()) {
        return make_error<EnvironmentMaps>(ErrorCode::NotInitialized, "IBL pipelines unavailable");
    }
    const u64 texels = static_cast<u64>(upload.width) * upload.height;
    if (upload.width == 0 || upload.height == 0 || upload.rgba32f.size() < texels * 4) {
        return make_error<EnvironmentMaps>(ErrorCode::InvalidArgument,
                                           "EnvironmentUpload: size/pixel count mismatch");
    }

    // ---- equirect source: RGBA16F (linear filtering is mandatory for it), full mips ----
    std::vector<u16> half(static_cast<usize>(texels * 4));
    for (usize i = 0; i < half.size(); ++i) {
        const f32 v = upload.rgba32f[i];
        half[i] = f32_to_f16(std::isfinite(v) ? std::clamp(v, 0.0f, 65000.0f) : 0.0f);
    }
    rhi::TextureDesc ed;
    ed.type = rhi::TextureType::Tex2D;
    ed.format = rhi::Format::RGBA16F;
    ed.width = upload.width;
    ed.height = upload.height;
    ed.mip_levels = full_mip_count(upload.width, upload.height);
    ed.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst | rhi::TextureUsage::TransferSrc;
    ed.debug_name = upload.debug_name + ".Equirect";
    const rhi::TextureHandle equirect = device.create_texture(ed);
    if (!equirect.is_valid()) {
        return make_error<EnvironmentMaps>(ErrorCode::OutOfMemory, "equirect texture creation failed");
    }
    device.update_texture(equirect,
                          ByteSpan(reinterpret_cast<const byte*>(half.data()), half.size() * sizeof(u16)),
                          true);
    const rhi::DescriptorHandle equirect_desc = device.register_texture(equirect, samplers.linear_repeat);

    EnvironmentMaps maps;
    const u32 face = environment_face_size(upload.width);
    maps.environment.size = face;
    maps.environment.mips = full_mip_count(face, face);
    maps.environment.texture = make_cube(device, face, maps.environment.mips, "IBL.Environment");
    maps.irradiance.size = kIrradianceSize;
    maps.irradiance.texture = make_cube(device, kIrradianceSize, 1, "IBL.Irradiance");
    maps.prefiltered.size = std::min(kPrefilteredSize, face);
    maps.prefiltered.mips = std::min(kPrefilteredMips, full_mip_count(maps.prefiltered.size, maps.prefiltered.size));
    maps.prefiltered.texture =
        make_cube(device, maps.prefiltered.size, maps.prefiltered.mips, "IBL.Prefiltered");

    auto cleanup_source = [&] {
        if (equirect_desc.is_valid()) {
            device.unregister_texture(equirect_desc);
        }
        device.destroy(equirect);
    };
    if (!maps.environment.texture.is_valid() || !maps.irradiance.texture.is_valid() ||
        !maps.prefiltered.texture.is_valid() || !equirect_desc.is_valid()) {
        cleanup_source();
        destroy_environment_maps(device, maps);
        return make_error<EnvironmentMaps>(ErrorCode::OutOfMemory, "IBL texture creation failed");
    }

    maps.environment.sampled = device.register_texture(maps.environment.texture, samplers.linear_clamp);
    std::vector<rhi::DescriptorHandle> env_views = storage_views(device, maps.environment.texture, maps.environment.mips);
    std::vector<rhi::DescriptorHandle> irr_views = storage_views(device, maps.irradiance.texture, 1);
    std::vector<rhi::DescriptorHandle> pre_views = storage_views(device, maps.prefiltered.texture, maps.prefiltered.mips);
    if (!maps.environment.sampled.is_valid() || !all_valid(env_views) || !all_valid(irr_views) ||
        !all_valid(pre_views)) {
        release_storage_views(device, env_views);
        release_storage_views(device, irr_views);
        release_storage_views(device, pre_views);
        cleanup_source();
        destroy_environment_maps(device, maps);
        return make_error<EnvironmentMaps>(ErrorCode::Internal, "IBL descriptor registration failed");
    }

    const u32 env_mips = maps.environment.mips;
    device.immediate_submit([&](rhi::CommandList& cmd) {
        cmd.push_debug_group("IBL.Precompute");

        // 1) equirect -> environment cube, every mip sampled directly from the source.
        cmd.push_debug_group("EquirectToCube");
        cmd.barrier(maps.environment.texture, rhi::ResourceState::Undefined, rhi::ResourceState::ShaderWrite);
        cmd.bind_pipeline(to_cube);
        for (u32 m = 0; m < env_mips; ++m) {
            const u32 size = std::max(1u, face >> m);
            IblPush p;
            p.src_tex = idx(equirect_desc);
            p.dst_img = idx(env_views[m]);
            p.dst_size = size;
            // One cube texel spans ~ (width / 4) / size equirect texels horizontally.
            p.src_lod = std::max(0.0f, std::log2(static_cast<f32>(upload.width) / (4.0f * static_cast<f32>(size))));
            cmd.push_constants(rhi::ShaderStage::Compute, 0, sizeof(p), &p);
            cmd.dispatch(groups(size), groups(size), 6);
        }
        cmd.barrier(maps.environment.texture, rhi::ResourceState::ShaderWrite, rhi::ResourceState::ShaderRead);
        cmd.pop_debug_group();

        // 2) diffuse irradiance.
        cmd.push_debug_group("Irradiance");
        cmd.barrier(maps.irradiance.texture, rhi::ResourceState::Undefined, rhi::ResourceState::ShaderWrite);
        cmd.bind_pipeline(irradiance);
        {
            IblPush p;
            p.src_tex = idx(maps.environment.sampled);
            p.dst_img = idx(irr_views[0]);
            p.dst_size = kIrradianceSize;
            p.sample_count = 512;
            p.src_size = face;
            p.src_mips = env_mips;
            cmd.push_constants(rhi::ShaderStage::Compute, 0, sizeof(p), &p);
            cmd.dispatch(groups(kIrradianceSize), groups(kIrradianceSize), 6);
        }
        cmd.barrier(maps.irradiance.texture, rhi::ResourceState::ShaderWrite, rhi::ResourceState::ShaderRead);
        cmd.pop_debug_group();

        // 3) GGX prefiltered specular mips.
        cmd.push_debug_group("Prefilter");
        cmd.barrier(maps.prefiltered.texture, rhi::ResourceState::Undefined, rhi::ResourceState::ShaderWrite);
        cmd.bind_pipeline(prefilter);
        for (u32 m = 0; m < maps.prefiltered.mips; ++m) {
            const u32 size = std::max(1u, maps.prefiltered.size >> m);
            IblPush p;
            p.src_tex = idx(maps.environment.sampled);
            p.dst_img = idx(pre_views[m]);
            p.dst_size = size;
            p.roughness = maps.prefiltered.mips > 1
                              ? static_cast<f32>(m) / static_cast<f32>(maps.prefiltered.mips - 1)
                              : 0.0f;
            p.sample_count = m == 0 ? 1u : 512u;
            p.src_size = face;
            p.src_mips = env_mips;
            cmd.push_constants(rhi::ShaderStage::Compute, 0, sizeof(p), &p);
            cmd.dispatch(groups(size), groups(size), 6);
        }
        cmd.barrier(maps.prefiltered.texture, rhi::ResourceState::ShaderWrite, rhi::ResourceState::ShaderRead);
        cmd.pop_debug_group();

        cmd.pop_debug_group();
    });

    release_storage_views(device, env_views);
    release_storage_views(device, irr_views);
    release_storage_views(device, pre_views);
    cleanup_source();

    maps.irradiance.sampled = device.register_texture(maps.irradiance.texture, samplers.linear_clamp);
    maps.prefiltered.sampled = device.register_texture(maps.prefiltered.texture, samplers.linear_clamp);
    if (!maps.irradiance.sampled.is_valid() || !maps.prefiltered.sampled.is_valid()) {
        destroy_environment_maps(device, maps);
        return make_error<EnvironmentMaps>(ErrorCode::Internal, "IBL descriptor registration failed");
    }
    AE_LOG_INFO(kLogCat, "environment '{}': {}x{} -> cube {} ({} mips), prefiltered {} ({} mips)",
                upload.debug_name, upload.width, upload.height, face, env_mips, maps.prefiltered.size,
                maps.prefiltered.mips);
    return maps;
}

} // namespace aether::renderer
