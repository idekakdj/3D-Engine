// renderer_impl.h — the concrete Renderer (clustered forward+ over the render graph).
//
// Private header. Implementation is split across:
//   renderer_impl.cpp   : creation, resource registration/release, settings, reload
//   renderer_frame.cpp  : per-frame CPU work (culling, cascades, uploads) + graph passes
// Main thread only (render() fans culling out to the JobSystem internally).
//
// Frame-slot contract: render() must be called at most once per Device frame; per-frame
// GPU data is ring-buffered by (internal render counter % frames_in_flight), which is
// only safe if Device::begin_frame() waited for the slot's previous use.
#pragma once

#include "frame_arena.h"
#include "geometry_arena.h"
#include "gpu_data.h"
#include "handle_pool.h"
#include "ibl.h"
#include "meshlets.h"
#include "pipelines.h"
#include "render_graph.h"
#include "render_math.h"
#include "shadow_math.h"

#include "aether/renderer/renderer.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aether::renderer {

class RendererImpl final : public Renderer {
public:
    RendererImpl(rhi::Device& device, UVec2 output_size);
    ~RendererImpl() override;

    Result<void> initialize();

    MeshHandle     register_mesh(const MeshUpload&) override;
    TextureHandle  register_texture(const TextureUpload&) override;
    MaterialHandle register_material(const MaterialDesc&) override;
    EnvHandle      register_environment(const EnvironmentUpload&) override;
    void           update_material(MaterialHandle, const MaterialDesc&) override;

    void release(MeshHandle) override;
    void release(TextureHandle) override;
    void release(MaterialHandle) override;
    void release(EnvHandle) override;

    void render(const RenderScene& scene, rhi::CommandList& cmd, const RenderTarget& target) override;
    void resize(UVec2 output_size) override;
    Result<void> reload_shaders() override;

    void               request_pick(UVec2 pixel) override;
    std::optional<u32> poll_pick() override;

    RendererSettings&    settings() override { return settings_; }
    const RendererStats& stats() const override { return stats_; }

private:
    // ---- resource records ----
    struct MeshRecord {
        GeometryAllocation   geo;
        std::vector<Submesh> submeshes;
        AABB                 bounds{};
        std::string          name;
        // ADR-0010: meshlets of a static mesh on mesh-shading devices (one buffer:
        // GpuMeshlet array, then the u32 word stream); ranges parallel to `submeshes`.
        rhi::BufferHandle          meshlet_buffer;
        u64                        meshlets_gpu = 0;
        u64                        meshlet_words_gpu = 0;
        std::vector<MeshletBuild::Range> meshlet_ranges;
    };
    struct TextureRecord {
        rhi::TextureHandle    texture;
        rhi::DescriptorHandle descriptor;
    };
    struct MaterialRecord {
        MaterialDesc desc;
    };
    struct EnvRecord {
        EnvironmentMaps maps;
    };
    struct PendingRelease {
        u64                   frame = 0;
        std::function<void()> fn;
    };
    struct PersistentTexture {
        rhi::TextureHandle    texture;
        rhi::DescriptorHandle sampled;
        rhi::DescriptorHandle storage;
        rhi::ResourceState    state = rhi::ResourceState::Undefined;
        UVec2                 size{ 0 };
        u32                   layers = 1;
    };

