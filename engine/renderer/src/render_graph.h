// render_graph.h — frame graph over the RHI (blueprint §7.1).
//
// Private header. Per frame:
//   1. passes are added in execution order; each declares the virtual textures/buffers
//      it reads/writes together with the exact rhi::ResourceState it needs;
//   2. compile() (pure CPU, no GPU work - unit-tested against a mock device):
//        * dead-pass culling: a pass survives if it has side effects, writes an imported
//          resource, or writes something a surviving later pass reads (writes are treated
//          as read-modify-write, so partial writes such as one cascade layer are safe);
//        * usage derivation: each transient's rhi::TextureUsage/BufferUsage is the union
//          of its accesses;
//        * transient allocation from RGResourcePool at first use and release after last
//          use (so equal-desc transients with disjoint lifetimes alias one allocation);
//        * barrier generation from per-PHYSICAL-resource state tracking (state persists
//          across frames inside the pool), including same-state barriers for RAW/WAW
//          hazards (e.g. ColorAttachment -> ColorAttachment between two render passes);
//        * imported resources start in their declared initial state and are transitioned
//          to their final state after the last pass;
//   3. execute(cmd): per surviving pass - debug group, barriers, pass lambda.
// Main thread only.
#pragma once

#include "aether/core/types.h"
#include "aether/rhi/command_list.h"
#include "aether/rhi/device.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aether::renderer {

// ---------------------------------------------------------------------------
// Descriptors / virtual handles
// ---------------------------------------------------------------------------
struct RGTextureDesc {
    rhi::TextureType  type = rhi::TextureType::Tex2D;
    rhi::Format       format = rhi::Format::RGBA16F;
    u32               width = 1;
    u32               height = 1;
    u32               depth = 1;
    u32               mip_levels = 1;
    u32               array_layers = 1;
    rhi::TextureUsage usage = rhi::TextureUsage::None; // extra usage; graph adds derived bits

    bool operator==(const RGTextureDesc&) const = default;
};

struct RGBufferDesc {
    u64              size = 0;
    rhi::BufferUsage usage = rhi::BufferUsage::None;

    bool operator==(const RGBufferDesc&) const = default;
};

struct RGTexture {
    u32 id = kInvalidU32;
    [[nodiscard]] bool valid() const noexcept { return id != kInvalidU32; }
};
struct RGBuffer {
    u32 id = kInvalidU32;
    [[nodiscard]] bool valid() const noexcept { return id != kInvalidU32; }
};

// Derived usage bit for a state (exposed for tests).
[[nodiscard]] rhi::TextureUsage texture_usage_for_state(rhi::ResourceState s, rhi::Format f);
[[nodiscard]] rhi::BufferUsage  buffer_usage_for_state(rhi::ResourceState s);

// ---------------------------------------------------------------------------
// Physical resource pool (persists across frames)
// ---------------------------------------------------------------------------
class RGResourcePool {
public:
    struct Samplers {
        rhi::SamplerHandle linear_clamp; // colour transients
        rhi::SamplerHandle point_clamp;  // depth transients
    };

    struct Texture {
        rhi::TextureHandle      handle;
        RGTextureDesc           desc; // including final usage
        rhi::ResourceState      state = rhi::ResourceState::Undefined;
        bool                    last_access_write = false;
        rhi::DescriptorHandle   sampled;       // registered at creation if Sampled
        std::vector<rhi::DescriptorHandle> storage; // per mip, lazily registered
        u64                     last_used_frame = 0;
        bool                    in_use = false;
        bool                    alive = false;
    };
    struct Buffer {
        rhi::BufferHandle  handle;
        RGBufferDesc       desc;
        rhi::ResourceState state = rhi::ResourceState::Undefined;
        bool               last_access_write = false;
        u64                address = 0;
        u64                last_used_frame = 0;
        bool               in_use = false;
        bool               alive = false;
    };

    RGResourcePool(rhi::Device& device, Samplers samplers, u32 max_unused_frames = 4);
    ~RGResourcePool();
    RGResourcePool(const RGResourcePool&) = delete;
    RGResourcePool& operator=(const RGResourcePool&) = delete;

