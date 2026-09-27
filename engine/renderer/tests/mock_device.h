// mock_device.h — GPU-free rhi::Device / rhi::CommandList for renderer unit tests.
//
// Hands out sequential handles, keeps descs, and VALIDATES command streams: every texture
// barrier's `from` must equal the tracked state (or Undefined = discard), attachments
// must be in ColorAttachment/DepthStencilAttachment inside begin_rendering, rendering
// scopes and debug groups must balance, and draws must happen with a bound pipeline.
// Shader compilation forwards to the real device-free glslang wrapper.
#pragma once

#include "aether/rhi/device.h"
#include "aether/rhi/shader_compiler.h"

#include <format>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether::renderer::test {

struct MockMipUpload {
    u32 texture = 0, mip = 0, layer = 0;
    u64 bytes = 0;
};

struct MockState {
    std::unordered_map<u32, rhi::ResourceState> texture_state;
    std::unordered_map<u32, rhi::TextureDesc>   textures;
    std::unordered_map<u32, rhi::BufferDesc>    buffers;
    std::unordered_map<u32, std::vector<byte>>  mapped;
    std::vector<std::string>                    errors;
    std::vector<std::string>                    log; // "barrier <tex> A->B", "group <name>", ...
    std::vector<MockMipUpload>                  mip_uploads;
    u32 indirect_count_draws = 0, invalidations = 0;
    u32 mesh_task_draws = 0, mesh_pipelines_created = 0; // ADR-0010
    u32 textures_created = 0, textures_destroyed = 0;
    u32 buffers_created = 0, buffers_destroyed = 0;
    u32 pipelines_created = 0, draws = 0, dispatches = 0;
};

class MockCommandList final : public rhi::CommandList {
public:
    explicit MockCommandList(MockState& s) : s_(s) {}

    void begin_rendering(const rhi::RenderingInfo& info) override {
        if (in_rendering_) {
            err("nested begin_rendering");
        }
        in_rendering_ = true;
        for (const auto& c : info.color) {
            expect_state(c.texture, rhi::ResourceState::ColorAttachment, "color attachment");
        }
        if (info.has_depth) {
            expect_state(info.depth.texture, rhi::ResourceState::DepthStencilAttachment, "depth attachment");
        }
        if (info.render_area.x == 0 || info.render_area.y == 0) {
            err("empty render area");
        }
    }
    void end_rendering() override {
        if (!in_rendering_) {
            err("end_rendering without begin");
        }
        in_rendering_ = false;
    }
    void set_viewport(const rhi::Viewport&) override {}
    void set_scissor(const rhi::Scissor&) override {}
    void bind_pipeline(rhi::PipelineHandle p) override {
        if (!p.is_valid()) {
            err("bind of invalid pipeline");
        }
        pipeline_bound_ = true;
    }
    void push_constants(rhi::ShaderStage, u32 offset, u32 size, const void*) override {
        if (offset + size > 128) {
            err("push constants exceed 128 bytes");
        }
    }
    void bind_vertex_buffer(u32, rhi::BufferHandle b, u64) override { check_buffer(b); }
    void bind_index_buffer(rhi::BufferHandle b, u64, bool) override { check_buffer(b); }
    void draw(u32, u32, u32, u32) override { on_draw(); }
    void draw_indexed(u32, u32, u32, i32, u32) override { on_draw(); }
    void draw_indexed_indirect(rhi::BufferHandle, u64, u32, u32) override { on_draw(); }
    void draw_indexed_indirect_count(rhi::BufferHandle a, u64, rhi::BufferHandle c, u64, u32, u32) override {
        check_buffer(a);
        check_buffer(c);
        ++s_.indirect_count_draws;
        on_draw();
    }
    void draw_mesh_tasks_indirect_count(rhi::BufferHandle a, u64, rhi::BufferHandle c, u64, u32, u32 stride) override {
        check_buffer(a);
        check_buffer(c);
        if (stride != 20) {
            err("mesh task command stride must be 20 (GpuMeshTaskCommand)");
        }
        ++s_.mesh_task_draws;
        on_draw();
    }
    void dispatch(u32, u32, u32) override {
        if (in_rendering_) {
            err("dispatch inside rendering scope");
        }
        if (!pipeline_bound_) {
            err("dispatch without pipeline");
        }
        ++s_.dispatches;
    }
    void dispatch_indirect(rhi::BufferHandle, u64) override { ++s_.dispatches; }
    void set_depth_bias(f32, f32, f32) override {}
    void fill_buffer(rhi::BufferHandle b, u64, u64, u32) override { check_buffer(b); }
    void barrier(rhi::TextureHandle t, rhi::ResourceState from, rhi::ResourceState to) override {
        if (in_rendering_) {
            err("barrier inside rendering scope");
        }
        if (!s_.textures.contains(t.value)) {
            err(std::format("barrier on unknown texture {}", t.value));
        }
        const auto it = s_.texture_state.find(t.value);
        const rhi::ResourceState cur = it == s_.texture_state.end() ? rhi::ResourceState::Undefined : it->second;
        if (from != rhi::ResourceState::Undefined && from != cur) {
            err(std::format("texture {} barrier from {} but tracked state is {}", t.value,
                            static_cast<int>(from), static_cast<int>(cur)));
        }
        s_.texture_state[t.value] = to;
        s_.log.push_back(std::format("barrier {} {}->{}", t.value, static_cast<int>(from), static_cast<int>(to)));
    }
    void barrier(rhi::BufferHandle b, rhi::ResourceState, rhi::ResourceState) override {
        if (in_rendering_) {
            err("buffer barrier inside rendering scope");
        }
        check_buffer(b);
    }
    void copy_buffer(rhi::BufferHandle a, rhi::BufferHandle b, u64, u64, u64) override {
        check_buffer(a);
        check_buffer(b);
    }
    void copy_buffer_to_texture(rhi::BufferHandle, rhi::TextureHandle, u32) override {}
    void push_debug_group(const char* name) override {
        ++depth_;
        s_.log.push_back(std::string("group ") + name);
    }
    void pop_debug_group() override {
        if (depth_ == 0) {
            err("unbalanced pop_debug_group");
        } else {
            --depth_;
        }
    }