    // ---- per-frame CPU data ----
    struct ViewSetup {
        Mat4    view{ 1.0f }, inv_view{ 1.0f };
        Mat4    proj{ 1.0f };            // jittered, reverse-Z
        Mat4    proj_unjittered{ 1.0f }; // reverse-Z
        Mat4    view_proj{ 1.0f }, inv_view_proj{ 1.0f }, inv_proj{ 1.0f };
        Mat4    unjittered_view_proj{ 1.0f };
        Vec3    camera_pos{ 0.0f };
        Vec3    forward{ 0.0f, 0.0f, -1.0f };
        f32     near_z = 0.1f, far_z = 1000.0f;
        Vec2    jitter_ndc{ 0.0f };
        Frustum frustum{};
    };
    struct ResolvedInstance {
        u32  first_index = 0;
        u32  index_count = 0;
        i32  vertex_offset = 0;
        u32  material = 0;
        f32  view_depth = 0.0f;
        u8   blend = 0;       // BlendMode
        bool valid = false;
        bool double_sided = false;
        bool skinned = false;   // skinned pipeline (palette valid)
        bool skin_arena = false; // vertices live in the skinned arena
        bool visible = false;   // main view
        bool caster = false;
        u32  meshlet_count = 0; // ADR-0010: static meshes with meshlets
        u64  meshlets = 0;
        u64  meshlet_words = 0;
        u8   cascade_mask = 0;
        AABB bounds{};
    };
    struct DrawItem {
        u64  key = 0;
        u32  instance = 0;
        u32  first_index = 0;
        u32  index_count = 0;
        i32  vertex_offset = 0;
        u32  pipeline = 0;
        bool skin_arena = false;
        bool skinned = false; // skinned pipeline permutation
        bool double_sided = false;
    };
    struct CascadeSetup {
        u32                                  count = 0;
        std::array<CascadeMatrices, kMaxCascades> matrices{};
        std::array<f32, kMaxCascades>        splits{};
        std::array<Frustum, kMaxCascades>    frusta{};
    };

    // GPU-driven opaque path state for the frame being recorded (ADR-0009).
    struct GpuDrivenFrame {
        bool active = false;    // opaque + masked instances are culled on the GPU
        bool occlusion = false; // two-phase Hi-Z occlusion culling
        bool meshlets = false;  // ADR-0010: static candidates drawn with task + mesh shaders
        u32  candidates = 0;
        u32  cmd_capacity = 0;  // commands per phase region (power of two >= candidates)
        std::array<u32, kMaxDrawBatches> batch_count{};
        std::array<u32, kMaxDrawBatches> batch_offset{};
        u64          candidates_gpu = 0;
        u64          view_gpu = 0;
        GpuCullView* view_cpu = nullptr; // Hi-Z index patched while the graph records
    };
    // One CPU readback block per frame slot (counter statistics + pick id).
    struct ReadbackSlot {
        bool pending = false;
        bool gpu_stats = false;
        bool pick = false;
    };

    // renderer_impl.cpp
    void create_samplers();
    void destroy_samplers();
    [[nodiscard]] GpuMaterial make_gpu_material(const MaterialDesc& d) const;
    void rebuild_material_table();
    void defer_release(std::function<void()> fn);
    void process_releases(bool force_all);
    void ensure_brdf_lut();
    void destroy_persistent(PersistentTexture& t);

    // renderer_frame.cpp
    void setup_view(const RenderScene& scene);
    void setup_lights(const RenderScene& scene);
    void resolve_instances(const RenderScene& scene, GpuInstance* gpu_instances, u32* user_ids);
    void setup_cascades();
    void cull_cascades(const RenderScene& scene);
    void build_draw_lists(const RenderScene& scene);
    void write_frame_constants(const RenderScene& scene, GpuFrame& f, u64 instances, u64 materials,
                               u64 lights, u64 joints, u64 lines);
    void ensure_shadow_map();
    void ensure_history();
    void draw_items(rhi::CommandList& cmd, std::span<const DrawItem> items, MeshPush push,
                    bool count_stats);
    void build_graph(const RenderScene& scene, const RenderTarget& target);
    // ADR-0009: GPU-driven culling, Hi-Z, picking readback.
    [[nodiscard]] bool gpu_culling_supported() const;
    [[nodiscard]] bool mesh_shading_supported() const;
    void build_mesh_meshlets(MeshRecord& rec, const MeshUpload& up);
    void write_cull_data(GpuCullInstance* candidates, GpuCullView* view);
    void collect_readbacks(u32 slot);
    void ensure_visibility_buffer(u32 instances);
    void ensure_readback_buffer();
    void draw_gpu_batches(rhi::CommandList& cmd, MeshPass pass, rhi::BufferHandle draws,
                          rhi::BufferHandle counters, MeshPush push, bool phase1, bool phase2);