    // Returns a pool id; reuses a free entry with an identical desc when possible.
    u32  acquire_texture(const RGTextureDesc& desc, std::string_view debug_name);
    void release_texture(u32 id);
    u32  acquire_buffer(const RGBufferDesc& desc, std::string_view debug_name);
    void release_buffer(u32 id);

    [[nodiscard]] Texture& texture(u32 id) { return textures_[id]; }
    [[nodiscard]] Buffer&  buffer(u32 id) { return buffers_[id]; }

    u32 storage_index(u32 texture_id, u32 mip);
    u64 address(u32 buffer_id);

    // Advances the pool clock; destroys entries unused for more than max_unused_frames.
    void end_frame();
    // Destroys everything (shutdown / device loss).
    void clear();

    [[nodiscard]] u32 live_texture_count() const noexcept;
    [[nodiscard]] u32 live_buffer_count() const noexcept;
    [[nodiscard]] u64 frame() const noexcept { return frame_; }

private:
    void destroy_texture(Texture& t);
    void destroy_buffer(Buffer& b);

    rhi::Device&         device_;
    Samplers             samplers_;
    u32                  max_unused_frames_;
    u64                  frame_ = 0;
    std::vector<Texture> textures_;
    std::vector<Buffer>  buffers_;
};

// ---------------------------------------------------------------------------
// Graph
// ---------------------------------------------------------------------------
class RenderGraph;

class RGContext {
public:
    rhi::CommandList& cmd;
    rhi::Device&      device;

    [[nodiscard]] rhi::TextureHandle   texture(RGTexture t) const;
    [[nodiscard]] rhi::BufferHandle    buffer(RGBuffer b) const;
    [[nodiscard]] const RGTextureDesc& desc(RGTexture t) const;
    // Bindless index of the sampled view (default sampler: linear clamp, point for depth).
    [[nodiscard]] u32 sampled(RGTexture t) const;
    // Bindless index of the storage view of `mip`.
    [[nodiscard]] u32 storage(RGTexture t, u32 mip = 0) const;
    // Buffer device address.
    [[nodiscard]] u64 address(RGBuffer b) const;

private:
    friend class RenderGraph;
    RGContext(rhi::CommandList& c, rhi::Device& d, RenderGraph& g) : cmd(c), device(d), graph_(g) {}
    RenderGraph& graph_;
};

using RGExecuteFn = std::function<void(RGContext&)>;

class RGPassBuilder {
public:
    RGPassBuilder& read(RGTexture t, rhi::ResourceState state = rhi::ResourceState::ShaderRead);
    RGPassBuilder& write(RGTexture t, rhi::ResourceState state);
    RGPassBuilder& read(RGBuffer b, rhi::ResourceState state = rhi::ResourceState::ShaderRead);
    RGPassBuilder& write(RGBuffer b, rhi::ResourceState state = rhi::ResourceState::ShaderWrite);
    // The pass has effects the graph cannot see (e.g. GPU->CPU readback): never culled.
    RGPassBuilder& side_effect();
    RGPassBuilder& execute(RGExecuteFn fn);

private:
    friend class RenderGraph;
    RGPassBuilder(RenderGraph& g, u32 pass) : graph_(g), pass_(pass) {}
    RenderGraph& graph_;
    u32          pass_;
};

struct RGBarrier {
    bool               is_texture = true;
    rhi::TextureHandle texture;
    rhi::BufferHandle  buffer;
    rhi::ResourceState from = rhi::ResourceState::Undefined;
    rhi::ResourceState to = rhi::ResourceState::Undefined;
    std::string_view   resource_name;
};

class RenderGraph {
public:
    RenderGraph(rhi::Device& device, RGResourcePool& pool);

    // Starts a new frame (drops all passes/resources of the previous one).
    void reset();

    RGTexture create_texture(const RGTextureDesc& desc, std::string_view name);
    RGBuffer  create_buffer(const RGBufferDesc& desc, std::string_view name);

