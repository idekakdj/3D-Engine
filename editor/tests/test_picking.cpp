// test_picking.cpp — ray construction, ray/box tests, entity picking, box select, and the GPU pick
// tracker with its CPU fallback (scripted mock renderer).
#include "aether/editor/picking.h"
#include "aether/gameplay/procedural_mesh.h"
#include "aether/gameplay/render_bridge.h"
#include "aether/renderer/renderer.h"
#include "aether/scene/components.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/visibility.h"
#include "aether/scene/world.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace aether;
using namespace aether::editor;

namespace {
bool near(const Vec3& a, const Vec3& b, f32 eps = 1e-3f) { return glm::all(glm::lessThan(glm::abs(a - b), Vec3(eps))); }

class NullRenderer final : public renderer::Renderer {
public:
    // Scripted picking: when `pick_supported`, a request is answered with `pick_answer` after
    // `pick_delay` poll_pick() calls (one per frame).
    bool pick_supported = false;
    u32  pick_answer    = 0;
    int  pick_delay     = 2;
    void request_pick(UVec2 pixel) override {
        last_pixel = pixel;
        if (pick_supported) {
            countdown_ = pick_delay;
        }
    }
    std::optional<u32> poll_pick() override {
        if (countdown_ < 0) {
            return std::nullopt;
        }
        if (countdown_-- == 0) {
            return pick_answer;
        }
        return std::nullopt;
    }
    UVec2 last_pixel{ 0, 0 };

    renderer::MeshHandle     register_mesh(const renderer::MeshUpload&) override { return renderer::MeshHandle(n_++, 1); }
    renderer::TextureHandle  register_texture(const renderer::TextureUpload&) override { return renderer::TextureHandle(n_++, 1); }
    renderer::MaterialHandle register_material(const renderer::MaterialDesc&) override { return renderer::MaterialHandle(n_++, 1); }
    renderer::EnvHandle      register_environment(const renderer::EnvironmentUpload&) override { return renderer::EnvHandle(n_++, 1); }
    void update_material(renderer::MaterialHandle, const renderer::MaterialDesc&) override {}
    void release(renderer::MeshHandle) override {}
    void release(renderer::TextureHandle) override {}
    void release(renderer::MaterialHandle) override {}
    void release(renderer::EnvHandle) override {}
    void render(const renderer::RenderScene&, rhi::CommandList&, const renderer::RenderTarget&) override {}
    void resize(UVec2) override {}
    Result<void> reload_shaders() override { return {}; }
    renderer::RendererSettings&    settings() override { return s_; }
    const renderer::RendererStats& stats() const override { return st_; }

private:
    int                        countdown_ = -1;
    u32                        n_ = 1;
    renderer::RendererSettings s_{};
    renderer::RendererStats    st_{};
};
} // namespace

TEST_CASE("pick rays follow the engine projection (top-left uv origin)") {
    const Mat4 view = look_at(Vec3(0, 0, 5), Vec3(0), Vec3(0, 1, 0));
    const Mat4 proj = perspective(kHalfPi, 1.0f, 0.1f, 100.0f);
    const Ray  c    = make_pick_ray(view, proj, Vec2(0.5f, 0.5f));
    CHECK(near(c.direction, Vec3(0, 0, -1)));
    CHECK(near(c.origin, Vec3(0, 0, 4.9f), 1e-2f));
    // Top of the viewport looks up (fov 90: 45 degrees at the edge).
    const Ray top = make_pick_ray(view, proj, Vec2(0.5f, 0.0f));
    CHECK(top.direction.y > 0.6f);
    const Ray right = make_pick_ray(view, proj, Vec2(1.0f, 0.5f));
    CHECK(right.direction.x > 0.6f);
    CHECK(glm::length(right.direction) == doctest::Approx(1.0f));
}

TEST_CASE("ray / AABB") {
    AABB box;
    box.min = Vec3(-1);
    box.max = Vec3(1);
    CHECK(ray_aabb(Ray{ Vec3(0, 0, 5), Vec3(0, 0, -1) }, box) == doctest::Approx(4.0f));
    CHECK_FALSE(ray_aabb(Ray{ Vec3(0, 0, 5), Vec3(0, 0, 1) }, box).has_value());   // pointing away
    CHECK_FALSE(ray_aabb(Ray{ Vec3(3, 0, 5), Vec3(0, 0, -1) }, box).has_value());  // parallel, outside
    CHECK(ray_aabb(Ray{ Vec3(0, 0, 0), Vec3(1, 0, 0) }, box) == doctest::Approx(0.0f)); // inside
    const Vec3 diag = glm::normalize(Vec3(-1, -1, -1));
    CHECK(ray_aabb(Ray{ Vec3(5), diag }, box).has_value());
}

