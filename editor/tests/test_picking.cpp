// test_picking.cpp — ray construction, ray/box tests and entity picking (mock renderer).
#include "aether/editor/picking.h"
#include "aether/gameplay/procedural_mesh.h"
#include "aether/gameplay/render_bridge.h"
#include "aether/renderer/renderer.h"
#include "aether/scene/components.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/visibility.h"
#include "aether/scene/world.h"

#include <doctest/doctest.h>

using namespace aether;
using namespace aether::editor;

namespace {
bool near(const Vec3& a, const Vec3& b, f32 eps = 1e-3f) { return glm::all(glm::lessThan(glm::abs(a - b), Vec3(eps))); }

class NullRenderer final : public renderer::Renderer {
public:
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
