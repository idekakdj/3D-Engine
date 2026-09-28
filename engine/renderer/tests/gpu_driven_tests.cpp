// gpu_driven_tests.cpp — ADR-0009: Hi-Z occlusion math (CPU reference of hiz_build.comp /
// gpu_cull.comp, reverse-Z min pyramid), the GPU-driven pass structure on the validating mock
// device, and the asynchronous readback plumbing of GPU statistics and picking.
#include "gpu_data.h"
#include "pipelines.h"
#include "mock_device.h"
#include "render_math.h"

#include "aether/renderer/instance_flags.h"
#include "aether/renderer/renderer.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace aether;
using namespace aether::renderer;

TEST_CASE("Hi-Z pyramid: power-of-two mip 0, full chain, min reduction (reverse-Z)") {
    CHECK(hiz_mip0_size(UVec2(320, 240)) == UVec2(256, 128));
    CHECK(hiz_mip0_size(UVec2(256, 256)) == UVec2(256, 256));
    CHECK(hiz_mip0_size(UVec2(1, 1)) == UVec2(1, 1));
    CHECK(hiz_mip_count(UVec2(256, 128)) == 9);
    CHECK(hiz_mip_count(UVec2(1, 1)) == 1);

    // 5x3 depth: every mip-0 texel must hold the MIN of the depth pixels it covers.
    const std::vector<f32> depth = { 0.9f, 0.8f, 0.7f, 0.6f, 0.5f,  //
                                     0.4f, 0.95f, 0.9f, 0.9f, 0.9f, //
                                     0.9f, 0.9f, 0.9f, 0.9f, 0.1f };
    const HiZPyramid h = build_hiz(depth, UVec2(5, 3));
    REQUIRE(h.levels.size() == 3); // 4x2, 2x1, 1x1
    CHECK(h.sizes[0] == UVec2(4, 2));
    CHECK(h.sizes[2] == UVec2(1, 1));
    CHECK(h.levels[2][0] == doctest::Approx(0.1f)); // global farthest
    const f32 global_min = *std::min_element(depth.begin(), depth.end());
    CHECK(h.levels.back()[0] == global_min);
}

TEST_CASE("Hi-Z occlusion test is conservative for random depth buffers and rectangles") {
    std::mt19937                          rng(1234);
    std::uniform_real_distribution<f32>   d01(0.0f, 1.0f);
    const UVec2 sizes[] = { UVec2(37, 23), UVec2(320, 240), UVec2(1, 1), UVec2(5, 300), UVec2(64, 64) };
    u32 occluded_count = 0;
    for (const UVec2 size : sizes) {
        std::vector<f32> depth(static_cast<usize>(size.x) * size.y);
        // Blocky content (large occluders + sky holes) so both outcomes occur.
        const u32 block = std::max(1u, size.x / 5);
        for (u32 y = 0; y < size.y; ++y) {
            for (u32 x = 0; x < size.x; ++x) {
                const u32 b = (x / block) * 7 + (y / block) * 13;
                depth[static_cast<usize>(y) * size.x + x] = (b % 5 == 0) ? 0.0f : 0.3f + 0.6f * f32((b * 37) % 11) / 11.0f;
            }
        }
        const HiZPyramid h = build_hiz(depth, size);
        std::uniform_int_distribution<u32> rx(0, size.x - 1), ry(0, size.y - 1);
        for (int t = 0; t < 400; ++t) {
            UVec2 a(rx(rng), ry(rng)), b(rx(rng), ry(rng));
            const UVec2 p0 = glm::min(a, b), p1 = glm::max(a, b);
            const f32 nearest = d01(rng);
            if (!hiz_occluded(h, size, p0, p1, nearest)) {
                continue;
            }
            ++occluded_count;
            // Brute force: every covered depth pixel must be strictly nearer than the object.
            bool ok = true;
            for (u32 y = p0.y; y <= p1.y && ok; ++y) {
                for (u32 x = p0.x; x <= p1.x; ++x) {
                    if (!(nearest < depth[static_cast<usize>(y) * size.x + x])) {
                        ok = false;
                        break;
                    }
                }
            }
            CHECK(ok);
        }
    }
    CHECK(occluded_count > 20); // the test exercised the culling branch
}