TEST_CASE("pick_entity returns the nearest visible mesh") {
    NullRenderer                  r;
    gameplay::RenderResourceCache cache(r, nullptr);
    World                         w;
    auto add_cube = [&](const char* name, const Vec3& pos) {
        const Entity          e = w.create(name);
        MeshRendererComponent mr;
        mr.mesh = gameplay::builtin_mesh_id(gameplay::BuiltinMesh::Cube);
        w.add<MeshRendererComponent>(e, mr);
        scene::set_local_position(w, e, pos);
        return e;
    };
    const Entity near_cube = add_cube("Near", Vec3(0, 0, 0));
    const Entity far_cube  = add_cube("Far", Vec3(0, 0, -5));
    const Entity side      = add_cube("Side", Vec3(4, 0, 0));
    w.update_transforms();

    const Ray ray{ Vec3(0, 0, 10), Vec3(0, 0, -1) };
    auto      hit = pick_entity(w, cache, ray);
    REQUIRE(hit.has_value());
    CHECK((hit->entity == near_cube));
    CHECK(hit->distance == doctest::Approx(9.5f));

    scene::set_visible(w, near_cube, false); // hidden entities are not pickable
    hit = pick_entity(w, cache, ray);
    REQUIRE(hit.has_value());
    CHECK((hit->entity == far_cube));

    CHECK((pick_entity(w, cache, Ray{ Vec3(4, 0, 10), Vec3(0, 0, -1) })->entity == side));
    CHECK_FALSE(pick_entity(w, cache, Ray{ Vec3(10, 10, 10), Vec3(0, 0, -1) }).has_value());
}

namespace {
Entity add_cube(World& w, const char* name, const Vec3& pos) {
    const Entity          e = w.create(name);
    MeshRendererComponent mr;
    mr.mesh = gameplay::builtin_mesh_id(gameplay::BuiltinMesh::Cube);
    w.add<MeshRendererComponent>(e, mr);
    scene::set_local_position(w, e, pos);
    return e;
}

// Mirrors EditorApp::request_viewport_pick + poll_gpu_pick: returns the picked entity and how.
struct ClickResult {
    Entity      entity = kNullEntity;
    const char* source = "";
    u64         frames = 0; // frames until resolved
};
ClickResult click(GpuPickTracker& t, NullRenderer& r, World& w, gameplay::RenderResourceCache& cache, const Ray& ray,
                  u64& frame) {
    auto cpu = [&] {
        const auto hit = pick_entity(w, cache, ray);
        return hit ? hit->entity : kNullEntity;
    };
    const u64  start = frame;
    const auto route = t.request(frame);
    r.request_pick(UVec2(10, 10));
    if (route == GpuPickTracker::Route::Cpu) {
        return { cpu(), "cpu", 0 };
    }
    for (int i = 0; i < 32; ++i) {
        ++frame;
        if (const auto out = t.update(frame, r.poll_pick())) {
            if (out->source == GpuPickTracker::Source::Gpu) {
                return { entity_from_pick_id(w, out->user_id), "gpu", frame - start };
            }
            return { cpu(), "fallback", frame - start };
        }
    }
    return { kNullEntity, "stuck", frame - start };
}
} // namespace

TEST_CASE("GPU pick tracker: answer, timeout, abandon and unsupported detection") {
    GpuPickTracker t(4, 2);
    CHECK(t.request(10, 7) == GpuPickTracker::Route::Gpu);
    CHECK(t.pending());
    CHECK_FALSE(t.update(11, std::nullopt).has_value());
    auto out = t.update(12, 42u);
    REQUIRE(out.has_value());
    CHECK(out->source == GpuPickTracker::Source::Gpu);
    CHECK(out->user_id == 42u);
    CHECK(out->tag == 7u);
    CHECK_FALSE(t.pending());

    // Timeout: nothing for `timeout` frames -> fallback on the next.
    CHECK(t.request(20) == GpuPickTracker::Route::Gpu);
    for (u64 f = 21; f <= 24; ++f) {
        CHECK_FALSE(t.update(f, std::nullopt).has_value());
    }
    out = t.update(25, std::nullopt);
    REQUIRE(out.has_value());
    CHECK(out->source == GpuPickTracker::Source::CpuFallback);
    CHECK(t.consecutive_timeouts() == 1);
    CHECK(t.gpu_available());

    // A late answer for the timed-out request is discarded (and proves the GPU path works).
    CHECK_FALSE(t.update(26, 5u).has_value());
    CHECK(t.consecutive_timeouts() == 0);
    // Within the ignore window new clicks resolve on the CPU (no attribution ambiguity).
    CHECK(t.request(27) == GpuPickTracker::Route::Cpu);
    CHECK_FALSE(t.pending());

    // Abandoning a pending request (double click) routes the new click to the CPU.
    CHECK(t.request(100) == GpuPickTracker::Route::Gpu);
    CHECK(t.request(101) == GpuPickTracker::Route::Cpu);
    CHECK_FALSE(t.pending());
    CHECK_FALSE(t.update(102, 9u).has_value()); // stale answer ignored

    // Two consecutive timeouts: GPU picking is considered unsupported until an answer arrives.
    GpuPickTracker u(2, 2);
    u64            f = 0;
    for (int k = 0; k < 2; ++k) {
        f += 10;
        REQUIRE(u.request(f) == GpuPickTracker::Route::Gpu);
        std::optional<GpuPickTracker::Outcome> o;
        while (!o) {
            o = u.update(++f, std::nullopt);
        }
        CHECK(o->source == GpuPickTracker::Source::CpuFallback);
    }
    CHECK_FALSE(u.gpu_available());
    CHECK(u.fallbacks() == 2);
    CHECK(u.request(f + 50) == GpuPickTracker::Route::Cpu);
    CHECK_FALSE(u.update(f + 51, 3u).has_value()); // probe answer
    CHECK(u.gpu_available());
    CHECK(u.request(f + 100) == GpuPickTracker::Route::Gpu);
}

