// render_graph.cpp — frame graph compile/execute (see render_graph.h).
#include "render_graph.h"

#include "format_utils.h"

#include "aether/core/error.h"
#include "aether/core/log.h"

#include <algorithm>

namespace aether::renderer {

namespace {
constexpr const char* kLogCat = "RenderGraph";

u32 descriptor_index(rhi::DescriptorHandle h) {
    return h.is_valid() ? h.index() : kInvalidU32;
}
} // namespace

rhi::TextureUsage texture_usage_for_state(rhi::ResourceState s, rhi::Format f) {
    using rhi::ResourceState;
    using rhi::TextureUsage;
    switch (s) {
    case ResourceState::ColorAttachment: return TextureUsage::ColorAttach;
    case ResourceState::DepthStencilAttachment: return TextureUsage::DepthAttach;
    case ResourceState::DepthStencilRead: return TextureUsage::DepthAttach | TextureUsage::Sampled;
    case ResourceState::ShaderRead: return TextureUsage::Sampled;
    case ResourceState::ShaderWrite:
    case ResourceState::General:
        return is_depth_format(f) ? TextureUsage::None : TextureUsage::Storage;
    case ResourceState::TransferSrc: return TextureUsage::TransferSrc;
    case ResourceState::TransferDst: return TextureUsage::TransferDst;
    default: return TextureUsage::None;
    }
}

rhi::BufferUsage buffer_usage_for_state(rhi::ResourceState s) {
    using rhi::BufferUsage;
    using rhi::ResourceState;
    switch (s) {
    case ResourceState::ShaderRead:
    case ResourceState::ShaderWrite:
    case ResourceState::General: return BufferUsage::Storage;
    case ResourceState::IndirectArgument: return BufferUsage::Indirect;
    case ResourceState::TransferSrc: return BufferUsage::TransferSrc;
    case ResourceState::TransferDst: return BufferUsage::TransferDst;
    default: return BufferUsage::None;
    }
}

// ===========================================================================
// RGResourcePool
// ===========================================================================
RGResourcePool::RGResourcePool(rhi::Device& device, Samplers samplers, u32 max_unused_frames)
    : device_(device), samplers_(samplers), max_unused_frames_(std::max(1u, max_unused_frames)) {}

RGResourcePool::~RGResourcePool() { clear(); }

u32 RGResourcePool::acquire_texture(const RGTextureDesc& desc, std::string_view debug_name) {
    for (u32 i = 0; i < textures_.size(); ++i) {
        Texture& t = textures_[i];
        if (t.alive && !t.in_use && t.desc == desc) {
            t.in_use = true;
            t.last_used_frame = frame_;
            return i;
        }
    }
    u32 id = kInvalidU32;
    for (u32 i = 0; i < textures_.size(); ++i) {
        if (!textures_[i].alive) {
            id = i;
            break;
        }
    }
    if (id == kInvalidU32) {
        id = static_cast<u32>(textures_.size());
        textures_.emplace_back();
    }
    Texture& t = textures_[id];
    rhi::TextureDesc td;
    td.type = desc.type;
    td.format = desc.format;
    td.width = std::max(1u, desc.width);
    td.height = std::max(1u, desc.height);
    td.depth = std::max(1u, desc.depth);
    td.mip_levels = std::max(1u, desc.mip_levels);
    td.array_layers = std::max(1u, desc.array_layers);
    td.usage = desc.usage;
    td.debug_name = std::string(debug_name);
    t = Texture{};
    t.handle = device_.create_texture(td);
    t.desc = desc;
    t.state = rhi::ResourceState::Undefined;
    t.alive = true;
    t.in_use = true;
    t.last_used_frame = frame_;
    t.storage.assign(td.mip_levels, rhi::DescriptorHandle{});
    if (t.handle.is_valid() && any(desc.usage & rhi::TextureUsage::Sampled)) {
        const rhi::SamplerHandle s =
            is_depth_format(desc.format) ? samplers_.point_clamp : samplers_.linear_clamp;
        t.sampled = device_.register_texture(t.handle, s);
    }
    if (!t.handle.is_valid()) {
        AE_LOG_ERROR(kLogCat, "failed to create transient texture '{}' ({}x{})", debug_name,
                     td.width, td.height);
    }
    return id;
}

void RGResourcePool::release_texture(u32 id) {
    if (id < textures_.size()) {
        textures_[id].in_use = false;
        textures_[id].last_used_frame = frame_;
    }
}

u32 RGResourcePool::acquire_buffer(const RGBufferDesc& desc, std::string_view debug_name) {
    for (u32 i = 0; i < buffers_.size(); ++i) {
        Buffer& b = buffers_[i];
        if (b.alive && !b.in_use && b.desc == desc) {
            b.in_use = true;
            b.last_used_frame = frame_;
            return i;
        }
    }
    u32 id = kInvalidU32;
    for (u32 i = 0; i < buffers_.size(); ++i) {
        if (!buffers_[i].alive) {
            id = i;
            break;
        }
    }
    if (id == kInvalidU32) {
        id = static_cast<u32>(buffers_.size());
        buffers_.emplace_back();
    }
    Buffer& b = buffers_[id];
    rhi::BufferDesc bd;
    bd.size = std::max<u64>(desc.size, 16);
    bd.usage = desc.usage;
    bd.memory = rhi::MemoryUsage::GpuOnly;
    bd.debug_name = std::string(debug_name);
    b = Buffer{};
    b.handle = device_.create_buffer(bd);
    b.desc = desc;
    b.alive = true;
    b.in_use = true;
    b.last_used_frame = frame_;
    if (!b.handle.is_valid()) {
        AE_LOG_ERROR(kLogCat, "failed to create transient buffer '{}' ({} bytes)", debug_name, bd.size);
    }
    return id;
}

void RGResourcePool::release_buffer(u32 id) {
    if (id < buffers_.size()) {
        buffers_[id].in_use = false;
        buffers_[id].last_used_frame = frame_;
    }
}

u32 RGResourcePool::storage_index(u32 texture_id, u32 mip) {
    if (texture_id >= textures_.size()) {
        return kInvalidU32;
    }
    Texture& t = textures_[texture_id];
    if (!t.handle.is_valid() || mip >= t.storage.size() ||
        !any(t.desc.usage & rhi::TextureUsage::Storage)) {
        return kInvalidU32;
    }
    if (!t.storage[mip].is_valid()) {
        t.storage[mip] = device_.register_storage_texture(t.handle, mip);
    }
    return descriptor_index(t.storage[mip]);
}

u64 RGResourcePool::address(u32 buffer_id) {
    if (buffer_id >= buffers_.size()) {
        return 0;
    }
    Buffer& b = buffers_[buffer_id];
    if (b.address == 0 && b.handle.is_valid()) {
        b.address = device_.buffer_device_address(b.handle);
    }
    return b.address;
}

void RGResourcePool::destroy_texture(Texture& t) {
    if (t.sampled.is_valid()) {
        device_.unregister_texture(t.sampled);
    }
    for (rhi::DescriptorHandle& s : t.storage) {
        if (s.is_valid()) {
            device_.unregister_storage_texture(s);
        }
    }
    if (t.handle.is_valid()) {
        device_.destroy(t.handle);
    }
    t = Texture{};
}

void RGResourcePool::destroy_buffer(Buffer& b) {
    if (b.handle.is_valid()) {
        device_.destroy(b.handle);
    }
    b = Buffer{};
}

void RGResourcePool::end_frame() {
    ++frame_;
    for (Texture& t : textures_) {
        if (t.alive && !t.in_use && frame_ - t.last_used_frame > max_unused_frames_) {
            destroy_texture(t);
        }
    }
    for (Buffer& b : buffers_) {
        if (b.alive && !b.in_use && frame_ - b.last_used_frame > max_unused_frames_) {
            destroy_buffer(b);
        }
    }
}

void RGResourcePool::clear() {
    for (Texture& t : textures_) {
        if (t.alive) {
            destroy_texture(t);
        }
    }
    for (Buffer& b : buffers_) {
        if (b.alive) {
            destroy_buffer(b);
        }
    }
    textures_.clear();
    buffers_.clear();
}

u32 RGResourcePool::live_texture_count() const noexcept {
    return static_cast<u32>(
        std::count_if(textures_.begin(), textures_.end(), [](const Texture& t) { return t.alive; }));
}
u32 RGResourcePool::live_buffer_count() const noexcept {
    return static_cast<u32>(
        std::count_if(buffers_.begin(), buffers_.end(), [](const Buffer& b) { return b.alive; }));
}

// ===========================================================================
// RGContext
// ===========================================================================
rhi::TextureHandle RGContext::texture(RGTexture t) const {
    return t.valid() ? graph_.textures_[t.id].handle : rhi::TextureHandle{};
}
rhi::BufferHandle RGContext::buffer(RGBuffer b) const {
    return b.valid() ? graph_.buffers_[b.id].handle : rhi::BufferHandle{};
}
const RGTextureDesc& RGContext::desc(RGTexture t) const { return graph_.textures_[t.id].desc; }

u32 RGContext::sampled(RGTexture t) const {
    if (!t.valid()) {
        return kInvalidU32;
    }
    const auto& r = graph_.textures_[t.id];
    if (r.imported) {
        return r.sampled_index;
    }
    return r.pool_id == kInvalidU32 ? kInvalidU32
                                    : descriptor_index(graph_.pool_.texture(r.pool_id).sampled);
}

u32 RGContext::storage(RGTexture t, u32 mip) const {
    if (!t.valid()) {
        return kInvalidU32;
    }
    const auto& r = graph_.textures_[t.id];
    if (r.imported) {
        return mip == 0 ? r.storage_index : kInvalidU32;
    }
    return r.pool_id == kInvalidU32 ? kInvalidU32 : graph_.pool_.storage_index(r.pool_id, mip);
}

u64 RGContext::address(RGBuffer b) const {
    if (!b.valid()) {
        return 0;
    }
    const auto& r = graph_.buffers_[b.id];
    if (r.imported) {
        return r.address;
    }
    return r.pool_id == kInvalidU32 ? 0 : graph_.pool_.address(r.pool_id);
}

// ===========================================================================
// RGPassBuilder
// ===========================================================================
RGPassBuilder& RGPassBuilder::read(RGTexture t, rhi::ResourceState state) {
    if (t.valid()) {
        graph_.add_access(pass_, { true, t.id, state, false });
    }
    return *this;
}
RGPassBuilder& RGPassBuilder::write(RGTexture t, rhi::ResourceState state) {
    if (t.valid()) {
        graph_.add_access(pass_, { true, t.id, state, true });
    }
    return *this;
}
RGPassBuilder& RGPassBuilder::read(RGBuffer b, rhi::ResourceState state) {
    if (b.valid()) {
        graph_.add_access(pass_, { false, b.id, state, false });
    }
    return *this;
}
RGPassBuilder& RGPassBuilder::write(RGBuffer b, rhi::ResourceState state) {
    if (b.valid()) {
        graph_.add_access(pass_, { false, b.id, state, true });
    }
    return *this;
}
RGPassBuilder& RGPassBuilder::side_effect() {
    graph_.passes_[pass_].side_effect = true;
    return *this;
}
RGPassBuilder& RGPassBuilder::execute(RGExecuteFn fn) {
    graph_.passes_[pass_].fn = std::move(fn);
    return *this;
}

// ===========================================================================
// RenderGraph
// ===========================================================================
RenderGraph::RenderGraph(rhi::Device& device, RGResourcePool& pool) : device_(device), pool_(pool) {}

void RenderGraph::reset() {
    passes_.clear();
    textures_.clear();
    buffers_.clear();
    final_barriers_.clear();
    compiled_ = false;
}

RGTexture RenderGraph::create_texture(const RGTextureDesc& desc, std::string_view name) {
    TextureRes r;
    r.name = std::string(name);
    r.desc = desc;
    textures_.push_back(std::move(r));
    return RGTexture{ static_cast<u32>(textures_.size() - 1) };
}

RGBuffer RenderGraph::create_buffer(const RGBufferDesc& desc, std::string_view name) {
    BufferRes r;
    r.name = std::string(name);
    r.desc = desc;
    buffers_.push_back(std::move(r));
    return RGBuffer{ static_cast<u32>(buffers_.size() - 1) };
}

RGTexture RenderGraph::import_texture(rhi::TextureHandle handle, const RGTextureDesc& desc,
                                      rhi::ResourceState initial, rhi::ResourceState final_state,
                                      std::string_view name, u32 sampled_index,
                                      u32 storage_index) {
    TextureRes r;
    r.name = std::string(name);
    r.desc = desc;
    r.imported = true;
    r.handle = handle;
    r.initial = initial;
    r.final_state = final_state;
    r.sampled_index = sampled_index;
    r.storage_index = storage_index;
    textures_.push_back(std::move(r));
    return RGTexture{ static_cast<u32>(textures_.size() - 1) };
}

RGBuffer RenderGraph::import_buffer(rhi::BufferHandle handle, u64 size, rhi::ResourceState initial,
                                    rhi::ResourceState final_state, std::string_view name) {
    BufferRes r;
    r.name = std::string(name);
    r.desc.size = size;
    r.imported = true;
    r.handle = handle;
    r.initial = initial;
    r.final_state = final_state;
    r.address = handle.is_valid() ? device_.buffer_device_address(handle) : 0;
    buffers_.push_back(std::move(r));
    return RGBuffer{ static_cast<u32>(buffers_.size() - 1) };
}

RGPassBuilder RenderGraph::add_pass(std::string_view name) {
    AE_ASSERT(!compiled_);
    Pass p;
    p.name = std::string(name);
    passes_.push_back(std::move(p));
    return RGPassBuilder(*this, static_cast<u32>(passes_.size() - 1));
}

void RenderGraph::add_access(u32 pass, const Access& a) {
    Pass& p = passes_[pass];
    for (Access& existing : p.accesses) {
        if (existing.texture == a.texture && existing.id == a.id) {
            if (existing.state != a.state) {
                AE_LOG_ERROR(kLogCat, "pass '{}' uses '{}' in two states ({} and {}); keeping the first",
                             p.name, a.texture ? textures_[a.id].name : buffers_[a.id].name,
                             state_name(existing.state), state_name(a.state));
                AE_ASSERT(false);
                return;
            }
            existing.write = existing.write || a.write;
            return;
        }
    }
    p.accesses.push_back(a);
}

void RenderGraph::cull_passes() {
    std::vector<bool> tex_needed(textures_.size(), false);
    std::vector<bool> buf_needed(buffers_.size(), false);
    for (usize i = 0; i < textures_.size(); ++i) {
        tex_needed[i] = textures_[i].imported;
    }
    for (usize i = 0; i < buffers_.size(); ++i) {
        buf_needed[i] = buffers_[i].imported;
    }
    for (usize pi = passes_.size(); pi-- > 0;) {
        Pass& p = passes_[pi];
        bool alive = p.side_effect || !culling_enabled_;
        for (const Access& a : p.accesses) {
            if (a.write && (a.texture ? tex_needed[a.id] : buf_needed[a.id])) {
                alive = true;
            }
        }
        p.culled = !alive;
        if (!alive) {
            continue;
        }
        // Reads need their producers; writes are read-modify-write (partial writes).
        for (const Access& a : p.accesses) {
            if (a.texture) {
                tex_needed[a.id] = true;
            } else {
                buf_needed[a.id] = true;
            }
        }
    }
}

void RenderGraph::derive_usage_and_lifetimes() {
    for (u32 pi = 0; pi < passes_.size(); ++pi) {
        const Pass& p = passes_[pi];
        if (p.culled) {
            continue;
        }
        for (const Access& a : p.accesses) {
            if (a.texture) {
                TextureRes& r = textures_[a.id];
                if (r.first_pass == kInvalidU32) {
                    r.first_pass = pi;
                }
                r.last_pass = pi;
                if (!r.imported) {
                    r.desc.usage |= texture_usage_for_state(a.state, r.desc.format);
                }
            } else {
                BufferRes& r = buffers_[a.id];
                if (r.first_pass == kInvalidU32) {
                    r.first_pass = pi;
                }
                r.last_pass = pi;
                if (!r.imported) {
                    r.desc.usage |= buffer_usage_for_state(a.state);
                }
            }
        }
    }
}

void RenderGraph::allocate_and_build_barriers() {
    for (TextureRes& r : textures_) {
        if (r.imported) {
            r.state = r.initial;
            r.last_write = is_write_state(r.initial);
        }
    }
    for (BufferRes& r : buffers_) {
        if (r.imported) {
            r.state = r.initial;
            r.last_write = is_write_state(r.initial);
        }
    }

    for (u32 pi = 0; pi < passes_.size(); ++pi) {
        Pass& p = passes_[pi];
        p.barriers.clear();
        if (p.culled) {
            continue;
        }
        // Allocate transients whose lifetime starts here.
        for (const Access& a : p.accesses) {
            if (a.texture) {
                TextureRes& r = textures_[a.id];
                if (!r.imported && r.first_pass == pi && r.pool_id == kInvalidU32) {
                    r.pool_id = pool_.acquire_texture(r.desc, r.name);
                    r.handle = pool_.texture(r.pool_id).handle;
                }
            } else {
                BufferRes& r = buffers_[a.id];
                if (!r.imported && r.first_pass == pi && r.pool_id == kInvalidU32) {
                    r.pool_id = pool_.acquire_buffer(r.desc, r.name);
                    r.handle = pool_.buffer(r.pool_id).handle;
                }
            }
        }
        // Transitions / hazards.
        for (const Access& a : p.accesses) {
            rhi::ResourceState* state = nullptr;
            bool*               last_write = nullptr;
            RGBarrier           b;
            if (a.texture) {
                TextureRes& r = textures_[a.id];
                if (r.imported) {
                    state = &r.state;
                    last_write = &r.last_write;
                } else {
                    auto& pt = pool_.texture(r.pool_id);
                    state = &pt.state;
                    last_write = &pt.last_access_write;
                }
                b.is_texture = true;
                b.texture = r.handle;
                b.resource_name = r.name;
            } else {
                BufferRes& r = buffers_[a.id];
                if (r.imported) {
                    state = &r.state;
                    last_write = &r.last_write;
                } else {
                    auto& pb = pool_.buffer(r.pool_id);
                    state = &pb.state;
                    last_write = &pb.last_access_write;
                }
                b.is_texture = false;
                b.buffer = r.handle;
                b.resource_name = r.name;
            }
            if (*state != a.state || *last_write || a.write) {
                b.from = *state;
                b.to = a.state;
                p.barriers.push_back(b);
            }
            *state = a.state;
            *last_write = a.write;
        }
        // Return transients whose lifetime ends here (aliasing for later passes).
        for (const Access& a : p.accesses) {
            if (a.texture) {
                TextureRes& r = textures_[a.id];
                if (!r.imported && r.last_pass == pi && r.pool_id != kInvalidU32) {
                    pool_.release_texture(r.pool_id);
                }
            } else {
                BufferRes& r = buffers_[a.id];
                if (!r.imported && r.last_pass == pi && r.pool_id != kInvalidU32) {
                    pool_.release_buffer(r.pool_id);
                }
            }
        }
    }

    // Imported resources end in their declared final state; a trailing write also gets a
    // same-state barrier so whoever consumes the resource next sees our writes.
    final_barriers_.clear();
    for (TextureRes& r : textures_) {
        if (!r.imported || r.final_state == rhi::ResourceState::Undefined) {
            continue;
        }
        if (r.state != r.final_state || (r.last_write && r.first_pass != kInvalidU32)) {
            RGBarrier b;
            b.is_texture = true;
            b.texture = r.handle;
            b.from = r.state;
            b.to = r.final_state;
            b.resource_name = r.name;
            final_barriers_.push_back(b);
            r.state = r.final_state;
            r.last_write = false;
        }
    }
    for (BufferRes& r : buffers_) {
        if (!r.imported || r.final_state == rhi::ResourceState::Undefined) {
            continue;
        }
        if (r.state != r.final_state || (r.last_write && r.first_pass != kInvalidU32)) {
            RGBarrier b;
            b.is_texture = false;
            b.buffer = r.handle;
            b.from = r.state;
            b.to = r.final_state;
            b.resource_name = r.name;
            final_barriers_.push_back(b);
            r.state = r.final_state;
            r.last_write = false;
        }
    }
}

void RenderGraph::compile() {
    if (compiled_) {
        return;
    }
    cull_passes();
    derive_usage_and_lifetimes();
    allocate_and_build_barriers();
    compiled_ = true;
}

void RenderGraph::execute(rhi::CommandList& cmd) {
    compile();
    RGContext ctx(cmd, device_, *this);
    for (Pass& p : passes_) {
        if (p.culled) {
            continue;
        }
        cmd.push_debug_group(p.name.c_str());
        for (const RGBarrier& b : p.barriers) {
            if (b.is_texture) {
                cmd.barrier(b.texture, b.from, b.to);
            } else {
                cmd.barrier(b.buffer, b.from, b.to);
            }
        }
        if (p.fn) {
            p.fn(ctx);
        }
        cmd.pop_debug_group();
    }
    for (const RGBarrier& b : final_barriers_) {
        if (b.is_texture) {
            cmd.barrier(b.texture, b.from, b.to);
        } else {
            cmd.barrier(b.buffer, b.from, b.to);
        }
    }
}

rhi::ResourceState RenderGraph::final_state(RGTexture t) const {
    if (!t.valid()) {
        return rhi::ResourceState::Undefined;
    }
    const TextureRes& r = textures_[t.id];
    return r.imported ? r.state : rhi::ResourceState::Undefined;
}

std::vector<RenderGraph::PassInfo> RenderGraph::pass_infos() const {
    std::vector<PassInfo> out;
    out.reserve(passes_.size());
    for (const Pass& p : passes_) {
        out.push_back(PassInfo{ p.name, p.culled, p.barriers });
    }
    return out;
}

rhi::TextureHandle RenderGraph::physical_texture(RGTexture t) const {
    return t.valid() ? textures_[t.id].handle : rhi::TextureHandle{};
}

rhi::TextureUsage RenderGraph::derived_usage(RGTexture t) const {
    return t.valid() ? textures_[t.id].desc.usage : rhi::TextureUsage::None;
}

} // namespace aether::renderer