TEST_CASE("Hi-Z occlusion: box behind a wall is culled, in front or over sky is not") {
    const UVec2 size(64, 32);
    std::vector<f32> depth(static_cast<usize>(size.x) * size.y, 0.0f); // sky (reverse-Z far = 0)
    for (u32 y = 0; y < size.y; ++y) {
        for (u32 x = 0; x < 40; ++x) {
            depth[static_cast<usize>(y) * size.x + x] = 0.5f; // wall over the left 40 columns
        }
    }
    const HiZPyramid h = build_hiz(depth, size);
    CHECK(hiz_occluded(h, size, UVec2(2, 2), UVec2(30, 20), 0.3f));        // behind the wall
    CHECK_FALSE(hiz_occluded(h, size, UVec2(2, 2), UVec2(30, 20), 0.7f));  // in front of it
    CHECK_FALSE(hiz_occluded(h, size, UVec2(2, 2), UVec2(30, 20), 0.5f));  // coplanar: kept
    CHECK_FALSE(hiz_occluded(h, size, UVec2(35, 2), UVec2(45, 20), 0.3f)); // reaches the sky

    // Projection: a box straight ahead covers the centre and has a positive reverse-Z depth.
    const Mat4 proj = to_reverse_z(perspective(60.0f * kDeg2Rad, 2.0f, 0.1f, 100.0f), 0.1f);
    const Mat4 vp = proj * look_at(Vec3(0, 0, 5), Vec3(0), Vec3(0, 1, 0));
    UVec2      p0, p1;
    f32        nearest = 0.0f;
    REQUIRE(project_aabb_rect(vp, AABB{ Vec3(-0.5f), Vec3(0.5f) }, size, p0, p1, nearest));
    CHECK(p0.x < 32);
    CHECK(p1.x >= 32);
    CHECK(nearest > 0.0f);
    CHECK(nearest < 1.0f);
    CHECK_FALSE(project_aabb_rect(vp, AABB{ Vec3(-1, -1, 4), Vec3(1, 1, 6) }, size, p0, p1, nearest)); // camera inside
}

// ---------------------------------------------------------------------------------------------
// Mock-device renderer tests
// ---------------------------------------------------------------------------------------------
namespace {
struct Fixture {
    test::MockDevice          device;
    std::unique_ptr<Renderer> r;
    MeshHandle                mesh;
    MaterialHandle            opaque, masked, glass;
    rhi::TextureHandle        target_tex;
    RenderTarget              target;