    [[nodiscard]] bool shadows_active() const;
    [[nodiscard]] bool debug_view_active() const { return settings_.debug_view != DebugView::None; }

    rhi::Device&     device_;
    UVec2            output_size_;
    u32              frames_in_flight_ = 2;
    u64              frame_counter_ = 0;
    RendererSettings settings_{};
    RendererStats    stats_{};

    // samplers
    rhi::SamplerHandle material_sampler_;
    rhi::SamplerHandle linear_clamp_;
    rhi::SamplerHandle point_clamp_;
    rhi::SamplerHandle shadow_sampler_;

    std::unique_ptr<PipelineLibrary> pipelines_;
    GeometryArena                    geometry_;
    FrameArena                       frame_arena_;
    std::unique_ptr<RGResourcePool>  rg_pool_;
    std::unique_ptr<RenderGraph>     graph_;

    HandlePool<MeshTag, MeshRecord>         meshes_;
    HandlePool<TextureTag, TextureRecord>   textures_;
    HandlePool<MaterialTag, MaterialRecord> materials_;
    HandlePool<EnvTag, EnvRecord>           environments_;
    std::vector<GpuMaterial>                material_table_; // [0] = default material
    bool                                    materials_dirty_ = true;
    std::vector<PendingRelease>             releases_;

    IblTexture                       brdf_lut_{};
    PersistentTexture                shadow_map_{};
    std::array<PersistentTexture, 2> history_{};
    u32                              history_index_ = 0;
    bool                             history_valid_ = false;
    Mat4                             prev_view_proj_{ 1.0f };
    bool                             has_prev_view_ = false;

    // per-frame scratch (capacity reused)
    ViewSetup                     view_{};
    CascadeSetup                  cascades_{};
    std::vector<GpuLight>         gpu_lights_;
    u32                           directional_count_ = 0;
    i32                           shadow_light_ = -1;
    Vec3                          shadow_dir_{ 0.0f, -1.0f, 0.0f };
    std::vector<ResolvedInstance> resolved_;
    std::vector<AABB>             chunk_caster_bounds_;
    AABB                          caster_bounds_{};
    bool                          has_casters_ = false;
    std::vector<DrawItem>         prepass_draws_;
    std::vector<DrawItem>         opaque_draws_;
    std::vector<DrawItem>         translucent_draws_;
    std::array<std::vector<DrawItem>, kMaxCascades> cascade_draws_;
    u64                           frame_gpu_address_ = 0;
    u32                           line_vertex_count_ = 0;
    u32                           frame_slot_ = 0;
    u32                           cpu_visible_ = 0; // instances drawn from CPU lists

    // ---- GPU-driven path + picking (ADR-0009) ----
    GpuDrivenFrame                 gpu_{};
    std::vector<GpuCullInstance>   cull_candidates_;
    rhi::BufferHandle              visibility_buffer_;  // u32 per frame instance, persistent
    u32                            visibility_capacity_ = 0;
    rhi::ResourceState             visibility_state_ = rhi::ResourceState::Undefined;
    bool                           visibility_clear_ = true;
    rhi::BufferHandle              readback_buffer_;    // GpuToCpu, kReadbackWords per slot
    rhi::ResourceState             readback_state_ = rhi::ResourceState::Undefined;
    std::vector<ReadbackSlot>      readback_slots_;
    std::array<u32, 4>             gpu_stats_latched_{}; // frustum, occlusion, visible, triangles
    std::optional<UVec2>           pending_pick_;        // RenderTarget pixel of the next render()
    i32                            logged_cull_mode_ = -1;
    bool                           pick_this_frame_ = false;
    UVec2                          pick_pixel_{ 0 };     // internal-resolution pixel
    std::optional<u32>             pick_result_;
};

} // namespace aether::renderer
