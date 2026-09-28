// renderer_frame.cpp — per-frame work: view setup, CPU culling, cascades, uploads and the
// render-graph pass setup (shadows -> prepass -> SSAO -> light culling -> forward+ ->
// TAA -> bloom -> tonemap -> debug lines).
#include "renderer_impl.h"

#include "format_utils.h"

#include "aether/core/job_system.h"
#include "aether/core/log.h"
#include "aether/core/time.h"
#include "aether/renderer/instance_flags.h"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cstring>
#include <format>

namespace aether::renderer {

namespace {
constexpr u32 kCullChunk = 256;
constexpr u32 kComputeGroup = 8;

u32 groups(u32 n, u32 g = kComputeGroup) { return (n + g - 1) / g; }
u32 didx(rhi::DescriptorHandle h) { return h.is_valid() ? h.index() : kGpuInvalidIndex; }

AABB empty_aabb() { return AABB{ Vec3(FLT_MAX), Vec3(-FLT_MAX) }; }

u32 depth_key(f32 d) { return std::bit_cast<u32>(std::max(d, 0.0f)); }

// Runs fn(chunk) for every chunk, on the job system when it is running.
void for_each_chunk(u32 chunks, const std::function<void(u32)>& fn) {
    if (chunks > 1 && JobSystem::worker_count() > 0) {
        JobCounter counter;
        JobSystem::parallel_for(chunks, 1, fn, &counter);
        JobSystem::wait(counter);
    } else {
        for (u32 c = 0; c < chunks; ++c) {
            fn(c);
        }
    }
}

rhi::Viewport full_viewport(UVec2 s) {
    return rhi::Viewport{ 0.0f, 0.0f, static_cast<f32>(s.x), static_cast<f32>(s.y), 0.0f, 1.0f };
}
rhi::Scissor full_scissor(UVec2 s) { return rhi::Scissor{ 0, 0, s.x, s.y }; }

constexpr const char* kCascadeNames[kMaxCascades] = { "Cascade0", "Cascade1", "Cascade2", "Cascade3" };
constexpr const char* kBloomDownNames[] = { "Bloom.Down0", "Bloom.Down1", "Bloom.Down2",
                                            "Bloom.Down3", "Bloom.Down4", "Bloom.Down5" };
constexpr const char* kBloomUpNames[] = { "Bloom.Up0", "Bloom.Up1", "Bloom.Up2",
                                          "Bloom.Up3", "Bloom.Up4", "Bloom.Up5" };
constexpr u32 kMaxBloomLevels = 6;
} // namespace

// ===========================================================================
// View + lights
// ===========================================================================
void RendererImpl::setup_view(const RenderScene& scene) {
    const RenderView& v = scene.view;
    ViewSetup& o = view_;
    o.near_z = std::max(v.near_z, 1e-4f);
    o.far_z = std::max(v.far_z, o.near_z * 2.0f);
    o.view = v.view;
    o.inv_view = glm::inverse(v.view);
    o.proj_unjittered = to_reverse_z(v.proj, o.near_z);

    const bool taa = settings_.taa && pipelines_->valid(PipelineId::Taa) && !debug_view_active();
    o.jitter_ndc = Vec2(0.0f);
    if (taa) {
        const Vec2 px = taa_jitter_pixels(frame_counter_);
        o.jitter_ndc = Vec2(2.0f * px.x / static_cast<f32>(output_size_.x),
                            2.0f * px.y / static_cast<f32>(output_size_.y));
    }
    o.proj = apply_clip_jitter(o.proj_unjittered, o.jitter_ndc);
    o.view_proj = o.proj * o.view;
    o.inv_view_proj = glm::inverse(o.view_proj);
    o.inv_proj = glm::inverse(o.proj);
    o.unjittered_view_proj = o.proj_unjittered * o.view;
    o.camera_pos = Vec3(o.inv_view[3]);
    const Vec3 back = Vec3(o.inv_view[2]);
    const f32  len = glm::length(back);
    o.forward = len > 1e-6f ? -back / len : Vec3(0.0f, 0.0f, -1.0f);
    o.frustum = extract_frustum(o.unjittered_view_proj, true, false);
}

void RendererImpl::setup_lights(const RenderScene& scene) {
    gpu_lights_.clear();
    spot_candidates_.clear();
    point_candidates_.clear();
    directional_count_ = 0;
    shadow_light_ = -1;
    // Directional lights first: the shader loops over [0, directional_count) globally and
    // over the clustered lists for the rest.
    for (int pass = 0; pass < 2; ++pass) {
        for (const RenderLight& l : scene.lights) {
            const bool directional = l.type == LightType::Directional;
            if (directional != (pass == 0)) {
                continue;
            }
            if (gpu_lights_.size() >= kMaxLights) {
                break;
            }
            GpuLight g;
            const f32 dl = glm::length(l.direction);
            g.direction = dl > 1e-6f ? l.direction / dl : Vec3(0.0f, -1.0f, 0.0f);
            g.position = l.position;
            g.color = l.color * std::max(l.intensity, 0.0f);
            g.range = std::max(l.range, 1e-3f);
            switch (l.type) {
            case LightType::Directional: g.type = kGpuLightDirectional; break;
            case LightType::Point:
                g.type = kGpuLightPoint;
                if (l.cast_shadows && settings_.point_shadows) { // ADR-0015: selected in setup_spot_shadows()
                    point_candidates_.push_back(PointCandidate{ static_cast<u32>(gpu_lights_.size()), g.position, g.range });
                }
                break;
            case LightType::Spot: {
                g.type = kGpuLightSpot;
                const f32 outer = std::clamp(l.outer_cone, -1.0f, 1.0f);
                const f32 inner = std::clamp(std::max(l.inner_cone, outer + 1e-4f), -1.0f, 1.0f);
                g.spot_scale = 1.0f / std::max(inner - outer, 1e-4f);
                g.spot_offset = -outer * g.spot_scale;
                if (l.cast_shadows && settings_.spot_shadows) { // ADR-0012: selected in setup_spot_shadows()
                    spot_candidates_.push_back(
                        SpotCandidate{ static_cast<u32>(gpu_lights_.size()), g.position, g.direction, g.range, outer });
                }
                break;
            }
            }
            if (directional) {
                if (shadow_light_ < 0 && l.cast_shadows && settings_.shadows) {
                    shadow_light_ = static_cast<i32>(gpu_lights_.size());
                    shadow_dir_ = g.direction;
                    g.shadow = 0;
                }
                ++directional_count_;
            }
            gpu_lights_.push_back(g);
        }
    }
}

// ===========================================================================
// Instance resolve + main-view culling (parallel)
// ===========================================================================
void RendererImpl::resolve_instances(const RenderScene& scene, GpuInstance* gpu, u32* user_ids) {
    const u32 n = static_cast<u32>(scene.instances.size());
    resolved_.assign(n, ResolvedInstance{});
    const u32 chunks = (n + kCullChunk - 1) / kCullChunk;
    chunk_caster_bounds_.assign(chunks, empty_aabb());
    const bool cull = settings_.frustum_culling;
    const bool gpu_cull = gpu_.active; // opaque/masked: frustum (and occlusion) tested on the GPU
    const u32  joint_total = static_cast<u32>(scene.joint_matrices.size());

    for_each_chunk(chunks, [&](u32 c) {
        const u32 begin = c * kCullChunk;
        const u32 end = std::min(n, begin + kCullChunk);
        AABB      casters = empty_aabb();
        for (u32 i = begin; i < end; ++i) {
            const RenderMeshInstance& in = scene.instances[i];
            ResolvedInstance&         r = resolved_[i];
            GpuInstance               gi;
            gi.model = in.transform;
            const Mat3 m3(in.transform);
            const f32  det = glm::determinant(m3);
            const Mat3 nm = std::abs(det) > 1e-12f ? glm::transpose(glm::inverse(m3)) : m3;
            for (int k = 0; k < 3; ++k) {
                gi.normal_col[k] = Vec4(nm[k], 0.0f);
            }

            const MeshRecord* mesh = meshes_.get(in.mesh);
            if (mesh && in.submesh < mesh->submeshes.size()) {
                const Submesh& sm = mesh->submeshes[in.submesh];
                r.valid = sm.index_count > 0;
                r.first_index = static_cast<u32>(mesh->geo.indices.offset) + sm.first_index;
                r.index_count = sm.index_count;
                r.vertex_offset = static_cast<i32>(mesh->geo.vertices.offset);
                r.skin_arena = mesh->geo.skinned;
                r.bounds = aabb_is_unset(in.world_bounds) ? transform_aabb(sm.bounds, in.transform)
                                                           : in.world_bounds;
                const bool wants_skin = instance_flags::has(in.flags, instance_flags::kSkinned);
                r.skinned = wants_skin && mesh->geo.skinned && in.first_joint != kInvalidU32 &&
                            in.joint_count > 0 &&
                            static_cast<u64>(in.first_joint) + in.joint_count <= joint_total;
                if (mesh->meshlet_buffer.is_valid() && in.submesh < mesh->meshlet_ranges.size()) {
                    const MeshletBuild::Range& mr = mesh->meshlet_ranges[in.submesh];
                    r.meshlet_count = mr.count;
                    r.meshlets = mesh->meshlets_gpu + static_cast<u64>(mr.first) * sizeof(GpuMeshlet);
                    r.meshlet_words = mesh->meshlet_words_gpu;
                }
            }
            if (const MaterialRecord* mat = materials_.get(in.material)) {
                r.material = in.material.index() + 1;
                r.blend = static_cast<u8>(mat->desc.blend);
                r.double_sided = mat->desc.double_sided;
            }
            gi.material = r.material;
            gi.flags = in.flags & instance_flags::kKnownMask;
            gi.joint_offset = r.skinned ? in.first_joint : 0u;
            gi.joint_count = r.skinned ? in.joint_count : 0u;
            std::memcpy(&gpu[i], &gi, sizeof(GpuInstance)); // write-combined: one sequential write
            if (user_ids) {
                user_ids[i] = in.user_id;
            }

            if (!r.valid) {
                continue;
            }
            const bool never_cull = instance_flags::has(in.flags, instance_flags::kNeverCull);
            const bool hidden = instance_flags::has(in.flags, instance_flags::kHiddenInMainView);
            // GPU path: `visible` means "GPU culling candidate" for opaque/masked instances.
            const bool gpu_candidate = gpu_cull && r.blend != static_cast<u8>(BlendMode::Translucent);
            r.visible = !hidden && (never_cull || !cull || gpu_candidate ||
                                    frustum_intersects_aabb(view_.frustum, r.bounds));
            r.view_depth = glm::dot(r.bounds.center() - view_.camera_pos, view_.forward);
            r.caster = instance_flags::has(in.flags, instance_flags::kCastShadow) &&
                       r.blend != static_cast<u8>(BlendMode::Translucent);
            if (r.caster) {
                casters.expand(r.bounds);
            }
        }
        chunk_caster_bounds_[c] = casters;
    });

    caster_bounds_ = empty_aabb();
    for (const AABB& b : chunk_caster_bounds_) {
        if (b.valid()) {
            caster_bounds_.expand(b);
        }
    }
    has_casters_ = caster_bounds_.valid();
}

// ===========================================================================
// Cascaded shadow maps
// ===========================================================================
void RendererImpl::setup_cascades() {
    cascades_.count = 0;
    if (!shadows_active() || !has_casters_) {
        return;
    }
    const u32 count = std::clamp(settings_.shadow_cascades, 1u, kMaxCascades);
    const f32 max_d = std::min(settings_.shadow_distance, view_.far_z);
    if (max_d <= view_.near_z * 1.01f) {
        return;
    }
    const u32 size = std::clamp(settings_.shadow_map_size, 256u, 8192u);
    cascades_.splits = compute_cascade_splits(count, view_.near_z, max_d);
    for (u32 c = 0; c < count; ++c) {
        const f32 d0 = c == 0 ? view_.near_z : cascades_.splits[c - 1];
        const f32 d1 = cascades_.splits[c];
        const BoundingSphere sphere = cascade_bounding_sphere(view_.proj_unjittered, view_.inv_view, d0, d1);
        // Extend toward the light so off-screen casters still land in the map.
        f32 extent = 0.0f;
        for (int k = 0; k < 8; ++k) {
            const Vec3 corner((k & 1) ? caster_bounds_.max.x : caster_bounds_.min.x,
                              (k & 2) ? caster_bounds_.max.y : caster_bounds_.min.y,
                              (k & 4) ? caster_bounds_.max.z : caster_bounds_.min.z);
            extent = std::max(extent, glm::dot(corner - sphere.center, -shadow_dir_));
        }
        extent = std::min(extent, sphere.radius + 4.0f * max_d); // keep depth precision sane
        cascades_.matrices[c] = fit_cascade(sphere, shadow_dir_, size, extent);
        cascades_.frusta[c] = extract_frustum(cascades_.matrices[c].view_proj, true, true);
    }
    cascades_.count = count;
}

void RendererImpl::cull_cascades(const RenderScene& scene) {
    if (cascades_.count == 0) {
        return;
    }
    const u32 n = static_cast<u32>(resolved_.size());
    const u32 chunks = (n + kCullChunk - 1) / kCullChunk;
    const u32 count = cascades_.count;
    for_each_chunk(chunks, [&](u32 c) {
        const u32 begin = c * kCullChunk;
        const u32 end = std::min(n, begin + kCullChunk);
        for (u32 i = begin; i < end; ++i) {
            ResolvedInstance& r = resolved_[i];
            if (!r.valid || !r.caster) {
                continue;
            }
            const bool never_cull = instance_flags::has(scene.instances[i].flags, instance_flags::kNeverCull);
            u8 mask = 0;
            for (u32 k = 0; k < count; ++k) {
                if (never_cull || frustum_intersects_aabb(cascades_.frusta[k], r.bounds)) {
                    mask = static_cast<u8>(mask | (1u << k));
                }
            }
            r.cascade_mask = mask;
        }
    });
}

// ===========================================================================
// Draw lists
// ===========================================================================
void RendererImpl::build_draw_lists(const RenderScene& scene) {
    prepass_draws_.clear();
    opaque_draws_.clear();
    translucent_draws_.clear();
    cull_candidates_.clear();
    gpu_.batch_count.fill(0);
    for (auto& l : cascade_draws_) {
        l.clear();
    }
    u32 visible = 0;
    for (u32 i = 0; i < resolved_.size(); ++i) {
        const ResolvedInstance& r = resolved_[i];
        if (!r.valid) {
            continue;
        }
        DrawItem d;
        d.instance = i;
        d.first_index = r.first_index;
        d.index_count = r.index_count;
        d.vertex_offset = r.vertex_offset;
        d.skin_arena = r.skin_arena;
        d.skinned = r.skinned;
        d.double_sided = r.double_sided;
        const bool masked = r.blend == static_cast<u8>(BlendMode::Masked);
        const bool translucent = r.blend == static_cast<u8>(BlendMode::Translucent);
        if (r.visible && gpu_.active && !translucent) {
            // GPU-driven: frustum/occlusion culled and drawn indirectly (ADR-0009).
            GpuCullInstance c;
            c.aabb_min = r.bounds.min;
            c.aabb_max = r.bounds.max;
            c.instance = i;
            // ADR-0010: static meshes with meshlets go through the task + mesh shaders.
            const bool meshlet = gpu_.meshlets && !r.skin_arena && !r.skinned && r.meshlet_count > 0;
            c.batch = draw_batch_index(r.skinned, masked, r.double_sided, r.skin_arena, meshlet);
            c.first_index = r.first_index;
            c.index_count = r.index_count;
            c.vertex_offset = r.vertex_offset;
            if (meshlet) {
                c.meshlets = r.meshlets;
                c.meshlet_words = r.meshlet_words;
                c.meshlet_count = r.meshlet_count;
                ++stats_.meshlet_instances;
            }
            c.flags = instance_flags::has(scene.instances[i].flags, instance_flags::kNeverCull) ? kCullFlagNeverCull
                                                                                                : 0u;
            ++gpu_.batch_count[c.batch];
            cull_candidates_.push_back(c);
        } else if (r.visible) {
            ++visible;
            if (translucent) {
                d.pipeline = mesh_pipeline_index(MeshPass::Translucent, r.skinned, false, r.double_sided);
                d.key = ~static_cast<u64>(depth_key(r.view_depth)); // back-to-front
                translucent_draws_.push_back(d);
            } else {
                d.pipeline = mesh_pipeline_index(MeshPass::Depth, r.skinned, masked, r.double_sided);
                d.key = (static_cast<u64>(d.pipeline) << 40) | depth_key(r.view_depth); // front-to-back
                prepass_draws_.push_back(d);
                d.pipeline = mesh_pipeline_index(MeshPass::Forward, r.skinned, false, r.double_sided);
                d.key = (static_cast<u64>(d.pipeline) << 40) | (static_cast<u64>(d.skin_arena) << 32) |
                        r.material;
                opaque_draws_.push_back(d);
            }
        }
        if (r.cascade_mask != 0) {
            d.pipeline = mesh_pipeline_index(MeshPass::Shadow, r.skinned, masked, r.double_sided);
            d.key = (static_cast<u64>(d.pipeline) << 40) | (static_cast<u64>(d.skin_arena) << 32) | r.material;
            for (u32 c = 0; c < cascades_.count; ++c) {
                if (r.cascade_mask & (1u << c)) {
                    cascade_draws_[c].push_back(d);
                }
            }
        }
    }
    auto by_key = [](const DrawItem& a, const DrawItem& b) { return a.key < b.key; };
    std::sort(prepass_draws_.begin(), prepass_draws_.end(), by_key);
    std::sort(opaque_draws_.begin(), opaque_draws_.end(), by_key);
    std::stable_sort(translucent_draws_.begin(), translucent_draws_.end(), by_key);
    for (u32 c = 0; c < cascades_.count; ++c) {
        std::sort(cascade_draws_[c].begin(), cascade_draws_[c].end(), by_key);
    }
    cpu_visible_ = visible;
    stats_.instances_visible = visible;
}

// ===========================================================================
// GPU-driven culling (ADR-0009)
// ===========================================================================
bool RendererImpl::gpu_culling_supported() const {
    const rhi::DeviceFeatures& f = device_.features();
    // Overdraw visualises the CPU draw lists (it re-pipelines every opaque item).
    return settings_.gpu_culling && f.draw_indirect_count && f.draw_indirect_first_instance &&
           pipelines_->valid(PipelineId::GpuCull) && settings_.debug_view != DebugView::Overdraw;
}

bool RendererImpl::mesh_shading_supported() const {
    if (!settings_.mesh_shading || !device_.features().mesh_shaders) {
        return false;
    }
    for (u32 i = kPipelineCount; i < kPipelineTableSize; ++i) {
        if (!pipelines_->get(i).is_valid()) {
            return false;
        }
    }
    return true;
}

void RendererImpl::write_cull_data(GpuCullInstance* candidates, GpuCullView* view) {
    const u32 n = static_cast<u32>(cull_candidates_.size());
    gpu_.candidates = n;
    gpu_.cmd_capacity = std::max(64u, std::bit_ceil(std::max(n, 1u)));
    u32 offset = 0;
    std::array<u32, kMaxDrawBatches> cursor{};
    for (u32 b = 0; b < kMaxDrawBatches; ++b) {
        gpu_.batch_offset[b] = offset;
        cursor[b] = offset;
        offset += gpu_.batch_count[b];
    }
    // Candidates grouped by batch (neighbouring threads mostly hit the same counter).
    for (const GpuCullInstance& c : cull_candidates_) {
        std::memcpy(&candidates[cursor[c.batch]++], &c, sizeof(GpuCullInstance));
    }
    GpuCullView v;
    for (u32 i = 0; i < 6; ++i) {
        v.planes[i] = view_.frustum.planes[i];
    }
    v.plane_count = settings_.frustum_culling ? view_.frustum.plane_count : 0u;
    v.view_proj = view_.view_proj; // jittered, like the depth buffer
    v.viewport = Vec2(output_size_);
    for (u32 b = 0; b < kMaxDrawBatches; ++b) {
        v.batch_offset[b] = gpu_.batch_offset[b];
    }
    std::memcpy(view, &v, sizeof(GpuCullView)); // Hi-Z fields are patched by the Hi-Z pass
    gpu_.view_cpu = view;
}

void RendererImpl::ensure_visibility_buffer(u32 instances) {
    const u32 want = std::max(256u, std::bit_ceil(std::max(instances, 1u)));
    if (visibility_buffer_.is_valid() && visibility_capacity_ >= want) {
        return;
    }
    if (visibility_buffer_.is_valid()) {
        device_.destroy(visibility_buffer_); // deferred past in-flight frames by the device
    }
    rhi::BufferDesc d;
    d.size = static_cast<u64>(want) * sizeof(u32);
    d.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst;
    d.debug_name = "GpuCull.Visibility";
    visibility_buffer_ = device_.create_buffer(d);
    visibility_capacity_ = visibility_buffer_.is_valid() ? want : 0u;
    visibility_state_ = rhi::ResourceState::Undefined;
    visibility_clear_ = true;
}

void RendererImpl::ensure_readback_buffer() {
    if (readback_buffer_.is_valid()) {
        return;
    }
    rhi::BufferDesc d;
    d.size = static_cast<u64>(frames_in_flight_) * kReadbackWords * sizeof(u32);
    d.usage = rhi::BufferUsage::TransferDst;
    d.memory = rhi::MemoryUsage::GpuToCpu;
    d.debug_name = "Renderer.Readback";
    readback_buffer_ = device_.create_buffer(d);
    readback_state_ = rhi::ResourceState::Undefined;
    readback_slots_.assign(frames_in_flight_, ReadbackSlot{});
}

void RendererImpl::collect_readbacks(u32 slot) {
    if (!readback_buffer_.is_valid() || slot >= readback_slots_.size() || !readback_slots_[slot].pending) {
        return;
    }
    // The frame that last used this slot has completed: Device::begin_frame() waited for it
    // (frame-slot contract, renderer_impl.h) and end_frame made its writes host-available.
    ReadbackSlot& rs = readback_slots_[slot];
    device_.invalidate_mapped(readback_buffer_);
    const auto* base = static_cast<const u32*>(device_.map(readback_buffer_));
    if (base != nullptr) {
        const u32* w = base + static_cast<usize>(slot) * kReadbackWords;
        if (rs.gpu_stats) {
            for (u32 k = 0; k < 4; ++k) {
                gpu_stats_latched_[k] = w[k];
            }
        }
        if (rs.pick) {
            pick_result_ = w[kCounterPick - kCounterStats];
        }
    }
    rs = ReadbackSlot{};
}

void RendererImpl::draw_gpu_batches(rhi::CommandList& cmd, MeshPass pass, rhi::BufferHandle draws,
                                    rhi::BufferHandle counters, MeshPush push, bool phase1, bool phase2) {
    if (gpu_.candidates == 0) {
        return;
    }
    cmd.bind_index_buffer(geometry_.index_buffer(), 0, false);
    const bool depth_like = pass == MeshPass::Depth || pass == MeshPass::Shadow;
    u32        bound_pipeline = kInvalidU32;
    int        bound_arena = -1;
    for (u32 b = 0; b < kMaxDrawBatches; ++b) {
        if (gpu_.batch_count[b] == 0) {
            continue;
        }
        const bool meshlet = (b & kBatchMeshlet) != 0;
        const bool skinned = (b & 8u) != 0;
        const bool masked = (b & 4u) != 0;
        const bool double_sided = (b & 2u) != 0;
        const bool skin_arena = (b & 1u) != 0;
        if (meshlet) {
            // ADR-0010: task + mesh shaders; the command array holds GpuMeshTaskCommands.
            const u32                 mp = meshlet_pipeline_index(pass, depth_like && masked, double_sided);
            const rhi::PipelineHandle p = pipelines_->get(mp);
            if (!p.is_valid()) {
                continue;
            }
            cmd.bind_pipeline(p);
            bound_pipeline = mp;
            MeshletPush mpush;
            mpush.frame = push.frame;
            mpush.light_grid = push.light_grid;
            mpush.light_indices = push.light_indices;
            mpush.view_index = push.view_index;
            mpush.pass_flags = push.pass_flags;
            mpush.ssao_tex = push.ssao_tex;
            mpush.candidates = gpu_.candidates_gpu;
            mpush.vertices = device_.buffer_device_address(geometry_.static_vertex_buffer());
            mpush.cone_culling = double_sided ? 0u : 1u;
            mpush.cull_view = gpu_.view_gpu;
            const u64 draws_gpu = device_.buffer_device_address(draws);
            for (u32 phase = 0; phase < 2; ++phase) {
                if ((phase == 0 && !phase1) || (phase == 1 && !phase2)) {
                    continue;
                }
                const u64 cmd_offset = (static_cast<u64>(phase) * gpu_.cmd_capacity + gpu_.batch_offset[b]) *
                                       sizeof(GpuMeshTaskCommand);
                const u64 count_offset =
                    static_cast<u64>((phase == 0 ? kCounterPhase1 : kCounterPhase2) + b) * sizeof(u32);
                mpush.commands = draws_gpu + cmd_offset;
                cmd.push_constants(rhi::ShaderStage::AllGraphics, 0, sizeof(mpush), &mpush);
                cmd.draw_mesh_tasks_indirect_count(draws, cmd_offset, counters, count_offset, gpu_.batch_count[b],
                                                   sizeof(GpuMeshTaskCommand));
                ++stats_.draw_calls;
            }
            continue;
        }
        const u32  pipeline = mesh_pipeline_index(pass, skinned, depth_like && masked, double_sided);
        const rhi::PipelineHandle p = pipelines_->get(pipeline);
        if (!p.is_valid()) {
            continue;
        }
        if (pipeline != bound_pipeline) {
            bound_pipeline = pipeline;
            cmd.bind_pipeline(p);
            cmd.push_constants(rhi::ShaderStage::AllGraphics, 0, sizeof(push), &push);
        }
        const int arena = skin_arena ? 1 : 0;
        if (arena != bound_arena) {
            bound_arena = arena;
            cmd.bind_vertex_buffer(0, skin_arena ? geometry_.skinned_vertex_buffer() : geometry_.static_vertex_buffer(),
                                   0);
        }
        for (u32 phase = 0; phase < 2; ++phase) {
            if ((phase == 0 && !phase1) || (phase == 1 && !phase2)) {
                continue;
            }
            const u64 cmd_offset =
                (static_cast<u64>(phase) * gpu_.cmd_capacity + gpu_.batch_offset[b]) * sizeof(GpuDrawCommand);
            const u64 count_offset =
                static_cast<u64>((phase == 0 ? kCounterPhase1 : kCounterPhase2) + b) * sizeof(u32);
            cmd.draw_indexed_indirect_count(draws, cmd_offset, counters, count_offset, gpu_.batch_count[b],
                                            sizeof(GpuDrawCommand));
            ++stats_.draw_calls;
        }
    }
}

void RendererImpl::request_pick(UVec2 pixel) { pending_pick_ = pixel; }

std::optional<u32> RendererImpl::poll_pick() {
    std::optional<u32> r = pick_result_;
    pick_result_.reset();
    return r;
}

void RendererImpl::draw_items(rhi::CommandList& cmd, std::span<const DrawItem> items, MeshPush push,
                              bool count_stats) {
    if (items.empty()) {
        return;
    }
    cmd.bind_index_buffer(geometry_.index_buffer(), 0, false);
    u32 bound_pipeline = kInvalidU32;
    int bound_arena = -1;
    bool pipeline_ok = false;
    for (const DrawItem& d : items) {
        if (d.pipeline != bound_pipeline) {
            bound_pipeline = d.pipeline;
            const rhi::PipelineHandle p = pipelines_->get(d.pipeline);
            pipeline_ok = p.is_valid();
            if (pipeline_ok) {
                cmd.bind_pipeline(p);
                cmd.push_constants(rhi::ShaderStage::AllGraphics, 0, sizeof(push), &push);
            }
        }
        if (!pipeline_ok) {
            continue;
        }
        const int arena = d.skin_arena ? 1 : 0;
        if (arena != bound_arena) {
            bound_arena = arena;
            cmd.bind_vertex_buffer(0, d.skin_arena ? geometry_.skinned_vertex_buffer()
                                                   : geometry_.static_vertex_buffer(), 0);
        }
        cmd.draw_indexed(d.index_count, 1, d.first_index, d.vertex_offset, d.instance);
        ++stats_.draw_calls;
        if (count_stats) {
            stats_.triangles += d.index_count / 3;
        }
    }
}

// ===========================================================================
// Persistent textures
// ===========================================================================
void RendererImpl::ensure_shadow_map() {
    const u32 size = std::clamp(settings_.shadow_map_size, 256u, 8192u);
    const u32 layers = kMaxCascades;
    if (shadow_map_.texture.is_valid() && shadow_map_.size.x == size && shadow_map_.layers == layers) {
        return;
    }
    destroy_persistent(shadow_map_); // device defers destruction past in-flight frames
    rhi::TextureDesc d;
    d.type = rhi::TextureType::Tex2DArray;
    d.format = kShadowFormat;
    d.width = size;
    d.height = size;
    d.array_layers = layers;
    d.usage = rhi::TextureUsage::DepthAttach | rhi::TextureUsage::Sampled;
    d.debug_name = "CascadedShadowMap";
    shadow_map_.texture = device_.create_texture(d);
    if (!shadow_map_.texture.is_valid()) {
        AE_LOG_ERROR("Renderer", "shadow map creation failed ({}^2 x {})", size, layers);
        return;
    }
    shadow_map_.sampled = device_.register_texture(shadow_map_.texture, shadow_sampler_);
    shadow_map_.size = UVec2(size);
    shadow_map_.layers = layers;
    shadow_map_.state = rhi::ResourceState::Undefined;
}

// ===========================================================================
// Spot-light shadows (ADR-0012)
// ===========================================================================
void RendererImpl::setup_spot_shadows() {
    spot_shadows_.clear();
    stats_.spot_shadow_maps = 0;
    stats_.point_shadow_maps = 0;
    if (!has_casters_ || !pipelines_->get(mesh_pipeline_index(MeshPass::Shadow, false, false, false)).is_valid()) {
        return;
    }
    setup_spot_views();
    setup_point_views();
}

void RendererImpl::setup_spot_views() {
    const u32 budget = std::min(settings_.max_spot_shadows, kMaxSpotShadows);
    if (spot_candidates_.empty() || budget == 0) {
        return;
    }
    // Only spots whose light volume reaches the view; the nearest ones win.
    std::vector<std::pair<f32, const SpotCandidate*>> order;
    for (const SpotCandidate& c : spot_candidates_) {
        if (!frustum_intersects_sphere(view_.frustum, c.position, c.range)) {
            continue;
        }
        order.emplace_back(glm::length(c.position - view_.camera_pos) - c.range, &c);
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (order.size() > budget) {
        order.resize(budget);
    }
    if (order.empty()) {
        return;
    }
    ensure_spot_shadow_map();
    if (!spot_shadow_map_.texture.is_valid()) {
        return;
    }
    const f32 size = static_cast<f32>(spot_shadow_map_.size.x);
    for (const auto& [key, c] : order) {
        (void)key;
        // Cone half-angle + a small margin (PCF footprint), capped: wider cones are unshadowed
        // outside the capped frustum rather than losing all resolution.
        const f32  half = std::clamp(std::acos(std::clamp(c->outer_cos, -1.0f, 1.0f)) + 2.0f * kDeg2Rad,
                                    1.0f * kDeg2Rad, 80.0f * kDeg2Rad);
        const f32  near_z = std::clamp(c->range * 0.01f, 0.02f, 0.5f);
        const f32  far_z = std::max(c->range, near_z * 2.0f);
        const Vec3 up = std::abs(c->direction.y) > 0.99f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f);
        const Mat4 view = look_at(c->position, c->position + c->direction, up);
        const Mat4 proj = to_reverse_z(perspective(2.0f * half, 1.0f, near_z, far_z), near_z);
        SpotShadowSetup sh;
        sh.light = c->light;
        sh.view_proj = proj * view;
        sh.frustum = extract_frustum(sh.view_proj, true, false);
        sh.texel_scale = 2.0f * std::tan(half) / size;
        sh.layer = static_cast<u32>(spot_shadows_.size());
        gpu_lights_[c->light].shadow = static_cast<u32>(spot_shadows_.size());
        spot_shadows_.push_back(sh);
    }
    stats_.spot_shadow_maps = static_cast<u32>(spot_shadows_.size());
}

// ADR-0015: a point light is shadowed by six perspective views (one per cube face, in the order
// +X, -X, +Y, -Y, +Z, -Z of point_shadow_face()). Each face's field of view is slightly wider
// than 90 degrees so the PCF footprint of a receiver near a face edge stays inside the face the
// shader selects (by the major axis of light -> receiver); no seams, no cube-map sampling.
void RendererImpl::setup_point_views() {
    const u32 budget = std::min(settings_.max_point_shadows, kMaxPointShadows);
    if (point_candidates_.empty() || budget == 0) {
        return;
    }
    std::vector<std::pair<f32, const PointCandidate*>> order;
    for (const PointCandidate& c : point_candidates_) {
        if (!frustum_intersects_sphere(view_.frustum, c.position, c.range)) {
            continue;
        }
        order.emplace_back(glm::length(c.position - view_.camera_pos) - c.range, &c);
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (order.size() > budget) {
        order.resize(budget);
    }
    if (order.empty()) {
        return;
    }
    ensure_point_shadow_map();
    if (!point_shadow_map_.texture.is_valid()) {
        return;
    }
    const f32 size = static_cast<f32>(point_shadow_map_.size.x);
    const f32 tan_half = point_shadow_tan_half_fov(point_shadow_map_.size.x);
    u32       slot = 0;
    for (const auto& [key, c] : order) {
        (void)key;
        const f32 near_z = std::clamp(c->range * 0.01f, 0.02f, 0.5f);
        const f32 far_z = std::max(c->range, near_z * 2.0f);
        const Mat4 proj = to_reverse_z(perspective(2.0f * std::atan(tan_half), 1.0f, near_z, far_z), near_z);
        gpu_lights_[c->light].shadow = static_cast<u32>(spot_shadows_.size());
        for (u32 f = 0; f < kPointShadowFaces; ++f) {
            const PointShadowFace face = point_shadow_face(f);
            SpotShadowSetup       sh;
            sh.light = c->light;
            sh.view_proj = proj * look_at(c->position, c->position + face.forward, face.up);
            sh.frustum = extract_frustum(sh.view_proj, true, false);
            sh.texel_scale = 2.0f * tan_half / size;
            sh.layer = slot * kPointShadowFaces + f;
            sh.point = true;
            spot_shadows_.push_back(sh);
        }
        ++slot;
    }
    stats_.point_shadow_maps = slot;
}

void RendererImpl::cull_spot_shadows(const RenderScene& scene) {
    for (auto& l : spot_draws_) {
        l.clear();
    }
    if (spot_shadows_.empty()) {
        return;
    }
    for (u32 i = 0; i < resolved_.size(); ++i) {
        const ResolvedInstance& r = resolved_[i];
        if (!r.valid || !r.caster) {
            continue;
        }
        const bool never_cull = instance_flags::has(scene.instances[i].flags, instance_flags::kNeverCull);
        DrawItem d;
        d.instance = i;
        d.first_index = r.first_index;
        d.index_count = r.index_count;
        d.vertex_offset = r.vertex_offset;
        d.skin_arena = r.skin_arena;
        d.skinned = r.skinned;
        d.double_sided = r.double_sided;
        const bool masked = r.blend == static_cast<u8>(BlendMode::Masked);
        d.pipeline = mesh_pipeline_index(MeshPass::Shadow, r.skinned, masked, r.double_sided);
        d.key = (static_cast<u64>(d.pipeline) << 40) | (static_cast<u64>(d.skin_arena) << 32) | r.material;
        for (u32 s = 0; s < spot_shadows_.size(); ++s) {
            if (never_cull || frustum_intersects_aabb(spot_shadows_[s].frustum, r.bounds)) {
                spot_draws_[s].push_back(d);
            }
        }
    }
    auto by_key = [](const DrawItem& a, const DrawItem& b) { return a.key < b.key; };
    for (u32 s = 0; s < spot_shadows_.size(); ++s) {
        std::sort(spot_draws_[s].begin(), spot_draws_[s].end(), by_key);
    }
}

void RendererImpl::ensure_spot_shadow_map() {
    const u32 size = std::clamp(settings_.spot_shadow_map_size, 128u, 4096u);
    const u32 layers = std::clamp(settings_.max_spot_shadows, 1u, kMaxSpotShadows);
    if (spot_shadow_map_.texture.is_valid() && spot_shadow_map_.size.x == size && spot_shadow_map_.layers == layers) {
        return;
    }
    destroy_persistent(spot_shadow_map_); // device defers destruction past in-flight frames
    rhi::TextureDesc d;
    d.type = rhi::TextureType::Tex2DArray;
    d.format = kShadowFormat;
    d.width = size;
    d.height = size;
    d.array_layers = layers;
    d.usage = rhi::TextureUsage::DepthAttach | rhi::TextureUsage::Sampled;
    d.debug_name = "SpotShadowMaps";
    spot_shadow_map_.texture = device_.create_texture(d);
    if (!spot_shadow_map_.texture.is_valid()) {
        AE_LOG_ERROR("Renderer", "spot shadow map creation failed ({}^2 x {})", size, layers);
        return;
    }
    // Point sampling: the shader gathers and compares itself (no compare sampler, see shadows.glsl).
    spot_shadow_map_.sampled = device_.register_texture(spot_shadow_map_.texture, point_clamp_);
    spot_shadow_map_.size = UVec2(size);
    spot_shadow_map_.layers = layers;
    spot_shadow_map_.state = rhi::ResourceState::Undefined;
}

void RendererImpl::ensure_point_shadow_map() {
    const u32 size = std::clamp(settings_.point_shadow_map_size, 64u, 2048u);
    const u32 layers = std::clamp(settings_.max_point_shadows, 1u, kMaxPointShadows) * kPointShadowFaces;
    if (point_shadow_map_.texture.is_valid() && point_shadow_map_.size.x == size && point_shadow_map_.layers == layers) {
        return;
    }
    destroy_persistent(point_shadow_map_);
    rhi::TextureDesc d;
    d.type = rhi::TextureType::Tex2DArray;
    d.format = kShadowFormat;
    d.width = size;
    d.height = size;
    d.array_layers = layers;
    d.usage = rhi::TextureUsage::DepthAttach | rhi::TextureUsage::Sampled;
    d.debug_name = "PointShadowMaps";
    point_shadow_map_.texture = device_.create_texture(d);
    if (!point_shadow_map_.texture.is_valid()) {
        AE_LOG_ERROR("Renderer", "point shadow map creation failed ({}^2 x {})", size, layers);
        return;
    }
    point_shadow_map_.sampled = device_.register_texture(point_shadow_map_.texture, point_clamp_);
    point_shadow_map_.size = UVec2(size);
    point_shadow_map_.layers = layers;
    point_shadow_map_.state = rhi::ResourceState::Undefined;
}

// ===========================================================================
// GI probe volume (ADR-0016)
// ===========================================================================
void RendererImpl::setup_gi(const RenderScene& scene) {
    gi_.active = false;
    gi_.slots.clear();
    gi_.views.clear();
    stats_.gi_probes = 0;
    stats_.gi_probes_updated = 0;
    const GiVolume& v = scene.gi;
    if (!settings_.gi || !v.enabled || !pipelines_->valid(PipelineId::GiProject) ||
        !pipelines_->get(mesh_pipeline_index(MeshPass::GiCapture, false, false, false)).is_valid()) {
        return;
    }
    const Vec3 lo = glm::min(v.min, v.max);
    const Vec3 hi = glm::max(v.min, v.max);
    if (!(hi.x - lo.x > 1e-3f && hi.y - lo.y > 1e-3f && hi.z - lo.z > 1e-3f)) {
        return;
    }
    UVec3 counts = glm::clamp(v.probe_counts, UVec3(2u), UVec3(kMaxGiProbeAxis));
    while (u64(counts.x) * counts.y * counts.z > kMaxGiProbes) { // shrink the densest axis
        u32& axis = counts.x >= counts.y && counts.x >= counts.z ? counts.x : (counts.y >= counts.z ? counts.y : counts.z);
        axis = std::max(2u, axis - 1u);
    }
    const u32 total = counts.x * counts.y * counts.z;
    if (lo != gi_.min || hi != gi_.max || counts != gi_.counts) {
        gi_.min = lo;
        gi_.max = hi;
        gi_.counts = counts;
        gi_.cursor = 0;
        gi_.clear = true; // stale probes of another volume must not light this one
    }
    const u32 slots = std::min({ std::clamp(settings_.gi_probes_per_frame, 1u, kMaxGiProbesPerFrame), total });
    if (!ensure_gi_resources(total)) {
        return;
    }
    gi_.active = true;
    stats_.gi_probes = total;

    const Vec3 spacing = (hi - lo) / Vec3(counts - UVec3(1u));
    const f32  near_z = 0.02f;
    const f32  far_z = glm::length(hi - lo) + 2.0f * std::max({ spacing.x, spacing.y, spacing.z }) + 1.0f;
    const Mat4 proj = to_reverse_z(perspective(90.0f * kDeg2Rad, 1.0f, near_z, far_z), near_z);
    gi_.cursor %= total;
    for (u32 s = 0; s < slots; ++s) {
        const u32   probe = (gi_.cursor + s) % total;
        const UVec3 c(probe % counts.x, (probe / counts.x) % counts.y, probe / (counts.x * counts.y));
        const Vec3  pos = lo + Vec3(c) * spacing;
        gi_.slots.push_back(probe);
        for (u32 f = 0; f < kPointShadowFaces; ++f) {
            const PointShadowFace face = point_shadow_face(f);
            SpotShadowSetup       vs;
            vs.view_proj = proj * look_at(pos, pos + face.forward, face.up);
            vs.frustum = extract_frustum(vs.view_proj, true, false);
            vs.layer = s * kPointShadowFaces + f;
            gi_.views.push_back(vs);
        }
    }
    gi_.cursor = (gi_.cursor + slots) % total;
    stats_.gi_probes_updated = slots;
}

void RendererImpl::cull_gi(const RenderScene& scene) {
    for (auto& l : gi_draws_) {
        l.clear();
    }
    if (!gi_.active) {
        return;
    }
    // Every opaque / masked instance (casting shadows or not) is seen by the probes.
    for (u32 i = 0; i < resolved_.size(); ++i) {
        const ResolvedInstance& r = resolved_[i];
        if (!r.valid || r.blend == static_cast<u8>(BlendMode::Translucent)) {
            continue;
        }
        const bool never_cull = instance_flags::has(scene.instances[i].flags, instance_flags::kNeverCull);
        DrawItem d;
        d.instance = i;
        d.first_index = r.first_index;
        d.index_count = r.index_count;
        d.vertex_offset = r.vertex_offset;
        d.skin_arena = r.skin_arena;
        d.skinned = r.skinned;
        d.double_sided = r.double_sided;
        d.pipeline = mesh_pipeline_index(MeshPass::GiCapture, r.skinned, false, false);
        d.key = (static_cast<u64>(d.pipeline) << 40) | (static_cast<u64>(d.skin_arena) << 32) | r.material;
        for (u32 v = 0; v < gi_.views.size(); ++v) {
            if (never_cull || frustum_intersects_aabb(gi_.views[v].frustum, r.bounds)) {
                gi_draws_[v].push_back(d);
            }
        }
    }
    auto by_key = [](const DrawItem& a, const DrawItem& b) { return a.key < b.key; };
    for (u32 v = 0; v < gi_.views.size(); ++v) {
        std::sort(gi_draws_[v].begin(), gi_draws_[v].end(), by_key);
    }
}

bool RendererImpl::ensure_gi_resources(u32 probes) {
    if (!gi_.buffer.is_valid() || gi_.capacity < probes) {
        if (gi_.buffer.is_valid()) {
            device_.destroy(gi_.buffer); // deferred past in-flight frames by the device
        }
        rhi::BufferDesc d;
        d.size = static_cast<u64>(probes) * sizeof(GpuGiProbe);
        d.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst;
        d.debug_name = "GI.Probes";
        gi_.buffer = device_.create_buffer(d);
        gi_.capacity = gi_.buffer.is_valid() ? probes : 0u;
        gi_.state = rhi::ResourceState::Undefined;
        gi_.clear = true;
        if (!gi_.buffer.is_valid()) {
            AE_LOG_ERROR("Renderer", "GI probe buffer creation failed ({} probes)", probes);
            return false;
        }
    }
    const u32 size = std::clamp(settings_.gi_capture_size, 8u, 64u);
    const u32 layers = std::clamp(settings_.gi_probes_per_frame, 1u, kMaxGiProbesPerFrame) * kPointShadowFaces;
    if (gi_capture_color_.texture.is_valid() && gi_capture_color_.size.x == size && gi_capture_color_.layers == layers) {
        return gi_capture_depth_.texture.is_valid();
    }
    destroy_persistent(gi_capture_color_);
    destroy_persistent(gi_capture_depth_);
    rhi::TextureDesc d;
    d.type = rhi::TextureType::Tex2DArray;
    d.width = size;
    d.height = size;
    d.array_layers = layers;
    d.format = kHdrFormat;
    d.usage = rhi::TextureUsage::ColorAttach | rhi::TextureUsage::Sampled;
    d.debug_name = "GI.Capture";
    gi_capture_color_.texture = device_.create_texture(d);
    d.format = kDepthFormat;
    d.usage = rhi::TextureUsage::DepthAttach;
    d.debug_name = "GI.CaptureDepth";
    gi_capture_depth_.texture = device_.create_texture(d);
    if (!gi_capture_color_.texture.is_valid() || !gi_capture_depth_.texture.is_valid()) {
        AE_LOG_ERROR("Renderer", "GI capture targets creation failed ({}^2 x {})", size, layers);
        destroy_persistent(gi_capture_color_);
        destroy_persistent(gi_capture_depth_);
        return false;
    }
    gi_capture_color_.sampled = device_.register_texture(gi_capture_color_.texture, point_clamp_);
    for (PersistentTexture* t : { &gi_capture_color_, &gi_capture_depth_ }) {
        t->size = UVec2(size);
        t->layers = layers;
        t->state = rhi::ResourceState::Undefined;
    }
    return true;
}

void RendererImpl::ensure_history() {
    if (history_[0].texture.is_valid() && history_[0].size == output_size_) {
        return;
    }
    for (PersistentTexture& h : history_) {
        destroy_persistent(h);
        rhi::TextureDesc d;
        d.format = kHdrFormat;
        d.width = output_size_.x;
        d.height = output_size_.y;
        d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage;
        d.debug_name = "TAA.History";
        h.texture = device_.create_texture(d);
        if (h.texture.is_valid()) {
            h.sampled = device_.register_texture(h.texture, linear_clamp_);
            h.storage = device_.register_storage_texture(h.texture, 0);
        }
        h.size = output_size_;
        h.state = rhi::ResourceState::Undefined;
    }
    history_valid_ = false;
}

// ===========================================================================
// Frame constants
// ===========================================================================
void RendererImpl::write_frame_constants(const RenderScene& scene, GpuFrame& f, u64 instances,
                                         u64 materials, u64 lights, u64 joints, u64 lines) {
    f.instances = instances;
    f.materials = materials;
    f.lights = lights;
    f.joints = joints;
    f.lines = lines;
    f.skin = device_.buffer_device_address(geometry_.skin_buffer());
    f.view = view_.view;
    f.proj = view_.proj;
    f.view_proj = view_.view_proj;
    f.inv_view = view_.inv_view;
    f.inv_proj = view_.inv_proj;
    f.inv_view_proj = view_.inv_view_proj;
    f.unjittered_view_proj = view_.unjittered_view_proj;
    f.prev_view_proj = has_prev_view_ ? prev_view_proj_ : view_.unjittered_view_proj;
    for (u32 c = 0; c < kMaxCascades; ++c) {
        const bool used = c < cascades_.count;
        f.cascade_view_proj[c] = used ? cascades_.matrices[c].view_proj : Mat4(1.0f);
        f.cascade_splits[static_cast<int>(c)] = used ? cascades_.splits[c] : 0.0f;
        f.cascade_texel_world[static_cast<int>(c)] = used ? cascades_.matrices[c].texel_world : 0.0f;
    }
    f.camera_pos = view_.camera_pos;
    f.near_z = view_.near_z;
    f.viewport = Vec2(output_size_);
    f.inv_viewport = Vec2(1.0f) / Vec2(output_size_);
    f.jitter_ndc = view_.jitter_ndc;
    f.far_z = view_.far_z;
    f.exposure = scene.view.exposure;

    const EnvironmentSettings& env = scene.environment;
    f.ambient = env.ambient_color * env.ambient_intensity;
    f.ibl_intensity = env.ambient_intensity;
    f.light_count = static_cast<u32>(gpu_lights_.size());
    f.directional_count = directional_count_;
    f.cascade_count = cascades_.count;
    f.shadow_map = cascades_.count > 0 ? didx(shadow_map_.sampled) : kGpuInvalidIndex;
    f.brdf_lut = didx(brdf_lut_.sampled);
    const EnvRecord* er = environments_.get(env.skybox);
    const bool ibl = er && settings_.ibl && f.brdf_lut != kGpuInvalidIndex;
    f.irradiance_map = ibl ? didx(er->maps.irradiance.sampled) : kGpuInvalidIndex;
    f.prefiltered_map = ibl ? didx(er->maps.prefiltered.sampled) : kGpuInvalidIndex;
    f.prefiltered_mips = er ? er->maps.prefiltered.mips : 1u;
    f.skybox_map = er ? didx(er->maps.environment.sampled) : kGpuInvalidIndex;
    f.skybox_lod = env.skybox_lod;
    f.debug_view = static_cast<u32>(settings_.debug_view);
    f.flags = kFrameFlagReverseZ | (f.shadow_map != kGpuInvalidIndex ? kFrameFlagShadows : 0u) |
              (ibl ? kFrameFlagIbl : 0u);

    const ClusterParams cp = make_cluster_params(output_size_, view_.near_z, view_.far_z);
    f.cluster_x = kClusterX;
    f.cluster_y = kClusterY;
    f.cluster_z = kClusterZ;
    f.cluster_z_scale = cp.z_scale;
    f.cluster_z_bias = cp.z_bias;
    f.cluster_tile_w = cp.tile_w;
    f.cluster_tile_h = cp.tile_h;
    f.frame_index = static_cast<u32>(frame_counter_);
    const f32 shadow_dist = cascades_.count > 0 ? cascades_.splits[cascades_.count - 1] : 0.0f;
    f.shadow_distance = shadow_dist;
    f.shadow_fade_start = shadow_dist * 0.9f;
    f.shadow_normal_bias = 1.5f;
    f.cascade_blend = 0.1f;
}

// ===========================================================================
// render()
// ===========================================================================
void RendererImpl::render(const RenderScene& scene, rhi::CommandList& cmd, const RenderTarget& target) {
    const f64 t0 = now_seconds();
    ++frame_counter_;
    process_releases(false);
    stats_ = RendererStats{};
    stats_.instances_submitted = static_cast<u32>(scene.instances.size());

    if (!target.texture.is_valid() || target.extent.x == 0 || target.extent.y == 0) {
        AE_LOG_WARN("Renderer", "render(): invalid RenderTarget - frame skipped");
        return;
    }
    const u32 slot = static_cast<u32>(frame_counter_ % frames_in_flight_);
    frame_slot_ = slot;
    collect_readbacks(slot); // results of the frame that used this slot frames_in_flight ago

    // ---- GPU-driven path + picking decisions (ADR-0009) ----
    gpu_.active = gpu_culling_supported();
    gpu_.occlusion = gpu_.active && settings_.occlusion_culling && pipelines_->valid(PipelineId::HiZBuild);
    gpu_.meshlets = gpu_.active && mesh_shading_supported();
    gpu_.candidates = 0;
    if (const i32 mode = (gpu_.active ? (gpu_.occlusion ? 2 : 1) : 0) + (gpu_.meshlets ? 3 : 0);
        mode != logged_cull_mode_) {
        logged_cull_mode_ = mode;
        constexpr const char* kModes[] = { "CPU frustum culling", "GPU frustum culling + indirect draws",
                                           "GPU frustum + two-phase Hi-Z occlusion culling + indirect draws" };
        AE_LOG_INFO("Renderer", "opaque path: {}{}", kModes[mode % 3],
                    gpu_.meshlets ? "; static meshes as meshlets (task + mesh shaders, meshlet frustum + cone culling)"
                                  : "");
    }
    pick_this_frame_ = false;
    if (pending_pick_) {
        const UVec2 p = *pending_pick_;
        pending_pick_.reset();
        if (p.x >= target.extent.x || p.y >= target.extent.y) {
            pick_result_ = 0u; // outside the image: nothing pickable there
        } else if (pipelines_->valid(PipelineId::PickResolve)) {
            // RenderTarget pixel -> internal-resolution pixel (the tonemap pass scales).
            const Vec2 uv = (Vec2(p) + 0.5f) / Vec2(target.extent);
            pick_pixel_ = glm::min(UVec2(uv * Vec2(output_size_)), output_size_ - UVec2(1));
            pick_this_frame_ = true;
        } else {
            AE_LOG_WARN("Renderer", "request_pick: picking shaders unavailable - request dropped");
        }
    }

    cmd.push_debug_group("Renderer");
    geometry_.record_pending_copies(cmd);
    if (materials_dirty_ || material_table_.size() != static_cast<usize>(materials_.capacity()) + 1) {
        rebuild_material_table();
    }

    setup_view(scene);
    setup_lights(scene);

    // ---- per-frame upload arena (sized up front; nothing grows mid-frame) ----
    const usize n_inst = scene.instances.size();
    const usize n_joints = scene.joint_matrices.size();
    const bool  lines_on = settings_.draw_debug_lines && !scene.debug_lines.empty() &&
                          pipelines_->valid(PipelineId::DebugLines);
    line_vertex_count_ = lines_on ? static_cast<u32>(scene.debug_lines.size() * 2) : 0u;
    const u64 bytes = FrameArena::padded(sizeof(GpuFrame)) +
                      FrameArena::padded(std::max<usize>(n_inst, 1) * sizeof(GpuInstance)) +
                      FrameArena::padded(material_table_.size() * sizeof(GpuMaterial)) +
                      FrameArena::padded(std::max<usize>(gpu_lights_.size(), 1) * sizeof(GpuLight)) +
                      FrameArena::padded(std::max<usize>(n_joints, 1) * sizeof(Mat4)) +
                      FrameArena::padded(std::max<u32>(line_vertex_count_, 1) * sizeof(GpuLineVertex)) +
                      FrameArena::padded(std::max<usize>(n_inst, 1) * sizeof(GpuCullInstance)) +
                      FrameArena::padded(sizeof(GpuCullView)) +
                      FrameArena::padded(std::max<usize>(n_inst, 1) * sizeof(u32)) +
                      FrameArena::padded(kMaxViewTableEntries * sizeof(GpuSpotShadow)) +
                      FrameArena::padded(kMaxGiProbesPerFrame * sizeof(u32)) +
                      FrameArena::kAlignment * 14;
    if (!frame_arena_.begin_frame(slot, bytes)) {
        cmd.pop_debug_group();
        return;
    }
    const auto a_frame = frame_arena_.allocate(sizeof(GpuFrame));
    const auto a_inst = frame_arena_.allocate_array<GpuInstance>(std::max<usize>(n_inst, 1));
    const auto a_mat = frame_arena_.allocate_array<GpuMaterial>(material_table_.size());
    const auto a_light = frame_arena_.allocate_array<GpuLight>(std::max<usize>(gpu_lights_.size(), 1));
    const auto a_joint = frame_arena_.allocate_array<Mat4>(std::max<usize>(n_joints, 1));
    const auto a_line = frame_arena_.allocate_array<GpuLineVertex>(std::max<u32>(line_vertex_count_, 1));
    const auto a_cull = frame_arena_.allocate_array<GpuCullInstance>(std::max<usize>(n_inst, 1));
    const auto a_cull_view = frame_arena_.allocate(sizeof(GpuCullView));
    const auto a_uid = frame_arena_.allocate_array<u32>(std::max<usize>(n_inst, 1));
    const auto a_spot = frame_arena_.allocate_array<GpuSpotShadow>(kMaxViewTableEntries);
    const auto a_gi_slots = frame_arena_.allocate_array<u32>(kMaxGiProbesPerFrame);
    if (!a_frame.valid() || !a_inst.valid() || !a_mat.valid() || !a_light.valid() || !a_joint.valid() ||
        !a_line.valid() || !a_cull.valid() || !a_cull_view.valid() || !a_uid.valid() || !a_spot.valid() ||
        !a_gi_slots.valid()) {
        cmd.pop_debug_group();
        return;
    }

    // ---- CPU culling (parallel) ----
    resolve_instances(scene, static_cast<GpuInstance*>(a_inst.cpu),
                      pick_this_frame_ ? static_cast<u32*>(a_uid.cpu) : nullptr);
    setup_cascades();
    cull_cascades(scene);
    build_draw_lists(scene);
    setup_spot_shadows(); // assigns GpuLight::shadow before the light upload below
    cull_spot_shadows(scene);
    setup_gi(scene);
    cull_gi(scene);
    if (gpu_.active) {
        write_cull_data(static_cast<GpuCullInstance*>(a_cull.cpu), static_cast<GpuCullView*>(a_cull_view.cpu));
        gpu_.candidates_gpu = a_cull.gpu;
        gpu_.view_gpu = a_cull_view.gpu;
        if (gpu_.occlusion) {
            ensure_visibility_buffer(static_cast<u32>(n_inst));
            gpu_.occlusion = visibility_buffer_.is_valid();
        }
    }
    if (gpu_.active || pick_this_frame_) {
        ensure_readback_buffer();
    }

    // ---- uploads ----
    std::memcpy(a_mat.cpu, material_table_.data(), material_table_.size() * sizeof(GpuMaterial));
    if (!gpu_lights_.empty()) {
        std::memcpy(a_light.cpu, gpu_lights_.data(), gpu_lights_.size() * sizeof(GpuLight));
    }
    if (n_joints > 0) {
        std::memcpy(a_joint.cpu, scene.joint_matrices.data(), n_joints * sizeof(Mat4));
    }
    if (line_vertex_count_ > 0) {
        auto* dst = static_cast<GpuLineVertex*>(a_line.cpu);
        for (usize i = 0; i < scene.debug_lines.size(); ++i) {
            const RenderLine& l = scene.debug_lines[i];
            dst[2 * i] = GpuLineVertex{ l.a, l.color_a };
            dst[2 * i + 1] = GpuLineVertex{ l.b, l.color_b };
        }
    }
    if (cascades_.count > 0) {
        ensure_shadow_map();
        if (!shadow_map_.texture.is_valid()) {
            cascades_.count = 0;
        }
    }
    GpuFrame frame;
    write_frame_constants(scene, frame, a_inst.gpu, a_mat.gpu, a_light.gpu, a_joint.gpu, a_line.gpu);
    frame.user_ids = a_uid.gpu;
    if (!spot_shadows_.empty()) { // ADR-0012
        auto* dst = static_cast<GpuSpotShadow*>(a_spot.cpu);
        for (u32 i = 0; i < spot_shadows_.size(); ++i) {
            GpuSpotShadow g;
            g.view_proj = spot_shadows_[i].view_proj;
            g.texel_scale = spot_shadows_[i].texel_scale;
            g.layer = spot_shadows_[i].layer;
            g.map = didx(spot_shadows_[i].point ? point_shadow_map_.sampled : spot_shadow_map_.sampled);
            std::memcpy(&dst[i], &g, sizeof(GpuSpotShadow));
        }
        frame.spot_shadows = a_spot.gpu;
        frame.spot_shadow_map = stats_.spot_shadow_maps > 0 ? didx(spot_shadow_map_.sampled) : kGpuInvalidIndex;
        frame.spot_shadow_count = static_cast<u32>(spot_shadows_.size());
    }
    if (gi_.active) { // ADR-0016: capture views follow the shadow views in the same table
        auto* dst = static_cast<GpuSpotShadow*>(a_spot.cpu) + kMaxLocalShadowViews;
        for (u32 i = 0; i < gi_.views.size(); ++i) {
            GpuSpotShadow g;
            g.view_proj = gi_.views[i].view_proj;
            std::memcpy(&dst[i], &g, sizeof(GpuSpotShadow));
        }
        std::memcpy(a_gi_slots.cpu, gi_.slots.data(), gi_.slots.size() * sizeof(u32));
        gi_.slots_gpu = a_gi_slots.gpu;
        const Vec3 spacing = (gi_.max - gi_.min) / Vec3(gi_.counts - UVec3(1u));
        frame.spot_shadows = a_spot.gpu;
        frame.gi_probes = device_.buffer_device_address(gi_.buffer);
        frame.gi_min = gi_.min;
        frame.gi_intensity = std::max(scene.gi.intensity, 0.0f);
        frame.gi_inv_spacing = Vec3(1.0f) / spacing;
        frame.gi_normal_bias = 0.25f * std::min({ spacing.x, spacing.y, spacing.z });
        frame.gi_counts = gi_.counts;
    }
    std::memcpy(a_frame.cpu, &frame, sizeof(GpuFrame));
    frame_gpu_address_ = a_frame.gpu;
    stats_.lights = static_cast<u32>(gpu_lights_.size());

    // ---- GPU passes ----
    build_graph(scene, target);
    graph_->execute(cmd);
    rg_pool_->end_frame();

    prev_view_proj_ = view_.unjittered_view_proj;
    has_prev_view_ = true;
    if (gpu_.active) {
        // GPU statistics arrive frames_in_flight frames late (never a stall).
        stats_.instances_gpu_frustum_culled = gpu_stats_latched_[0];
        stats_.instances_gpu_occlusion_culled = gpu_.occlusion ? gpu_stats_latched_[1] : 0u;
        stats_.instances_visible = cpu_visible_ + gpu_stats_latched_[2];
        stats_.triangles += gpu_stats_latched_[3];
    }
    cmd.pop_debug_group();
    stats_.cpu_record_ms = (now_seconds() - t0) * 1000.0;
}

// ===========================================================================
// Render graph
// ===========================================================================
void RendererImpl::build_graph(const RenderScene& scene, const RenderTarget& target) {
    (void)scene;
    RenderGraph& g = *graph_;
    g.reset();
    const UVec2 size = output_size_;
    const u64   frame_addr = frame_gpu_address_;
    const bool  debug = debug_view_active();
    const bool  overdraw = settings_.debug_view == DebugView::Overdraw;

    RGTextureDesc out_desc;
    out_desc.format = target.format;
    out_desc.width = target.extent.x;
    out_desc.height = target.extent.y;
    const RGTexture out = g.import_texture(target.texture, out_desc, target.initial_state,
                                           target.final_state, "RenderTarget");

    // ---- cascaded shadow maps ----
    RGTexture shadow;
    if (cascades_.count > 0) {
        RGTextureDesc sd;
        sd.type = rhi::TextureType::Tex2DArray;
        sd.format = kShadowFormat;
        sd.width = sd.height = shadow_map_.size.x;
        sd.array_layers = shadow_map_.layers;
        shadow = g.import_texture(shadow_map_.texture, sd, shadow_map_.state, rhi::ResourceState::ShaderRead,
                                  "CascadedShadowMap", didx(shadow_map_.sampled));
        g.add_pass("CascadedShadows")
            .write(shadow, rhi::ResourceState::DepthStencilAttachment)
            .execute([this, frame_addr](RGContext& ctx) {
                const UVec2 ss = shadow_map_.size;
                for (u32 c = 0; c < cascades_.count; ++c) {
                    ctx.cmd.push_debug_group(kCascadeNames[c]);
                    rhi::RenderingInfo ri;
                    ri.render_area = ss;
                    ri.has_depth = true;
                    ri.depth.texture = shadow_map_.texture;
                    ri.depth.layer = c;
                    ri.depth.load = rhi::LoadOp::Clear;
                    ri.depth.store = rhi::StoreOp::Store;
                    ri.depth.clear_depth = 0.0f; // reverse-Z
                    ctx.cmd.begin_rendering(ri);
                    ctx.cmd.set_viewport(full_viewport(ss));
                    ctx.cmd.set_scissor(full_scissor(ss));
                    // Reverse-Z: negative bias pushes occluders away from the light. The
                    // slope term scales with the per-pixel depth gradient; the shader adds a
                    // normal offset scaled by this cascade's world texel size.
                    ctx.cmd.set_depth_bias(-(1.0f + static_cast<f32>(c)), 0.0f, -2.0f);
                    MeshPush push;
                    push.frame = frame_addr;
                    push.view_index = 1 + c;
                    push.pass_flags = kPassFlagShadow;
                    draw_items(ctx.cmd, cascade_draws_[c], push, false);
                    ctx.cmd.end_rendering();
                    ctx.cmd.pop_debug_group();
                }
            });
    }

    // ---- spot-light shadow maps (ADR-0012) ----
    RGTexture spot_shadow;
    if (stats_.spot_shadow_maps > 0) {
        RGTextureDesc sd;
        sd.type = rhi::TextureType::Tex2DArray;
        sd.format = kShadowFormat;
        sd.width = sd.height = spot_shadow_map_.size.x;
        sd.array_layers = spot_shadow_map_.layers;
        spot_shadow = g.import_texture(spot_shadow_map_.texture, sd, spot_shadow_map_.state,
                                       rhi::ResourceState::ShaderRead, "SpotShadowMaps", didx(spot_shadow_map_.sampled));
        g.add_pass("SpotShadows")
            .write(spot_shadow, rhi::ResourceState::DepthStencilAttachment)
            .execute([this, frame_addr](RGContext& ctx) {
                const UVec2 ss = spot_shadow_map_.size;
                for (u32 s = 0; s < spot_shadows_.size(); ++s) {
                    if (spot_shadows_[s].point) {
                        continue;
                    }
                    ctx.cmd.push_debug_group("SpotShadow");
                    rhi::RenderingInfo ri;
                    ri.render_area = ss;
                    ri.has_depth = true;
                    ri.depth.texture = spot_shadow_map_.texture;
                    ri.depth.layer = spot_shadows_[s].layer;
                    ri.depth.load = rhi::LoadOp::Clear;
                    ri.depth.store = rhi::StoreOp::Store;
                    ri.depth.clear_depth = 0.0f; // reverse-Z
                    ctx.cmd.begin_rendering(ri);
                    ctx.cmd.set_viewport(full_viewport(ss));
                    ctx.cmd.set_scissor(full_scissor(ss));
                    // Reverse-Z: negative bias pushes occluders away from the light; the shader
                    // adds a normal offset scaled by the texel size at the receiver's distance.
                    ctx.cmd.set_depth_bias(-1.0f, 0.0f, -1.5f);
                    MeshPush push;
                    push.frame = frame_addr;
                    push.view_index = 1 + kMaxCascades + s;
                    push.pass_flags = kPassFlagShadow;
                    draw_items(ctx.cmd, spot_draws_[s], push, false);
                    ctx.cmd.end_rendering();
                    ctx.cmd.pop_debug_group();
                }
            });
    }

    // ---- point-light shadow maps: 6 cube-face views per light (ADR-0015) ----
    RGTexture point_shadow;
    if (stats_.point_shadow_maps > 0) {
        RGTextureDesc pd;
        pd.type = rhi::TextureType::Tex2DArray;
        pd.format = kShadowFormat;
        pd.width = pd.height = point_shadow_map_.size.x;
        pd.array_layers = point_shadow_map_.layers;
        point_shadow = g.import_texture(point_shadow_map_.texture, pd, point_shadow_map_.state,
                                        rhi::ResourceState::ShaderRead, "PointShadowMaps", didx(point_shadow_map_.sampled));
        g.add_pass("PointShadows")
            .write(point_shadow, rhi::ResourceState::DepthStencilAttachment)
            .execute([this, frame_addr](RGContext& ctx) {
                const UVec2 ps = point_shadow_map_.size;
                for (u32 s = 0; s < spot_shadows_.size(); ++s) {
                    if (!spot_shadows_[s].point) {
                        continue;
                    }
                    ctx.cmd.push_debug_group("PointShadowFace");
                    rhi::RenderingInfo ri;
                    ri.render_area = ps;
                    ri.has_depth = true;
                    ri.depth.texture = point_shadow_map_.texture;
                    ri.depth.layer = spot_shadows_[s].layer;
                    ri.depth.load = rhi::LoadOp::Clear;
                    ri.depth.store = rhi::StoreOp::Store;
                    ri.depth.clear_depth = 0.0f; // reverse-Z
                    ctx.cmd.begin_rendering(ri);
                    ctx.cmd.set_viewport(full_viewport(ps));
                    ctx.cmd.set_scissor(full_scissor(ps));
                    ctx.cmd.set_depth_bias(-1.0f, 0.0f, -1.5f); // as the spot maps
                    MeshPush push;
                    push.frame = frame_addr;
                    push.view_index = 1 + kMaxCascades + s;
                    push.pass_flags = kPassFlagShadow;
                    draw_items(ctx.cmd, spot_draws_[s], push, false);
                    ctx.cmd.end_rendering();
                    ctx.cmd.pop_debug_group();
                }
            });
    }

    // ---- GI probe capture + projection (ADR-0016) ----
    RGBuffer gi_probes;
    if (gi_.active) {
        gi_probes = g.import_buffer(gi_.buffer, static_cast<u64>(gi_.capacity) * sizeof(GpuGiProbe), gi_.state,
                                    rhi::ResourceState::ShaderRead, "GI.Probes");
        gi_.state = rhi::ResourceState::ShaderRead;
        if (gi_.clear) {
            gi_.clear = false;
            g.add_pass("GI.Clear")
                .write(gi_probes, rhi::ResourceState::TransferDst)
                .execute([gi_probes](RGContext& ctx) { ctx.cmd.fill_buffer(ctx.buffer(gi_probes), 0, ~0ull, 0u); });
        }
        RGTextureDesc cd;
        cd.type = rhi::TextureType::Tex2DArray;
        cd.format = kHdrFormat;
        cd.width = cd.height = gi_capture_color_.size.x;
        cd.array_layers = gi_capture_color_.layers;
        const RGTexture capture = g.import_texture(gi_capture_color_.texture, cd, gi_capture_color_.state,
                                                   rhi::ResourceState::ShaderRead, "GI.Capture",
                                                   didx(gi_capture_color_.sampled));
        gi_capture_color_.state = rhi::ResourceState::ShaderRead;
        cd.format = kDepthFormat;
        const RGTexture capture_depth = g.import_texture(gi_capture_depth_.texture, cd, gi_capture_depth_.state,
                                                         rhi::ResourceState::DepthStencilAttachment, "GI.CaptureDepth");
        gi_capture_depth_.state = rhi::ResourceState::DepthStencilAttachment;
        g.add_pass("GI.Capture")
            .write(capture, rhi::ResourceState::ColorAttachment)
            .write(capture_depth, rhi::ResourceState::DepthStencilAttachment)
            .read(gi_probes, rhi::ResourceState::ShaderRead)
            .read(shadow, rhi::ResourceState::ShaderRead)
            .read(spot_shadow, rhi::ResourceState::ShaderRead)
            .read(point_shadow, rhi::ResourceState::ShaderRead)
            .execute([this, frame_addr](RGContext& ctx) {
                const UVec2 cs = gi_capture_color_.size;
                for (u32 v = 0; v < gi_.views.size(); ++v) {
                    ctx.cmd.push_debug_group("GI.CaptureFace");
                    rhi::RenderingInfo ri;
                    ri.render_area = cs;
                    rhi::ColorAttachment ca;
                    ca.texture = gi_capture_color_.texture;
                    ca.layer = gi_.views[v].layer;
                    ca.load = rhi::LoadOp::Clear;
                    ca.clear_color = Vec4(0.0f); // alpha 0 = nothing hit (sky)
                    ri.color.push_back(ca);
                    ri.has_depth = true;
                    ri.depth.texture = gi_capture_depth_.texture;
                    ri.depth.layer = gi_.views[v].layer;
                    ri.depth.load = rhi::LoadOp::Clear;
                    ri.depth.store = rhi::StoreOp::DontCare;
                    ri.depth.clear_depth = 0.0f; // reverse-Z
                    ctx.cmd.begin_rendering(ri);
                    ctx.cmd.set_viewport(full_viewport(cs));
                    ctx.cmd.set_scissor(full_scissor(cs));
                    MeshPush push;
                    push.frame = frame_addr;
                    push.view_index = 1 + kMaxCascades + kMaxLocalShadowViews + v;
                    draw_items(ctx.cmd, gi_draws_[v], push, false);
                    ctx.cmd.end_rendering();
                    ctx.cmd.pop_debug_group();
                }
            });
        const u64 slots_addr = gi_.slots_gpu;
        g.add_pass("GI.Project")
            .read(capture, rhi::ResourceState::ShaderRead)
            .write(gi_probes, rhi::ResourceState::ShaderWrite)
            .execute([this, frame_addr, gi_probes, capture, slots_addr](RGContext& ctx) {
                ctx.cmd.bind_pipeline(pipelines_->get(PipelineId::GiProject));
                GiProjectPush p;
                p.frame = frame_addr;
                p.probes = ctx.address(gi_probes);
                p.slots = slots_addr;
                p.capture_tex = ctx.sampled(capture);
                p.size = gi_capture_color_.size.x;
                p.view_base = kMaxLocalShadowViews;
                ctx.cmd.push_constants(rhi::ShaderStage::Compute, 0, sizeof(p), &p);
                ctx.cmd.dispatch(static_cast<u32>(gi_.slots.size()), 1, 1);
            });
    }

    // ---- GPU-driven culling buffers (ADR-0009) ----
    // Counters: per-batch draw counts of both phases + statistics + pick id (kCounter*).
    const bool gpu = gpu_.active;
    const bool occlusion = gpu && gpu_.occlusion;
    RGBuffer   counters;
    RGBuffer   draws;
    RGBuffer   visibility;
    if (gpu || pick_this_frame_) {
        RGBufferDesc cd;
        cd.size = static_cast<u64>(kCounterWords) * sizeof(u32);
        counters = g.create_buffer(cd, "GpuCull.Counters");
        g.add_pass("GpuCull.Reset")
            .write(counters, rhi::ResourceState::TransferDst)
            .execute([counters](RGContext& ctx) { ctx.cmd.fill_buffer(ctx.buffer(counters), 0, ~0ull, 0u); });
    }
    if (gpu) {
        RGBufferDesc dcd;
        dcd.size = static_cast<u64>(gpu_.cmd_capacity) * 2 * sizeof(GpuDrawCommand); // phase 1 | phase 2
        draws = g.create_buffer(dcd, "GpuCull.DrawCommands");
        if (occlusion) {
            visibility = g.import_buffer(visibility_buffer_, static_cast<u64>(visibility_capacity_) * sizeof(u32),
                                         visibility_state_, rhi::ResourceState::ShaderWrite, "GpuCull.Visibility");
            visibility_state_ = rhi::ResourceState::ShaderWrite;
            if (visibility_clear_) {
                // Nothing was visible "last frame": phase 1 draws nothing, phase 2 tests all.
                visibility_clear_ = false;
                g.add_pass("GpuCull.VisibilityReset")
                    .write(visibility, rhi::ResourceState::TransferDst)
                    .execute([visibility](RGContext& ctx) { ctx.cmd.fill_buffer(ctx.buffer(visibility), 0, ~0ull, 0u); });
            }
        }
    }
    // One culling dispatch. Phase 2 writes the second command/count region.
    auto add_cull_pass = [&](const char* name, u32 phase, RGTexture hzb) {
        RGPassBuilder pass = g.add_pass(name);
        pass.write(draws, rhi::ResourceState::ShaderWrite).write(counters, rhi::ResourceState::ShaderWrite);
        if (visibility.valid()) {
            if (phase == kCullPhase2) {
                pass.write(visibility, rhi::ResourceState::ShaderWrite);
            } else {
                pass.read(visibility, rhi::ResourceState::ShaderRead);
            }
        }
        if (hzb.valid()) {
            pass.read(hzb, rhi::ResourceState::ShaderRead);
        }
        pass.execute([this, draws, counters, visibility, phase, hzb](RGContext& ctx) {
            if (hzb.valid() && gpu_.view_cpu != nullptr) {
                gpu_.view_cpu->hzb_tex = ctx.sampled(hzb); // host-visible arena, read at submit
            }
            ctx.cmd.bind_pipeline(pipelines_->get(PipelineId::GpuCull));
            const bool second = phase == kCullPhase2;
            CullPush   c;
            c.candidates = gpu_.candidates_gpu;
            c.view = gpu_.view_gpu;
            c.draws = ctx.address(draws) + (second ? static_cast<u64>(gpu_.cmd_capacity) * sizeof(GpuDrawCommand) : 0u);
            c.counts = ctx.address(counters) + static_cast<u64>(second ? kCounterPhase2 : kCounterPhase1) * sizeof(u32);
            c.stats = ctx.address(counters);
            c.visibility = visibility.valid() ? ctx.address(visibility) : 0u;
            c.count = gpu_.candidates;
            c.phase = phase;
            ctx.cmd.push_constants(rhi::ShaderStage::Compute, 0, sizeof(c), &c);
            ctx.cmd.dispatch(groups(std::max(gpu_.candidates, 1u), kCullGroupSize), 1, 1);
        });
    };
    if (gpu) {
        add_cull_pass(occlusion ? "GpuCull.Phase1" : "GpuCull", occlusion ? kCullPhase1 : kCullPhaseFrustum, RGTexture{});
    }

    // ---- depth prepass (reverse-Z: clear 0, GreaterEqual) ----
    RGTextureDesc dd;
    dd.format = kDepthFormat;
    dd.width = size.x;
    dd.height = size.y;
    const RGTexture depth = g.create_texture(dd, "SceneDepth");
    {
        RGPassBuilder pre = g.add_pass("DepthPrepass");
        pre.write(depth, rhi::ResourceState::DepthStencilAttachment);
        if (gpu) {
            pre.read(draws, rhi::ResourceState::IndirectArgument).read(counters, rhi::ResourceState::IndirectArgument);
        }
        pre.execute([this, depth, size, frame_addr, gpu, draws, counters](RGContext& ctx) {
            rhi::RenderingInfo ri;
            ri.render_area = size;
            ri.has_depth = true;
            ri.depth.texture = ctx.texture(depth);
            ri.depth.load = rhi::LoadOp::Clear;
            ri.depth.clear_depth = 0.0f;
            ctx.cmd.begin_rendering(ri);
            ctx.cmd.set_viewport(full_viewport(size));
            ctx.cmd.set_scissor(full_scissor(size));
            MeshPush push;
            push.frame = frame_addr;
            if (gpu) {
                draw_gpu_batches(ctx.cmd, MeshPass::Depth, ctx.buffer(draws), ctx.buffer(counters), push, true, false);
            } else {
                draw_items(ctx.cmd, prepass_draws_, push, false);
            }
            ctx.cmd.end_rendering();
        });
    }

    // ---- two-phase Hi-Z occlusion culling (ADR-0009) ----
    if (occlusion) {
        // Mip 0 = largest power of two <= the depth extent per axis; full chain to 1x1.
        const UVec2 hzb0(std::bit_floor(size.x), std::bit_floor(size.y));
        const u32   hzb_mips = static_cast<u32>(std::bit_width(std::max(hzb0.x, hzb0.y)));
        RGTextureDesc hd;
        hd.format = rhi::Format::R32F;
        hd.width = hzb0.x;
        hd.height = hzb0.y;
        hd.mip_levels = hzb_mips;
        const RGTexture hzb = g.create_texture(hd, "HiZ");
        if (gpu_.view_cpu != nullptr) {
            gpu_.view_cpu->hzb_size = hzb0;
            gpu_.view_cpu->hzb_mips = hzb_mips;
        }
        g.add_pass("HiZ.Build")
            .read(depth, rhi::ResourceState::ShaderRead)
            .write(hzb, rhi::ResourceState::ShaderWrite)
            .execute([this, depth, hzb, size, hzb0, hzb_mips](RGContext& ctx) {
                ctx.cmd.bind_pipeline(pipelines_->get(PipelineId::HiZBuild));
                UVec2 src_size = size;
                for (u32 m = 0; m < hzb_mips; ++m) {
                    const UVec2 dst_size = glm::max(UVec2(hzb0.x >> m, hzb0.y >> m), UVec2(1));
                    HiZPush     p;
                    p.src = m == 0 ? ctx.sampled(depth) : ctx.storage(hzb, m - 1);
                    p.dst = ctx.storage(hzb, m);
                    p.src_size = src_size;
                    p.dst_size = dst_size;
                    p.from_depth = m == 0 ? 1u : 0u;
                    ctx.cmd.push_constants(rhi::ShaderStage::Compute, 0, sizeof(p), &p);
                    ctx.cmd.dispatch(groups(dst_size.x, kHiZGroupSize), groups(dst_size.y, kHiZGroupSize), 1);
                    if (m + 1 < hzb_mips) {
                        // Next level reads this one (whole-image barrier; all mips are General).
                        ctx.cmd.barrier(ctx.texture(hzb), rhi::ResourceState::ShaderWrite, rhi::ResourceState::ShaderWrite);
                    }
                    src_size = dst_size;
                }
            });
        add_cull_pass("GpuCull.Phase2", kCullPhase2, hzb);
        g.add_pass("DepthPrepass.Phase2")
            .write(depth, rhi::ResourceState::DepthStencilAttachment)
            .read(draws, rhi::ResourceState::IndirectArgument)
            .read(counters, rhi::ResourceState::IndirectArgument)
            .execute([this, depth, size, frame_addr, draws, counters](RGContext& ctx) {
                rhi::RenderingInfo ri;
                ri.render_area = size;
                ri.has_depth = true;
                ri.depth.texture = ctx.texture(depth);
                ri.depth.load = rhi::LoadOp::Load;
                ri.depth.store = rhi::StoreOp::Store;
                ctx.cmd.begin_rendering(ri);
                ctx.cmd.set_viewport(full_viewport(size));
                ctx.cmd.set_scissor(full_scissor(size));
                MeshPush push;
                push.frame = frame_addr;
                draw_gpu_batches(ctx.cmd, MeshPass::Depth, ctx.buffer(draws), ctx.buffer(counters), push, false, true);
                ctx.cmd.end_rendering();
            });
    }

    // ---- picking: R32Uint id buffer over the final depth, one texel read back (ADR-0009) ----
    if (pick_this_frame_) {
        RGTextureDesc pd;
        pd.format = kPickIdFormat;
        pd.width = size.x;
        pd.height = size.y;
        const RGTexture ids = g.create_texture(pd, "Pick.Ids");
        RGPassBuilder   pick = g.add_pass("Pick.Ids");
        pick.write(ids, rhi::ResourceState::ColorAttachment).read(depth, rhi::ResourceState::DepthStencilAttachment);
        if (gpu) {
            pick.read(draws, rhi::ResourceState::IndirectArgument).read(counters, rhi::ResourceState::IndirectArgument);
        }
        pick.execute([this, ids, depth, size, frame_addr, gpu, draws, counters](RGContext& ctx) {
            rhi::RenderingInfo ri;
            ri.render_area = size;
            ri.color.push_back(rhi::ColorAttachment{ ctx.texture(ids), 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                                     Vec4(0.0f) }); // 0 = nothing pickable
            ri.has_depth = true;
            ri.depth.texture = ctx.texture(depth);
            ri.depth.load = rhi::LoadOp::Load;
            ri.depth.store = rhi::StoreOp::Store;
            ctx.cmd.begin_rendering(ri);
            ctx.cmd.set_viewport(full_viewport(size));
            ctx.cmd.set_scissor(full_scissor(size));
            MeshPush push;
            push.frame = frame_addr;
            if (gpu) {
                draw_gpu_batches(ctx.cmd, MeshPass::Pick, ctx.buffer(draws), ctx.buffer(counters), push, true, true);
            } else {
                std::vector<DrawItem> items(prepass_draws_.begin(), prepass_draws_.end());
                for (DrawItem& d : items) {
                    d.pipeline = mesh_pipeline_index(MeshPass::Pick, d.skinned, false, d.double_sided);
                }
                draw_items(ctx.cmd, items, push, false);
            }
            ctx.cmd.end_rendering();
        });
        const UVec2 pixel = pick_pixel_;
        g.add_pass("Pick.Resolve")
            .write(ids, rhi::ResourceState::ShaderWrite) // imageLoad (storage images live in General)
            .write(counters, rhi::ResourceState::ShaderWrite)
            .execute([this, ids, counters, pixel](RGContext& ctx) {
                ctx.cmd.bind_pipeline(pipelines_->get(PipelineId::PickResolve));
                PickPush p;
                p.counters = ctx.address(counters);
                p.id_img = ctx.storage(ids);
                p.pixel = pixel;
                ctx.cmd.push_constants(rhi::ShaderStage::Compute, 0, sizeof(p), &p);
                ctx.cmd.dispatch(1, 1, 1);
            });
    }

    // ---- SSAO (half resolution + separable bilateral blur) ----
    RGTexture ao;
    const bool ssao_on = settings_.ssao && pipelines_->valid(PipelineId::Ssao) &&
                         pipelines_->valid(PipelineId::SsaoBlur) &&
                         (!debug || settings_.debug_view == DebugView::AmbientOcclusion);
    if (ssao_on) {
        const UVec2 half = glm::max((size + UVec2(1)) / 2u, UVec2(1));
        RGTextureDesc ad;
        ad.format = rhi::Format::RG16F;
        ad.width = half.x;
        ad.height = half.y;
        const RGTexture ao_raw = g.create_texture(ad, "SSAO.Raw");
        const RGTexture ao_tmp = g.create_texture(ad, "SSAO.BlurH");
        ao = g.create_texture(ad, "SSAO"); // aliases SSAO.Raw (disjoint lifetimes)
        g.add_pass("SSAO")
            .read(depth, rhi::ResourceState::ShaderRead)
            .write(ao_raw, rhi::ResourceState::ShaderWrite)
            .execute([this, depth, ao_raw, half, frame_addr](RGContext& ctx) {
                ctx.cmd.bind_pipeline(pipelines_->get(PipelineId::Ssao));
                SsaoPush p;
                p.frame = frame_addr;
                p.depth_tex = ctx.sampled(depth);
                p.out_img = ctx.storage(ao_raw);
                p.out_size = half;
                ctx.cmd.push_constants(rhi::ShaderStage::Compute, 0, sizeof(p), &p);
                ctx.cmd.dispatch(groups(half.x), groups(half.y), 1);
            });
        const RGTexture blur_src[2] = { ao_raw, ao_tmp };
        const RGTexture blur_dst[2] = { ao_tmp, ao };
        for (int pass = 0; pass < 2; ++pass) {
            const RGTexture src = blur_src[pass];
            const RGTexture dst = blur_dst[pass];
            g.add_pass(pass == 0 ? "SSAO.BlurH" : "SSAO.BlurV")
                .read(src, rhi::ResourceState::ShaderRead)
                .write(dst, rhi::ResourceState::ShaderWrite)
                .execute([this, src, dst, half, pass](RGContext& ctx) {
                    ctx.cmd.bind_pipeline(pipelines_->get(PipelineId::SsaoBlur));
                    SsaoBlurPush p;
                    p.src_tex = ctx.sampled(src);
                    p.dst_img = ctx.storage(dst);
                    p.size = half;
                    p.direction = pass == 0 ? IVec2(1, 0) : IVec2(0, 1);
                    ctx.cmd.push_constants(rhi::ShaderStage::Compute, 0, sizeof(p), &p);
                    ctx.cmd.dispatch(groups(half.x), groups(half.y), 1);
                });
        }
    }

    // ---- clustered light assignment ----
    RGBufferDesc grid_desc;
    grid_desc.size = static_cast<u64>(kClusterCount) * sizeof(u32);
    RGBufferDesc index_desc;
    index_desc.size = static_cast<u64>(kClusterCount) * kMaxLightsPerCluster * sizeof(u32);
    const RGBuffer light_grid = g.create_buffer(grid_desc, "LightGrid");
    const RGBuffer light_indices = g.create_buffer(index_desc, "LightIndices");
    if (pipelines_->valid(PipelineId::LightCull)) {
        g.add_pass("LightCulling")
            .write(light_grid, rhi::ResourceState::ShaderWrite)
            .write(light_indices, rhi::ResourceState::ShaderWrite)
            .execute([this, light_grid, light_indices, frame_addr](RGContext& ctx) {
                ctx.cmd.bind_pipeline(pipelines_->get(PipelineId::LightCull));
                LightCullPush p;
                p.frame = frame_addr;
                p.light_grid = ctx.address(light_grid);
                p.light_indices = ctx.address(light_indices);
                ctx.cmd.push_constants(rhi::ShaderStage::Compute, 0, sizeof(p), &p);
                ctx.cmd.dispatch(1, 1, kClusterZ); // local size = 16 x 9 clusters per slice
            });
    } else {
        // No culling shader: zero counts => directional + ambient lighting only.
        g.add_pass("LightCulling.Fallback")
            .write(light_grid, rhi::ResourceState::TransferDst)
            .execute([light_grid](RGContext& ctx) {
                ctx.cmd.fill_buffer(ctx.buffer(light_grid), 0, ~0ull, 0u);
            });
    }

    // ---- forward+ lighting: opaque, masked, sky, translucent in one rendering scope ----
    RGTextureDesc hd;
    hd.format = kHdrFormat;
    hd.width = size.x;
    hd.height = size.y;
    const RGTexture hdr = g.create_texture(hd, "SceneColor");
    {
        RGPassBuilder fwd = g.add_pass("ForwardLighting");
        fwd.write(hdr, rhi::ResourceState::ColorAttachment)
            .read(depth, rhi::ResourceState::DepthStencilAttachment)
            .read(shadow, rhi::ResourceState::ShaderRead)
            .read(spot_shadow, rhi::ResourceState::ShaderRead)
            .read(point_shadow, rhi::ResourceState::ShaderRead)
            .read(gi_probes, rhi::ResourceState::ShaderRead)
            .read(ao, rhi::ResourceState::ShaderRead)
            .read(light_grid, rhi::ResourceState::ShaderRead);
        if (pipelines_->valid(PipelineId::LightCull)) {
            fwd.read(light_indices, rhi::ResourceState::ShaderRead);
        }
        if (gpu) {
            fwd.read(draws, rhi::ResourceState::IndirectArgument).read(counters, rhi::ResourceState::IndirectArgument);
        }
        fwd.execute([this, hdr, depth, ao, light_grid, light_indices, size, frame_addr, overdraw, gpu, draws,
                     counters](RGContext& ctx) {
            rhi::RenderingInfo ri;
            ri.render_area = size;
            ri.color.push_back(rhi::ColorAttachment{ ctx.texture(hdr), 0, 0, rhi::LoadOp::Clear,
                                                     rhi::StoreOp::Store, Vec4(0.0f) });
            ri.has_depth = true;
            ri.depth.texture = ctx.texture(depth);
            ri.depth.load = rhi::LoadOp::Load;
            ri.depth.store = rhi::StoreOp::Store;
            ctx.cmd.begin_rendering(ri);
            ctx.cmd.set_viewport(full_viewport(size));
            ctx.cmd.set_scissor(full_scissor(size));

            MeshPush push;
            push.frame = frame_addr;
            push.light_grid = ctx.address(light_grid);
            push.light_indices = ctx.address(light_indices);
            push.ssao_tex = ctx.sampled(ao);

            if (overdraw) {
                std::vector<DrawItem> all;
                all.reserve(opaque_draws_.size() + translucent_draws_.size());
                for (const auto* list : { &opaque_draws_, &translucent_draws_ }) {
                    for (DrawItem d : *list) {
                        d.pipeline = mesh_pipeline_index(MeshPass::Overdraw, d.skinned, false, false);
                        all.push_back(d);
                    }
                }
                push.pass_flags = kPassFlagOverdraw;
                draw_items(ctx.cmd, all, push, true);
            } else {
                ctx.cmd.push_debug_group("Opaque+Masked");
                if (gpu) {
                    draw_gpu_batches(ctx.cmd, MeshPass::Forward, ctx.buffer(draws), ctx.buffer(counters), push, true,
                                     true);
                } else {
                    draw_items(ctx.cmd, opaque_draws_, push, true);
                }
                ctx.cmd.pop_debug_group();

                if (pipelines_->valid(PipelineId::Sky)) {
                    ctx.cmd.push_debug_group("Sky");
                    ctx.cmd.bind_pipeline(pipelines_->get(PipelineId::Sky));
                    SkyPush sp;
                    sp.frame = frame_addr;
                    ctx.cmd.push_constants(rhi::ShaderStage::AllGraphics, 0, sizeof(sp), &sp);
                    ctx.cmd.draw(3);
                    ++stats_.draw_calls;
                    ctx.cmd.pop_debug_group();
                }

                ctx.cmd.push_debug_group("Translucent");
                draw_items(ctx.cmd, translucent_draws_, push, true);
                ctx.cmd.pop_debug_group();
            }
            ctx.cmd.end_rendering();
        });
    }

    // ---- TAA ----
    RGTexture scene_color = hdr;
    const bool taa_on = settings_.taa && pipelines_->valid(PipelineId::Taa) && !debug;
    if (!taa_on) {
        history_valid_ = false;
    } else {
        ensure_history();
        PersistentTexture& cur = history_[history_index_];
        PersistentTexture& prev = history_[history_index_ ^ 1u];
        if (cur.texture.is_valid() && prev.texture.is_valid()) {
            RGTextureDesc td = hd;
            const RGTexture hist_prev = g.import_texture(prev.texture, td, prev.state, rhi::ResourceState::ShaderRead,
                                                         "TAA.HistoryPrev", didx(prev.sampled));
            const RGTexture hist_cur = g.import_texture(cur.texture, td, cur.state, rhi::ResourceState::ShaderRead,
                                                        "TAA.History", didx(cur.sampled), didx(cur.storage));
            const bool reset = !history_valid_ || !has_prev_view_;
            g.add_pass("TAA")
                .read(hdr, rhi::ResourceState::ShaderRead)
                .read(depth, rhi::ResourceState::ShaderRead)
                .read(hist_prev, rhi::ResourceState::ShaderRead)
                .write(hist_cur, rhi::ResourceState::ShaderWrite)
                .execute([this, hdr, depth, hist_prev, hist_cur, size, frame_addr, reset](RGContext& ctx) {
                    ctx.cmd.bind_pipeline(pipelines_->get(PipelineId::Taa));
                    TaaPush p;
                    p.frame = frame_addr;
                    p.color_tex = ctx.sampled(hdr);
                    p.depth_tex = ctx.sampled(depth);
                    p.history_tex = ctx.sampled(hist_prev);
                    p.out_img = ctx.storage(hist_cur);
                    p.size = size;
                    p.reset = reset ? 1u : 0u;
                    ctx.cmd.push_constants(rhi::ShaderStage::Compute, 0, sizeof(p), &p);
                    ctx.cmd.dispatch(groups(size.x), groups(size.y), 1);
                });
            scene_color = hist_cur;
            // States after this frame (the graph leaves both in their final state).
            cur.state = rhi::ResourceState::ShaderRead;
            prev.state = rhi::ResourceState::ShaderRead;
            history_index_ ^= 1u;
            history_valid_ = true;
        }
    }

    // ---- bloom: dual-filter chain, one texture per level ----
    RGTexture bloom;
    u32       bloom_levels = 0;
    if (settings_.bloom && !debug && pipelines_->valid(PipelineId::BloomDown) &&
        pipelines_->valid(PipelineId::BloomUp)) {
        std::array<RGTexture, kMaxBloomLevels> down{};
        std::array<UVec2, kMaxBloomLevels>     level_size{};
        UVec2                                  s = size;
        while (bloom_levels < kMaxBloomLevels && s.x >= 4 && s.y >= 4) {
            s = glm::max(s / 2u, UVec2(1));
            level_size[bloom_levels] = s;
            RGTextureDesc bd = hd;
            bd.width = s.x;
            bd.height = s.y;
            down[bloom_levels] = g.create_texture(bd, kBloomDownNames[bloom_levels]);
            ++bloom_levels;
        }
        for (u32 i = 0; i < bloom_levels; ++i) {
            const RGTexture src = i == 0 ? scene_color : down[i - 1];
            const RGTexture dst = down[i];
            const UVec2     src_size = i == 0 ? size : level_size[i - 1];
            const UVec2     dst_size = level_size[i];
            g.add_pass(kBloomDownNames[i])
                .read(src, rhi::ResourceState::ShaderRead)
                .write(dst, rhi::ResourceState::ShaderWrite)
                .execute([this, src, dst, src_size, dst_size, i](RGContext& ctx) {
                    ctx.cmd.bind_pipeline(pipelines_->get(PipelineId::BloomDown));
                    BloomPush p;
                    p.src_tex = ctx.sampled(src);
                    p.dst_img = ctx.storage(dst);
                    p.flags = i == 0 ? 1u : 0u;
                    p.src_texel = Vec2(1.0f) / Vec2(src_size);
                    p.dst_size = dst_size;
                    ctx.cmd.push_constants(rhi::ShaderStage::Compute, 0, sizeof(p), &p);
                    ctx.cmd.dispatch(groups(dst_size.x), groups(dst_size.y), 1);
                });
        }
        bloom = bloom_levels > 0 ? down[bloom_levels - 1] : RGTexture{};
        for (u32 i = bloom_levels >= 2 ? bloom_levels - 1 : 0; i-- > 0;) {
            RGTextureDesc ud = hd;
            ud.width = level_size[i].x;
            ud.height = level_size[i].y;
            const RGTexture low = bloom; // previous (smaller) level result
            const RGTexture same = down[i];
            const RGTexture dst = g.create_texture(ud, kBloomUpNames[i]);
            const UVec2     low_size = level_size[i + 1];
            const UVec2     dst_size = level_size[i];
            g.add_pass(kBloomUpNames[i])
                .read(low, rhi::ResourceState::ShaderRead)
                .read(same, rhi::ResourceState::ShaderRead)
                .write(dst, rhi::ResourceState::ShaderWrite)
                .execute([this, low, same, dst, low_size, dst_size](RGContext& ctx) {
                    ctx.cmd.bind_pipeline(pipelines_->get(PipelineId::BloomUp));
                    BloomPush p;
                    p.src_tex = ctx.sampled(low);
                    p.add_tex = ctx.sampled(same);
                    p.dst_img = ctx.storage(dst);
                    p.src_texel = Vec2(1.0f) / Vec2(low_size);
                    p.dst_size = dst_size;
                    p.radius = 1.0f;
                    ctx.cmd.push_constants(rhi::ShaderStage::Compute, 0, sizeof(p), &p);
                    ctx.cmd.dispatch(groups(dst_size.x), groups(dst_size.y), 1);
                });
            bloom = dst;
        }
    }

    // ---- tonemap (+ debug views) into the caller's target ----
    const rhi::PipelineHandle tonemap = pipelines_->get_for_format(PipelineId::Tonemap, target.format);
    const UVec2               out_size = target.extent;
    if (tonemap.is_valid()) {
        const f32 bloom_intensity = bloom_levels > 0 ? std::clamp(settings_.bloom_intensity, 0.0f, 1.0f) : 0.0f;
        const f32 bloom_norm = bloom_levels > 0 ? 1.0f / static_cast<f32>(bloom_levels) : 1.0f;
        const f32  exposure = scene.view.exposure;
        const u32  debug_view = static_cast<u32>(settings_.debug_view);
        const bool oetf = is_unorm_color_format(target.format);
        g.add_pass("Tonemap")
            .read(scene_color, rhi::ResourceState::ShaderRead)
            .read(bloom, rhi::ResourceState::ShaderRead)
            .write(out, rhi::ResourceState::ColorAttachment)
            .execute([this, scene_color, bloom, out, out_size, tonemap, bloom_intensity, bloom_norm, exposure,
                      debug_view, oetf](RGContext& ctx) {
                rhi::RenderingInfo ri;
                ri.render_area = out_size;
                ri.color.push_back(rhi::ColorAttachment{ ctx.texture(out), 0, 0, rhi::LoadOp::DontCare,
                                                         rhi::StoreOp::Store, Vec4(0.0f) });
                ctx.cmd.begin_rendering(ri);
                ctx.cmd.set_viewport(full_viewport(out_size));
                ctx.cmd.set_scissor(full_scissor(out_size));
                ctx.cmd.bind_pipeline(tonemap);
                TonemapPush p;
                p.color_tex = ctx.sampled(scene_color);
                p.bloom_tex = ctx.sampled(bloom);
                p.exposure = exposure;
                p.bloom_intensity = bloom_intensity;
                p.bloom_norm = bloom_norm;
                p.debug_view = debug_view;
                p.apply_oetf = oetf ? 1u : 0u;
                p.inv_target_size = Vec2(1.0f) / Vec2(out_size);
                ctx.cmd.push_constants(rhi::ShaderStage::AllGraphics, 0, sizeof(p), &p);
                ctx.cmd.draw(3);
                ++stats_.draw_calls;
                ctx.cmd.end_rendering();
            });
    } else {
        g.add_pass("ClearTarget")
            .write(out, rhi::ResourceState::ColorAttachment)
            .execute([out, out_size](RGContext& ctx) {
                rhi::RenderingInfo ri;
                ri.render_area = out_size;
                ri.color.push_back(rhi::ColorAttachment{ ctx.texture(out), 0, 0, rhi::LoadOp::Clear,
                                                         rhi::StoreOp::Store, Vec4(0.0f, 0.0f, 0.0f, 1.0f) });
                ctx.cmd.begin_rendering(ri);
                ctx.cmd.end_rendering();
            });
    }

    // ---- debug lines: 1 px LINE_LIST, depth-tested in-shader against scene depth ----
    if (line_vertex_count_ > 0) {
        const rhi::PipelineHandle lines = pipelines_->get_for_format(PipelineId::DebugLines, target.format);
        if (lines.is_valid()) {
            const bool     decode = is_srgb_format(target.format);
            const u32      count = line_vertex_count_;
            g.add_pass("DebugLines")
                .read(depth, rhi::ResourceState::ShaderRead)
                .write(out, rhi::ResourceState::ColorAttachment)
                .execute([this, depth, out, out_size, lines, decode, count, frame_addr](RGContext& ctx) {
                    rhi::RenderingInfo ri;
                    ri.render_area = out_size;
                    ri.color.push_back(rhi::ColorAttachment{ ctx.texture(out), 0, 0, rhi::LoadOp::Load,
                                                             rhi::StoreOp::Store, Vec4(0.0f) });
                    ctx.cmd.begin_rendering(ri);
                    ctx.cmd.set_viewport(full_viewport(out_size));
                    ctx.cmd.set_scissor(full_scissor(out_size));
                    ctx.cmd.bind_pipeline(lines);
                    LinePush p;
                    p.frame = frame_addr;
                    p.depth_tex = ctx.sampled(depth);
                    p.decode_srgb = decode ? 1u : 0u;
                    p.inv_target_size = Vec2(1.0f) / Vec2(out_size);
                    p.depth_bias = 1e-5f;
                    ctx.cmd.push_constants(rhi::ShaderStage::AllGraphics, 0, sizeof(p), &p);
                    ctx.cmd.draw(count);
                    ++stats_.draw_calls;
                    ctx.cmd.end_rendering();
                });
        }
    }

    // ---- CPU readback block (GPU statistics + pick id), consumed frames_in_flight later ----
    if (counters.valid() && readback_buffer_.is_valid()) {
        const u64      rb_size = static_cast<u64>(frames_in_flight_) * kReadbackWords * sizeof(u32);
        const RGBuffer rb = g.import_buffer(readback_buffer_, rb_size, readback_state_, rhi::ResourceState::TransferDst,
                                            "Renderer.Readback");
        readback_state_ = rhi::ResourceState::TransferDst;
        const u64 dst_offset = static_cast<u64>(frame_slot_) * kReadbackWords * sizeof(u32);
        g.add_pass("Readback")
            .read(counters, rhi::ResourceState::TransferSrc)
            .write(rb, rhi::ResourceState::TransferDst)
            .side_effect()
            .execute([counters, rb, dst_offset](RGContext& ctx) {
                ctx.cmd.copy_buffer(ctx.buffer(counters), ctx.buffer(rb), kReadbackWords * sizeof(u32),
                                    kCounterStats * sizeof(u32), dst_offset);
            });
        ReadbackSlot& rs = readback_slots_[frame_slot_];
        rs.pending = true;
        rs.gpu_stats = gpu;
        rs.pick = pick_this_frame_;
    }

    g.compile();
    if (shadow.valid()) {
        shadow_map_.state = g.final_state(shadow);
    }
    if (spot_shadow.valid()) {
        spot_shadow_map_.state = g.final_state(spot_shadow);
    }
    if (point_shadow.valid()) {
        point_shadow_map_.state = g.final_state(point_shadow);
    }
}

} // namespace aether::renderer
