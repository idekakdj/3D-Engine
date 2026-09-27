// aether/rhi/device.h — the RHI device: resource factory, swapchain, frame + submit.
//
// FROZEN CONTRACT (ADR-0001/0002 / blueprint §6). The Vulkan backend implements this;
// no Vk* type appears here. Create via rhi::create_device(). ImGui hooks live in imgui.h.
//
// Binding model (ADR-0002) - every pipeline shares ONE universal pipeline layout:
//   set 0, binding 0 : sampled textures  - combined image sampler array (bindless,
//                       PARTIALLY_BOUND | UPDATE_AFTER_BIND). Index = DescriptorHandle::index().
//                       Shaders may alias it as sampler2D[] / samplerCube[] /
//                       sampler2DArray[] / sampler2DShadow[] - the view type must match.
//   set 0, binding 1 : storage images    - storage image array (bindless), same rules.
//   push constants   : 128 bytes, VK_SHADER_STAGE_ALL.
//   buffers          : NOT bound - accessed via buffer_device_address() (GL_EXT_buffer_reference),
//                       passed in push constants or inside other buffers.
//
// Swapchain state contract (ADR-0002):
//   begin_frame(): swapchain image is in ResourceState::Undefined.
//   end_frame():   caller must leave it in ResourceState::ColorAttachment; the device
//                  performs ColorAttachment -> Present. Swapchain format is BGRA8Unorm
//                  (display encoding is done in-shader - see renderer RenderTarget).
//
// Lifetime: destroy()/unregister_*() are deferred internally by frames_in_flight() frames,
// so it is always safe to call them right after the last use was recorded.
#pragma once

#include "aether/core/error.h"
#include "aether/core/math.h"
#include "aether/core/types.h"
#include "aether/rhi/command_list.h"
#include "aether/rhi/resources.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aether {
class Window;
}

namespace aether::rhi {

// Optional GPU features, queried and gated (blueprint §6.3 feature tiers).
struct DeviceFeatures {
    bool dynamic_rendering = false;
    bool timeline_semaphores = false;
    bool synchronization2 = false;
    bool descriptor_indexing = false; // bindless
    bool buffer_device_address = false;
    bool draw_indirect_count = false;
    bool draw_indirect_first_instance = false; // ADR-0009 (additive): non-zero firstInstance in indirect draws
    bool mesh_shaders = false;
    bool ray_tracing = false;
    bool wide_lines = false;          // false on Intel Arc (ADR-0001)
    bool fill_mode_non_solid = false; // gate wireframe on this
    bool depth_clamp = false;
    bool sampler_anisotropy = false;
    bool texture_compression_bc = false; // ADR-0009 (additive): BC1..BC7 sampled textures
    u32  max_bindless_textures = 0;
    u32  max_push_constant_bytes = 128;
    std::string adapter_name;
    std::string driver_version;
};

struct DeviceDesc {
    Window* window = nullptr;
    bool    enable_validation = true; // best-effort; skipped (with a warning) if layer absent
    bool    vsync = true;
    u32     frames_in_flight = 2;     // CPU/GPU overlap; independent of swapchain image count
};

// Per-frame recording context returned by begin_frame().
struct FrameInfo {
    CommandList*  cmd = nullptr;        // primary command list for this frame
    TextureHandle swapchain_image;      // current backbuffer as an RHI texture
    Format        swapchain_format = Format::BGRA8Unorm;
    UVec2         extent{ 0, 0 };
    u64           frame_index = 0;      // monotonic
    u32           frame_slot = 0;       // frame_index % frames_in_flight (per-frame ring index)
    u32           image_index = 0;
    bool          valid = false;        // false => skip rendering (minimized / out-of-date)
};

// GPU timing / stats surfaced to the profiler.
struct FrameStats {
    f64 gpu_time_ms = 0.0;
    u32 draw_calls = 0;
    u32 dispatches = 0;
    u32 pipeline_binds = 0;
};

class Device {
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual const DeviceFeatures& features() const = 0;
    [[nodiscard]] virtual u32 frames_in_flight() const = 0;