    // External resource. `initial` is its state when the graph starts, `final` the state
    // it is left in. Bindless indices are optional (needed only for ctx.sampled/storage).
    RGTexture import_texture(rhi::TextureHandle handle, const RGTextureDesc& desc,
                             rhi::ResourceState initial, rhi::ResourceState final_state,
                             std::string_view name, u32 sampled_index = kInvalidU32,
                             u32 storage_index = kInvalidU32);
    RGBuffer import_buffer(rhi::BufferHandle handle, u64 size, rhi::ResourceState initial,
                           rhi::ResourceState final_state, std::string_view name);

    RGPassBuilder add_pass(std::string_view name);

    // Culling + allocation + barrier generation. Idempotent per frame.
    void compile();
    // Records every surviving pass. compile() is called implicitly if needed.
    void execute(rhi::CommandList& cmd);

    // After compile(): the state an imported texture/buffer ends in (for callers that
    // track persistent resources).
    [[nodiscard]] rhi::ResourceState final_state(RGTexture t) const;

    void set_culling_enabled(bool enabled) noexcept { culling_enabled_ = enabled; }

    // ---- introspection (tests / debug UI) ----
    struct PassInfo {
        std::string_view       name;
        bool                   culled = false;
        std::vector<RGBarrier> barriers;
    };
    [[nodiscard]] std::vector<PassInfo> pass_infos() const;
    [[nodiscard]] const std::vector<RGBarrier>& final_barriers() const noexcept { return final_barriers_; }
    [[nodiscard]] rhi::TextureHandle physical_texture(RGTexture t) const;
    [[nodiscard]] rhi::TextureUsage  derived_usage(RGTexture t) const;
    [[nodiscard]] u32                pass_count() const noexcept { return static_cast<u32>(passes_.size()); }

private:
    friend class RGPassBuilder;
    friend class RGContext;

    struct Access {
        bool               texture = true;
        u32                id = 0;
        rhi::ResourceState state = rhi::ResourceState::Undefined;
        bool               write = false;
    };
    struct Pass {
        std::string            name;
        std::vector<Access>    accesses;
        RGExecuteFn            fn;
        bool                   side_effect = false;
        bool                   culled = false;
        std::vector<RGBarrier> barriers;
    };
    struct TextureRes {
        std::string        name;
        RGTextureDesc      desc;
        bool               imported = false;
        rhi::TextureHandle handle;          // imported, or resolved physical
        rhi::ResourceState initial = rhi::ResourceState::Undefined;
        rhi::ResourceState final_state = rhi::ResourceState::Undefined;
        u32                sampled_index = kInvalidU32;
        u32                storage_index = kInvalidU32;
        u32                pool_id = kInvalidU32;
        u32                first_pass = kInvalidU32;
        u32                last_pass = kInvalidU32;
        rhi::ResourceState state = rhi::ResourceState::Undefined; // imported tracking
        bool               last_write = false;
    };
    struct BufferRes {
        std::string        name;
        RGBufferDesc       desc;
        bool               imported = false;
        rhi::BufferHandle  handle;
        rhi::ResourceState initial = rhi::ResourceState::Undefined;
        rhi::ResourceState final_state = rhi::ResourceState::Undefined;
        u32                pool_id = kInvalidU32;
        u32                first_pass = kInvalidU32;
        u32                last_pass = kInvalidU32;
        rhi::ResourceState state = rhi::ResourceState::Undefined;
        bool               last_write = false;
        u64                address = 0;
    };

    void add_access(u32 pass, const Access& a);
    void cull_passes();
    void derive_usage_and_lifetimes();
    void allocate_and_build_barriers();

    rhi::Device&             device_;
    RGResourcePool&          pool_;
    std::vector<Pass>        passes_;
    std::vector<TextureRes>  textures_;
    std::vector<BufferRes>   buffers_;
    std::vector<RGBarrier>   final_barriers_;
    bool                     compiled_ = false;
    bool                     culling_enabled_ = true;
};

} // namespace aether::renderer