TEST_CASE("GPU picking end to end: GPU answer wins, CPU fallback without support") {
    NullRenderer                  r;
    gameplay::RenderResourceCache cache(r, nullptr);
    World                         w;
    const Entity                  center = add_cube(w, "Center", Vec3(0, 0, 0));
    const Entity                  side   = add_cube(w, "Side", Vec3(4, 0, 0));
    w.update_transforms();
    const Ray      ray{ Vec3(0, 0, 10), Vec3(0, 0, -1) }; // CPU would hit Center
    GpuPickTracker t(/*frames_in_flight + 2*/ 4, 2);
    u64            frame = 1;

    // Supported: the GPU id (Side) is used even though the CPU ray would say Center.
    r.pick_supported = true;
    r.pick_delay     = 2;
    r.pick_answer    = pick_id_of(side);
    ClickResult c    = click(t, r, w, cache, ray, frame);
    CHECK(std::string(c.source) == "gpu");
    CHECK((c.entity == side));
    CHECK(c.frames <= 4);

    // GPU says "nothing there" (id 0): the click deselects, no CPU guess.
    r.pick_answer = 0;
    frame += 20;
    c = click(t, r, w, cache, ray, frame);
    CHECK(std::string(c.source) == "gpu");
    CHECK((c.entity == kNullEntity));

    // Unsupported (default renderer behaviour): fallback after timeout + 1 frames, twice, then
    // immediate CPU picks.
    r.pick_supported = false;
    for (int k = 0; k < 2; ++k) {
        frame += 20;
        c = click(t, r, w, cache, ray, frame);
        CHECK(std::string(c.source) == "fallback");
        CHECK((c.entity == center));
        CHECK(c.frames == 5);
    }
    frame += 20;
    c = click(t, r, w, cache, ray, frame);
    CHECK(std::string(c.source) == "cpu");
    CHECK((c.entity == center));

    // Stale ids do not resolve to a different entity.
    const u32 stale = pick_id_of(side);
    w.destroy(side);
    CHECK((entity_from_pick_id(w, stale) == kNullEntity));
    CHECK((entity_from_pick_id(w, 0) == kNullEntity));
    CHECK((entity_from_pick_id(w, pick_id_of(center)) == center));
}

TEST_CASE("ray / plane and box selection") {
    const auto t = ray_plane(Ray{ Vec3(0, 5, 0), glm::normalize(Vec3(1, -1, 0)) }, Vec3(0, 1, 0), 0.0f);
    REQUIRE(t.has_value());
    CHECK(*t == doctest::Approx(5.0f * std::sqrt(2.0f)));
    CHECK_FALSE(ray_plane(Ray{ Vec3(0, 5, 0), Vec3(0, 1, 0) }, Vec3(0, 1, 0), 0.0f).has_value()); // behind
    CHECK_FALSE(ray_plane(Ray{ Vec3(0, 5, 0), Vec3(1, 0, 0) }, Vec3(0, 1, 0), 0.0f).has_value()); // parallel

    NullRenderer                  r;
    gameplay::RenderResourceCache cache(r, nullptr);
    World                         w;
    const Entity                  left   = add_cube(w, "L", Vec3(-3, 0, 0));
    const Entity                  middle = add_cube(w, "M", Vec3(0, 0, 0));
    const Entity                  behind = add_cube(w, "Behind", Vec3(0, 0, 20)); // behind the camera
    w.update_transforms();
    const Mat4 vp = perspective(kHalfPi, 1.0f, 0.1f, 100.0f) * look_at(Vec3(0, 0, 10), Vec3(0), Vec3(0, 1, 0));
    auto       in = entities_in_rect(w, cache, vp, Vec2(0.4f, 0.4f), Vec2(0.6f, 0.6f));
    REQUIRE(in.size() == 1);
    CHECK((in[0] == middle));
    in = entities_in_rect(w, cache, vp, Vec2(0.6f, 0.6f), Vec2(0.0f, 0.4f)); // corners in any order
    REQUIRE(in.size() == 2);
    CHECK((std::find(in.begin(), in.end(), left) != in.end()));
    CHECK((std::find(in.begin(), in.end(), behind) == in.end()));
}