    [[nodiscard]] bool balanced() const { return depth_ == 0 && !in_rendering_; }

private:
    void err(std::string e) { s_.errors.push_back(std::move(e)); }
    void on_draw() {
        if (!in_rendering_) {
            err("draw outside rendering scope");
        }
        if (!pipeline_bound_) {
            err("draw without pipeline");
        }
        ++s_.draws;
    }
    void expect_state(rhi::TextureHandle t, rhi::ResourceState want, const char* what) {
        const auto it = s_.texture_state.find(t.value);
        if (it == s_.texture_state.end() || it->second != want) {
            err(std::format("{} {} not in expected state", what, t.value));
        }
    }
    void check_buffer(rhi::BufferHandle b) {
        if (!s_.buffers.contains(b.value)) {
            err(std::format("use of unknown/destroyed buffer {}", b.value));
        }
    }

    MockState& s_;
    bool       in_rendering_ = false;
    bool       pipeline_bound_ = false;
    int        depth_ = 0;
};

class MockDevice final : public rhi::Device {
public:
    MockDevice() : cmd_(state) {
        features_.dynamic_rendering = features_.timeline_semaphores = features_.synchronization2 = true;
        features_.descriptor_indexing = features_.buffer_device_address = true;
        features_.depth_clamp = features_.sampler_anisotropy = true;
        features_.texture_compression_bc = true;
        features_.max_bindless_textures = 1u << 16;
        features_.adapter_name = "MockDevice";
    }

    MockState       state;
    MockCommandList cmd_;

    const rhi::DeviceFeatures& features() const override { return features_; }
    // Tests toggle optional features (e.g. draw_indirect_count for the GPU-driven path).
    rhi::DeviceFeatures& mutable_features() { return features_; }
    u32 frames_in_flight() const override { return 2; }

    rhi::BufferHandle create_buffer(const rhi::BufferDesc& d) override {
        const rhi::BufferHandle h(next_++, 1);
        state.buffers[h.value] = d;
        ++state.buffers_created;
        return h;
    }
    rhi::TextureHandle create_texture(const rhi::TextureDesc& d) override {
        const rhi::TextureHandle h(next_++, 1);
        state.textures[h.value] = d;
        state.texture_state[h.value] = rhi::ResourceState::Undefined;
        ++state.textures_created;
        return h;
    }
    rhi::SamplerHandle  create_sampler(const rhi::SamplerDesc&) override { return rhi::SamplerHandle(next_++, 1); }
    rhi::ShaderHandle   create_shader(const rhi::ShaderDesc& d) override {
        return d.spirv.empty() ? rhi::ShaderHandle{} : rhi::ShaderHandle(next_++, 1);
    }
    rhi::PipelineHandle create_graphics_pipeline(const rhi::GraphicsPipelineDesc& d) override {
        if (d.mesh.is_valid()) { // ADR-0010 mesh-shading pipeline: no vertex stage
            if (!features_.mesh_shaders || d.vertex.is_valid() || !d.fragment.is_valid()) {
                return {};
            }
            ++state.pipelines_created;
            ++state.mesh_pipelines_created;
            return rhi::PipelineHandle(next_++, 1);
        }
        if (!d.vertex.is_valid() || !d.fragment.is_valid()) {
            return {};
        }
        ++state.pipelines_created;
        return rhi::PipelineHandle(next_++, 1);
    }
    rhi::PipelineHandle create_compute_pipeline(const rhi::ComputePipelineDesc& d) override {
        if (!d.compute.is_valid()) {
            return {};
        }
        ++state.pipelines_created;
        return rhi::PipelineHandle(next_++, 1);
    }