    // ---- resource lifetime ----
    virtual BufferHandle   create_buffer(const BufferDesc&) = 0;
    virtual TextureHandle  create_texture(const TextureDesc&) = 0;
    virtual SamplerHandle  create_sampler(const SamplerDesc&) = 0;
    virtual ShaderHandle   create_shader(const ShaderDesc&) = 0;
    virtual PipelineHandle create_graphics_pipeline(const GraphicsPipelineDesc&) = 0;
    virtual PipelineHandle create_compute_pipeline(const ComputePipelineDesc&) = 0;

    virtual void destroy(BufferHandle) = 0;
    virtual void destroy(TextureHandle) = 0;
    virtual void destroy(SamplerHandle) = 0;
    virtual void destroy(ShaderHandle) = 0;
    virtual void destroy(PipelineHandle) = 0;

    [[nodiscard]] virtual const TextureDesc* texture_desc(TextureHandle) const = 0; // null if invalid
    [[nodiscard]] virtual const BufferDesc*  buffer_desc(BufferHandle) const = 0;

    // GPU virtual address for GL_EXT_buffer_reference access (any buffer usage).
    [[nodiscard]] virtual u64 buffer_device_address(BufferHandle) = 0;

    // ---- data transfer (callable any time on the main thread; data is visible to all
    //      GPU work submitted by the next end_frame / submit) ----
    virtual void  update_buffer(BufferHandle, ByteSpan data, u64 dst_offset = 0) = 0;
    virtual void* map(BufferHandle) = 0;   // CpuToGpu / GpuToCpu / CpuOnly only; persistent
    virtual void  unmap(BufferHandle) = 0;
    // ADR-0009 (additive): before the CPU reads a mapped GpuToCpu buffer that GPU work wrote,
    // make those writes visible (invalidates non-coherent memory; no-op when coherent). The
    // writing frame must have completed (e.g. frames_in_flight() begin_frame()s later).
    virtual void  invalidate_mapped(BufferHandle) {}
    // Mip 0 of every layer, tightly packed; generates the remaining mips if requested.
    // Leaves the texture in ResourceState::ShaderRead.
    virtual void  update_texture(TextureHandle, ByteSpan data, bool gen_mips = true) = 0;
    virtual void  update_texture_mip(TextureHandle, u32 mip, u32 layer, ByteSpan data) = 0;

    // ---- bindless registration (returned index is used directly in shaders) ----
    virtual DescriptorHandle register_texture(TextureHandle, SamplerHandle) = 0;         // binding 0
    virtual DescriptorHandle register_storage_texture(TextureHandle, u32 mip = 0) = 0;   // binding 1
    virtual void             unregister_texture(DescriptorHandle) = 0;
    virtual void             unregister_storage_texture(DescriptorHandle) = 0;

    // ---- frame lifecycle ----
    // Waits for this frame slot's previous GPU work, acquires the next backbuffer and
    // begins the primary command list. valid=false => skip rendering this frame.
    virtual FrameInfo begin_frame() = 0;
    // Submits recorded work and presents. Handles OUT_OF_DATE/SUBOPTIMAL by recreating
    // the swapchain (never asserts on them).
    virtual void      end_frame(const FrameInfo&) = 0;
    // React to a window resize (recreate swapchain). Safe to call redundantly.
    virtual void      on_resize(u32 width, u32 height) = 0;
    // Block until the GPU is idle (shutdown / resource teardown).
    virtual void      wait_idle() = 0;

    // Record + submit + wait a one-off command list (init-time uploads, IBL precompute).
    virtual void      immediate_submit(const std::function<void(CommandList&)>& record) = 0;

    [[nodiscard]] virtual FrameStats last_frame_stats() const = 0;

    // ---- shader compilation (runtime glslang; blueprint §7.4). Never throws. ----
    virtual Result<std::vector<u32>> compile_glsl(ShaderStage stage, StringView source,
                                                  StringView name = "inline",
                                                  const ShaderCompileOptions& options = {}) = 0;
    virtual Result<std::vector<u32>> compile_glsl_file(ShaderStage stage,
                                                       const std::filesystem::path& file,
                                                       const ShaderCompileOptions& options = {}) = 0;
};

// Factory. Creates the Vulkan backend device bound to the window's surface.
Result<std::unique_ptr<Device>> create_device(const DeviceDesc& desc);

} // namespace aether::rhi