    explicit Fixture(bool gpu_features, bool mesh_shaders = false) {
        device.mutable_features().draw_indirect_count = gpu_features;
        device.mutable_features().draw_indirect_first_instance = gpu_features;
        device.mutable_features().mesh_shaders = mesh_shaders;
        auto created = Renderer::create(RendererDesc{ &device, UVec2(160, 90) });
        REQUIRE(created);
        r = std::move(created.value());
        std::vector<Vertex> v(8);
        for (u32 i = 0; i < 8; ++i) {
            v[i].position = Vec3((i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
            v[i].normal = glm::normalize(v[i].position);
        }
        const std::vector<u32> idx = { 0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1,
                                       2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3 };
        MeshUpload mu;
        mu.vertices = v;
        mu.indices = idx;
        mesh = r->register_mesh(mu);
        MaterialDesc md;
        opaque = r->register_material(md);
        md.blend = BlendMode::Masked;
        md.double_sided = true;
        masked = r->register_material(md);
        md.blend = BlendMode::Translucent;
        glass = r->register_material(md);
        target_tex = device.make_target(160, 90);
        target = RenderTarget{ target_tex, rhi::Format::BGRA8Unorm, UVec2(160, 90), rhi::ResourceState::Undefined,
                               rhi::ResourceState::ColorAttachment };
    }

    RenderScene scene(u32 count) const {
        RenderScene s;
        s.view.view = look_at(Vec3(0, 5, 20), Vec3(0), Vec3(0, 1, 0));
        s.view.proj = perspective(60.0f * kDeg2Rad, 16.0f / 9.0f, 0.1f, 500.0f);
        s.view.view_proj = s.view.proj * s.view.view;
        for (u32 i = 0; i < count; ++i) {
            RenderMeshInstance inst;
            inst.mesh = mesh;
            inst.material = i % 3 == 0 ? opaque : (i % 3 == 1 ? masked : glass);
            inst.transform = glm::translate(Mat4(1.0f), Vec3(f32(i % 20) * 3.0f - 30.0f, 0.0f, -f32(i / 20) * 3.0f));
            inst.user_id = i + 1;
            s.instances.push_back(inst);
        }
        RenderLight sun;
        sun.type = LightType::Directional;
        sun.cast_shadows = true;
        s.lights.push_back(sun);
        return s;
    }

    void frame(const RenderScene& s) {
        device.state.texture_state[target_tex.value] = rhi::ResourceState::Undefined;
        r->render(s, device.cmd_, target);
        CHECK(device.cmd_.balanced());
    }

    bool logged(const std::string& group) const {
        return std::find(device.state.log.begin(), device.state.log.end(), "group " + group) != device.state.log.end();
    }

    // The mock's mapped memory stands in for the GPU-written readback ring.
    u32* readback_words() {
        for (auto& [value, desc] : device.state.buffers) {
            if (desc.debug_name == "Renderer.Readback") {
                rhi::BufferHandle h;
                h.value = value;
                return static_cast<u32*>(device.map(h));
            }
        }
        return nullptr;
    }
};
} // namespace

TEST_CASE("GPU-driven path: pass structure, indirect-count draws, fallbacks") {
    Fixture f(true);
    const RenderScene s = f.scene(300);
    f.frame(s);
    CHECK(f.logged("GpuCull.Phase1"));
    CHECK(f.logged("HiZ.Build"));
    CHECK(f.logged("GpuCull.Phase2"));
    CHECK(f.logged("DepthPrepass.Phase2"));
    CHECK(f.logged("Readback"));
    CHECK(f.device.state.indirect_count_draws > 0);

    // Occlusion off: a single frustum-culling dispatch.
    f.device.state.log.clear();
    f.r->settings().occlusion_culling = false;
    f.frame(s);
    CHECK(f.logged("GpuCull"));
    CHECK_FALSE(f.logged("HiZ.Build"));
    CHECK_FALSE(f.logged("GpuCull.Phase2"));

    // CPU fallback when disabled, and for the Overdraw view (it visualises the CPU lists).
    f.device.state.log.clear();
    const u32 indirect_before = f.device.state.indirect_count_draws;
    f.r->settings().gpu_culling = false;
    f.frame(s);
    CHECK_FALSE(f.logged("GpuCull"));
    CHECK(f.device.state.indirect_count_draws == indirect_before);
    CHECK(f.r->stats().instances_visible > 0);
    f.r->settings().gpu_culling = true;
    f.r->settings().debug_view = DebugView::Overdraw;
    f.device.state.log.clear();
    f.frame(s);
    CHECK_FALSE(f.logged("GpuCull"));
    f.r->settings().debug_view = DebugView::None;

    // Every debug view and an empty scene still record valid frames on the GPU path.
    f.r->settings().occlusion_culling = true;
    for (u8 dv = 0; dv < static_cast<u8>(DebugView::Overdraw); ++dv) {
        f.r->settings().debug_view = static_cast<DebugView>(dv);
        f.frame(s);
    }
    f.r->settings().debug_view = DebugView::None;
    f.frame(RenderScene{});
    CHECK(f.device.state.errors.empty());
    for (const std::string& e : f.device.state.errors) {
        MESSAGE(e);
    }
}

TEST_CASE("mesh shading: static candidates are drawn as meshlets, with fallbacks (ADR-0010)") {
    Fixture f(true, true);
    CHECK(f.device.state.mesh_pipelines_created == kMeshletPipelineCount);
    const RenderScene s = f.scene(90); // opaque, masked + double-sided, translucent
    f.frame(s);
    CHECK(f.device.state.mesh_task_draws > 0);
    CHECK(f.r->stats().meshlet_instances == 60); // the two non-translucent thirds
    // Picking draws its id pass through the meshlet pipelines too.
    const u32 before_pick = f.device.state.mesh_task_draws;
    f.r->request_pick(UVec2(80, 45));
    f.frame(s);
    CHECK(f.device.state.mesh_task_draws > before_pick);

    // Disabled by setting: the indexed indirect path draws everything.
    f.r->settings().mesh_shading = false;
    const u32 tasks = f.device.state.mesh_task_draws;
    const u32 indexed = f.device.state.indirect_count_draws;
    f.frame(s);
    CHECK(f.device.state.mesh_task_draws == tasks);
    CHECK(f.device.state.indirect_count_draws > indexed);
    CHECK(f.r->stats().meshlet_instances == 0);

    // Without the GPU-driven path there are no meshlet draws either.
    f.r->settings().mesh_shading = true;
    f.r->settings().gpu_culling = false;
    f.frame(s);
    CHECK(f.device.state.mesh_task_draws == tasks);
    CHECK(f.device.state.errors.empty());
    for (const std::string& e : f.device.state.errors) {
        MESSAGE(e);
    }
}

TEST_CASE("mesh shading: releasing a mesh frees its meshlet buffer") {
    Fixture f(true, true);
    const u32 destroyed = f.device.state.buffers_destroyed;
    f.r->release(f.mesh);
    f.frame(RenderScene{});
    CHECK(f.device.state.buffers_destroyed > destroyed);
    CHECK(f.device.state.errors.empty());
}

TEST_CASE("spot shadows: selection, budget, pass and fallbacks (ADR-0012)") {
    Fixture f(true);
    RenderScene s = f.scene(40);
    s.lights.clear();
    auto spot = [](Vec3 pos, Vec3 dir, bool shadows) {
        RenderLight l;
        l.type = LightType::Spot;
        l.position = pos;
        l.direction = glm::normalize(dir);
        l.range = 30.0f;
        l.outer_cone = 0.7f;
        l.inner_cone = 0.8f;
        l.cast_shadows = shadows;
        return l;
    };
    for (int i = 0; i < 6; ++i) { // six shadow-casting spots over the instance grid
        s.lights.push_back(spot(Vec3(f32(i) * 4.0f - 10.0f, 8.0f, -5.0f), Vec3(0, -1, 0), true));
    }
    s.lights.push_back(spot(Vec3(0, 8, -5), Vec3(0, -1, 0), false));       // no shadows requested
    s.lights.push_back(spot(Vec3(0, 8, 400), Vec3(0, -1, 0), true));       // behind the camera, out of view
    for (RenderMeshInstance& inst : s.instances) {
        inst.flags |= instance_flags::kCastShadow;
    }
    f.frame(s);
    CHECK(f.r->stats().spot_shadow_maps == 4); // default budget
    CHECK(f.logged("SpotShadow"));

    f.r->settings().max_spot_shadows = 2;
    f.device.state.log.clear();
    f.frame(s);
    CHECK(f.r->stats().spot_shadow_maps == 2);

    f.r->settings().max_spot_shadows = 99; // clamped to kMaxSpotShadows
    f.frame(s);
    CHECK(f.r->stats().spot_shadow_maps == 6);

    f.r->settings().spot_shadows = false;
    f.device.state.log.clear();
    f.frame(s);
    CHECK(f.r->stats().spot_shadow_maps == 0);
    CHECK_FALSE(f.logged("SpotShadow"));

    // Independent of the cascaded (sun) shadows switch, which software rasterisers turn off.
    f.r->settings().spot_shadows = true;
    f.r->settings().shadows = false;
    f.frame(s);
    CHECK(f.r->stats().spot_shadow_maps == 6);

    // No casters: no shadow maps are rendered.
    for (RenderMeshInstance& inst : s.instances) {
        inst.flags &= ~instance_flags::kCastShadow;
    }
    f.frame(s);
    CHECK(f.r->stats().spot_shadow_maps == 0);
    CHECK(f.device.state.errors.empty());
    for (const std::string& e : f.device.state.errors) {
        MESSAGE(e);
    }
}

TEST_CASE("GPU-driven path without drawIndirectCount falls back to the CPU path") {
    Fixture f(false);
    f.frame(f.scene(60));
    CHECK_FALSE(f.logged("GpuCull"));
    CHECK_FALSE(f.logged("GpuCull.Phase1"));
    CHECK(f.device.state.indirect_count_draws == 0);
    CHECK(f.device.state.errors.empty());
}

TEST_CASE("GPU statistics are read back with frames-in-flight latency") {
    Fixture f(true);
    const RenderScene s = f.scene(90);
    f.frame(s); // frame 1 -> slot 1
    u32* rb = f.readback_words();
    REQUIRE(rb != nullptr);
    // Pretend the GPU finished frame 1: frustum 7, occlusion 5, visible 40, triangles 480.
    rb[1 * kReadbackWords + 0] = 7;
    rb[1 * kReadbackWords + 1] = 5;
    rb[1 * kReadbackWords + 2] = 40;
    rb[1 * kReadbackWords + 3] = 480;
    f.frame(s); // frame 2 -> slot 0: nothing from slot 1 yet
    CHECK(f.r->stats().instances_gpu_frustum_culled == 0);
    f.frame(s); // frame 3 -> slot 1: collects frame 1
    CHECK(f.r->stats().instances_gpu_frustum_culled == 7);
    CHECK(f.r->stats().instances_gpu_occlusion_culled == 5);
    CHECK(f.r->stats().instances_visible >= 40); // + CPU-drawn translucent instances
    CHECK(f.r->stats().triangles >= 480);
    CHECK(f.device.state.invalidations > 0);
    CHECK(f.device.state.errors.empty());
}

TEST_CASE("picking: request_pick -> async readback -> poll_pick returns the id once") {
    for (const bool gpu : { true, false }) {
        CAPTURE(gpu);
        Fixture f(gpu);
        const RenderScene s = f.scene(30);
        f.frame(s);
        f.frame(s);
        CHECK_FALSE(f.r->poll_pick().has_value());

        f.r->request_pick(UVec2(80, 45));
        f.device.state.log.clear();
        f.frame(s); // frame 3 -> slot 1 records the id pass
        CHECK(f.logged("Pick.Ids"));
        CHECK(f.logged("Pick.Resolve"));
        CHECK_FALSE(f.r->poll_pick().has_value()); // never a stall: not available yet
        u32* rb = f.readback_words();
        REQUIRE(rb != nullptr);
        rb[1 * kReadbackWords + (kCounterPick - kCounterStats)] = 17; // "GPU" wrote user_id 17
        f.frame(s); // slot 0
        CHECK_FALSE(f.r->poll_pick().has_value());
        f.frame(s); // slot 1 again: result collected
        const std::optional<u32> picked = f.r->poll_pick();
        REQUIRE(picked.has_value());
        CHECK(*picked == 17);
        CHECK_FALSE(f.r->poll_pick().has_value()); // returned once

        // Outside the target: answered with 0 (nothing pickable) on the next render.
        f.r->request_pick(UVec2(1000, 1000));
        f.frame(s);
        const std::optional<u32> none = f.r->poll_pick();
        REQUIRE(none.has_value());
        CHECK(*none == 0);
        CHECK(f.device.state.errors.empty());
        for (const std::string& e : f.device.state.errors) {
            MESSAGE(e);
        }
    }
}

TEST_CASE("point shadows: six faces per light, budget, pass, mixed with spots (ADR-0015)") {
    Fixture f(true);
    RenderScene s = f.scene(40);
    s.lights.clear();
    auto point = [](Vec3 pos, bool shadows) {
        RenderLight l;
        l.type = LightType::Point;
        l.position = pos;
        l.range = 20.0f;
        l.cast_shadows = shadows;
        return l;
    };
    for (int i = 0; i < 5; ++i) {
        s.lights.push_back(point(Vec3(f32(i) * 4.0f - 8.0f, 3.0f, -5.0f), true));
    }
    s.lights.push_back(point(Vec3(0, 3, -5), false));  // no shadows requested
    s.lights.push_back(point(Vec3(0, 3, 400), true));  // behind the camera, out of view
    for (RenderMeshInstance& inst : s.instances) {
        inst.flags |= instance_flags::kCastShadow;
    }
    f.frame(s);
    CHECK(f.r->stats().point_shadow_maps == 2); // default budget
    CHECK(f.r->stats().spot_shadow_maps == 0);
    CHECK(f.logged("PointShadowFace"));
    CHECK_FALSE(f.logged("SpotShadow"));

    f.r->settings().max_point_shadows = 99; // clamped to kMaxPointShadows
    f.frame(s);
    CHECK(f.r->stats().point_shadow_maps == kMaxPointShadows);

    // Spots and points share the GpuSpotShadow table (spots first) but use separate maps.
    RenderLight sp;
    sp.type = LightType::Spot;
    sp.position = Vec3(0, 8, -5);
    sp.direction = Vec3(0, -1, 0);
    sp.range = 30.0f;
    sp.cast_shadows = true;
    s.lights.push_back(sp);
    f.device.state.log.clear();
    f.frame(s);
    CHECK(f.r->stats().spot_shadow_maps == 1);
    CHECK(f.r->stats().point_shadow_maps == kMaxPointShadows);
    CHECK(f.logged("SpotShadow"));
    CHECK(f.logged("PointShadowFace"));

    f.r->settings().point_shadows = false;
    f.device.state.log.clear();
    f.frame(s);
    CHECK(f.r->stats().point_shadow_maps == 0);
    CHECK_FALSE(f.logged("PointShadowFace"));
    CHECK(f.r->stats().spot_shadow_maps == 1);

    f.r->settings().point_shadows = true;
    f.r->settings().point_shadow_map_size = 256; // re-created at the new size
    f.frame(s);
    CHECK(f.r->stats().point_shadow_maps == kMaxPointShadows);
    CHECK(f.device.state.errors.empty());
    for (const std::string& e : f.device.state.errors) {
        MESSAGE(e);
    }
}