    void destroy(rhi::BufferHandle h) override {
        if (state.buffers.erase(h.value)) {
            ++state.buffers_destroyed;
        }
        state.mapped.erase(h.value);
    }
    void destroy(rhi::TextureHandle h) override {
        if (state.textures.erase(h.value)) {
            ++state.textures_destroyed;
        }
    }
    void destroy(rhi::SamplerHandle) override {}
    void destroy(rhi::ShaderHandle) override {}
    void destroy(rhi::PipelineHandle) override {}

    const rhi::TextureDesc* texture_desc(rhi::TextureHandle h) const override {
        const auto it = state.textures.find(h.value);
        return it == state.textures.end() ? nullptr : &it->second;
    }
    const rhi::BufferDesc* buffer_desc(rhi::BufferHandle h) const override {
        const auto it = state.buffers.find(h.value);
        return it == state.buffers.end() ? nullptr : &it->second;
    }
    u64 buffer_device_address(rhi::BufferHandle h) override { return h.is_valid() ? (u64(h.index()) << 32) : 0; }

    void  update_buffer(rhi::BufferHandle h, ByteSpan data, u64 off) override {
        const auto it = state.buffers.find(h.value);
        if (it == state.buffers.end() || off + data.size() > it->second.size) {
            state.errors.push_back(std::format("update_buffer out of range on {}", h.value));
        }
    }
    void* map(rhi::BufferHandle h) override {
        const auto it = state.buffers.find(h.value);
        if (it == state.buffers.end()) {
            return nullptr;
        }
        auto& mem = state.mapped[h.value];
        mem.resize(static_cast<usize>(it->second.size));
        return mem.data();
    }
    void unmap(rhi::BufferHandle) override {}
    void invalidate_mapped(rhi::BufferHandle h) override {
        if (!state.buffers.contains(h.value)) {
            state.errors.push_back(std::format("invalidate_mapped on unknown buffer {}", h.value));
        }
        ++state.invalidations;
    }
    void update_texture(rhi::TextureHandle h, ByteSpan, bool) override {
        state.texture_state[h.value] = rhi::ResourceState::ShaderRead;
    }
    void update_texture_mip(rhi::TextureHandle h, u32 mip, u32 layer, ByteSpan data) override {
        const auto it = state.textures.find(h.value);
        if (it == state.textures.end() || mip >= it->second.mip_levels || layer >= it->second.array_layers) {
            state.errors.push_back(std::format("update_texture_mip out of range on {}", h.value));
        }
        state.mip_uploads.push_back(MockMipUpload{ h.value, mip, layer, data.size() });
        state.texture_state[h.value] = rhi::ResourceState::ShaderRead;
    }

    rhi::DescriptorHandle register_texture(rhi::TextureHandle, rhi::SamplerHandle) override {
        return rhi::DescriptorHandle(next_desc_++, 1);
    }
    rhi::DescriptorHandle register_storage_texture(rhi::TextureHandle, u32) override {
        return rhi::DescriptorHandle(next_desc_++, 1);
    }
    void unregister_texture(rhi::DescriptorHandle) override {}
    void unregister_storage_texture(rhi::DescriptorHandle) override {}

    rhi::FrameInfo begin_frame() override {
        rhi::FrameInfo f;
        f.cmd = &cmd_;
        f.valid = true;
        return f;
    }
    void end_frame(const rhi::FrameInfo&) override {}
    void on_resize(u32, u32) override {}
    void wait_idle() override {}
    void immediate_submit(const std::function<void(rhi::CommandList&)>& record) override {
        MockCommandList c(state);
        record(c);
        if (!c.balanced()) {
            state.errors.push_back("immediate_submit left an unbalanced command list");
        }
    }
    rhi::FrameStats last_frame_stats() const override { return {}; }

    Result<std::vector<u32>> compile_glsl(rhi::ShaderStage stage, StringView src, StringView name,
                                          const rhi::ShaderCompileOptions& o) override {
        return rhi::compile_glsl(stage, src, name, o);
    }
    Result<std::vector<u32>> compile_glsl_file(rhi::ShaderStage stage, const std::filesystem::path& f,
                                               const rhi::ShaderCompileOptions& o) override {
        return rhi::compile_glsl_file(stage, f, o);
    }

    // A texture standing in for the swapchain image.
    rhi::TextureHandle make_target(u32 w, u32 h, rhi::Format f = rhi::Format::BGRA8Unorm) {
        rhi::TextureDesc d;
        d.format = f;
        d.width = w;
        d.height = h;
        d.usage = rhi::TextureUsage::ColorAttach | rhi::TextureUsage::Sampled;
        return create_texture(d);
    }

private:
    rhi::DeviceFeatures features_;
    u32                 next_ = 1;
    u32                 next_desc_ = 0;
};

} // namespace aether::renderer::test
