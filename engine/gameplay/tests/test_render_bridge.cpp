// test_render_bridge.cpp — RenderResourceCache (mock renderer) and ECS -> RenderScene extraction.
#include "gameplay_test_utils.h"

#include "aether/assets/importers.h"
#include "aether/gameplay/components.h"
#include "aether/gameplay/environment.h"
#include "aether/gameplay/procedural_mesh.h"
#include "aether/gameplay/render_bridge.h"
#include "aether/renderer/instance_flags.h"
#include "aether/scene/components.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/visibility.h"
#include "aether/scene/world.h"

#include <doctest/doctest.h>

#include <fstream>

using namespace aether;
using namespace aether::gameplay;
using namespace aether::gameplay::test;

namespace {
bool near(const Vec3& a, const Vec3& b, f32 eps = 1e-4f) {
    return glm::all(glm::lessThan(glm::abs(a - b), Vec3(eps)));
}
Entity mesh_entity(World& w, const char* name, AssetId mesh, AssetId material = {}) {
    const Entity          e = w.create(name);
    MeshRendererComponent mr;
    mr.mesh     = mesh;
    mr.material = material;
    w.add<MeshRendererComponent>(e, mr);
    return e;
}
} // namespace

TEST_CASE("procedural meshes are well formed") {
    for (u8 i = 0; i < static_cast<u8>(BuiltinMesh::Count); ++i) {
        const auto             kind = static_cast<BuiltinMesh>(i);
        const assets::MeshData m    = make_builtin_mesh(kind);
        REQUIRE_FALSE(m.vertices.empty());
        REQUIRE(m.indices.size() % 3 == 0);
        REQUIRE(m.submeshes.size() == 1);
        CHECK(m.submeshes[0].index_count == m.indices.size());
        for (const u32 idx : m.indices) {
            REQUIRE(idx < m.vertices.size());
        }
        for (const Vertex& v : m.vertices) {
            CHECK(glm::length(v.normal) == doctest::Approx(1.0f).epsilon(1e-3));
            CHECK(std::abs(v.tangent.w) == doctest::Approx(1.0f));
        }
        // Counter-clockwise front faces: every triangle's geometric normal agrees with its vertices'.
        for (usize t = 0; t < m.indices.size(); t += 3) {
            const Vertex& a = m.vertices[m.indices[t]];
            const Vertex& b = m.vertices[m.indices[t + 1]];
            const Vertex& c = m.vertices[m.indices[t + 2]];
            const Vec3    n = glm::cross(b.position - a.position, c.position - a.position);
            if (glm::length(n) > 1e-8f) {
                CHECK(glm::dot(n, a.normal + b.normal + c.normal) > 0.0f);
            }
        }
        CHECK(builtin_mesh_from_id(builtin_mesh_id(kind)) == kind);
    }
    const assets::MeshData box = make_box_mesh(Vec3(1, 2, 3));
    CHECK(near(box.bounds.min, Vec3(-1, -2, -3)));
    CHECK(near(box.bounds.max, Vec3(1, 2, 3)));
    CHECK_FALSE(builtin_mesh_from_id(AssetId{ 5, 5 }).has_value());
    for (u8 i = 0; i < static_cast<u8>(BuiltinMaterial::Count); ++i) {
        const auto m = static_cast<BuiltinMaterial>(i);
        CHECK(builtin_material_from_id(builtin_material_id(m)) == m);
    }
    CHECK(builtin_material_id(BuiltinMaterial::Default) == default_material_id());
    CHECK(make_builtin_material(BuiltinMaterial::Gold).metallic_factor == 1.0f);
}

TEST_CASE("cache: built-in and runtime assets without an asset manager") {
    MockRenderer r;
    {
        RenderResourceCache cache(r, nullptr);
        const auto*         cube = cache.mesh(builtin_mesh_id(BuiltinMesh::Cube));
        REQUIRE(cube != nullptr);
        CHECK(cube->handle.is_valid());
        CHECK(cube->submeshes.size() == 1);
        CHECK(cache.mesh(builtin_mesh_id(BuiltinMesh::Cube)) == cube); // cached
        CHECK(r.meshes.size() == 1);

        CHECK(cache.mesh(AssetId{}) == nullptr);
        CHECK(cache.mesh(AssetId{ 42, 42 }) == nullptr); // unknown without assets: failed
        const renderer::MaterialHandle def = cache.default_material();
        CHECK(cache.material(AssetId{ 42, 42 }) == def);
        CHECK(cache.material(AssetId{}) == def);
        CHECK(cache.material(default_material_id()).is_valid());
        const renderer::MaterialHandle gold = cache.material(builtin_material_id(BuiltinMaterial::Gold));
        CHECK(gold != def);
        CHECK(r.materials.back().metallic == 1.0f);

        assets::MaterialData red;
        red.base_color_factor = Vec4(1, 0, 0, 1);
        red.alpha_mode        = assets::AlphaMode::Mask;
        cache.add_material(AssetId{ 7, 7 }, red);
        const renderer::MaterialHandle red_h = cache.material(AssetId{ 7, 7 });
        CHECK(red_h != def);
        CHECK(r.materials.back().base_color == Vec4(1, 0, 0, 1));
        CHECK(r.materials.back().blend == renderer::BlendMode::Masked);
        red.base_color_factor = Vec4(0, 1, 0, 1);
        cache.add_material(AssetId{ 7, 7 }, red); // replacing updates in place
        CHECK(cache.material(AssetId{ 7, 7 }) == red_h);
        CHECK(r.material_updates.back().desc.base_color == Vec4(0, 1, 0, 1));

        cache.add_mesh(AssetId{ 8, 8 }, make_sphere_mesh(1.0f, 8, 4), "ball");
        REQUIRE(cache.mesh(AssetId{ 8, 8 }) != nullptr);
        CHECK(r.meshes.back() == "ball");

        const RenderCacheStats s = cache.stats();
        CHECK(s.meshes == 2);
        CHECK(s.failed >= 1);
        cache.invalidate(AssetId{ 8, 8 });
        CHECK(r.released_meshes == 1);
    }
    // Destruction releases everything that was registered.
    CHECK(r.released_meshes == 2);
    CHECK(r.released_materials >= 3);
}

TEST_CASE("cache: meshes, materials and textures from the asset manager") {
    AssetFixture assets(/*use_jobs=*/false);
    MockRenderer r;
    RenderResourceCache cache(r, &assets.manager);

    const AssetId mesh_id     = assets.manager.resolve("samples/cube/cube.gltf", "mesh:0");
    const AssetId material_id = assets.manager.resolve("samples/cube/cube.gltf", "material:0");
    REQUIRE(mesh_id.is_valid());
    REQUIRE(material_id.is_valid());

    // Synchronous manager: data is ready on first use.
    const auto* mesh = cache.mesh(mesh_id);
    REQUIRE(mesh != nullptr);
    CHECK(mesh->submeshes.size() >= 1);
    const renderer::MaterialHandle mat = cache.material(material_id);
    CHECK(mat != cache.default_material());
    const renderer::MaterialDesc& desc = r.materials.front();
    CHECK(desc.base_color_tex.is_valid()); // the cube material is textured
    REQUIRE_FALSE(r.textures.empty());
    CHECK(r.textures.front().format == rhi::Format::RGBA8Srgb);
    CHECK(r.textures.front().width > 0);

    cache.update();
    const RenderCacheStats s = cache.stats();
    CHECK(s.meshes == 1);
    CHECK(s.materials >= 1);
    CHECK(s.textures >= 1);
    CHECK(s.pending == 0);
    CHECK(s.failed == 0);
}

TEST_CASE("cache: asynchronous loads resolve on later frames") {
    AssetFixture assets(/*use_jobs=*/true);
    MockRenderer r;
    RenderResourceCache cache(r, &assets.manager);
    const AssetId mesh_id = assets.manager.resolve("samples/cube/cube.gltf", "mesh:0");
    const RenderResourceCache::Mesh* mesh = cache.mesh(mesh_id);
    for (int i = 0; i < 200 && mesh == nullptr; ++i) {
        assets.manager.wait_all();
        cache.update();
        mesh = cache.mesh(mesh_id);
    }
    REQUIRE(mesh != nullptr);
    CHECK(mesh->handle.is_valid());
}

TEST_CASE("extract: camera, lights, instances, visibility and overrides") {
    MockRenderer        r;
    RenderResourceCache cache(r, nullptr);
    World               world;

    const Entity cam = world.create("Camera");
    world.add<CameraComponent>(cam, CameraComponent{ 90.0f, 0.5f, 200.0f, true });
    scene::set_local_position(world, cam, Vec3(0, 0, 10));
    const Entity other_cam = world.create("Other");
    world.add<CameraComponent>(other_cam, CameraComponent{ 60.0f, 0.1f, 10.0f, false });

    const Entity sun = world.create("Sun");
    LightComponent sl;
    sl.kind = LightKind::Directional;
    world.add<LightComponent>(sun, sl);
    scene::set_local_rotation(world, sun, glm::angleAxis(-kHalfPi, Vec3(1, 0, 0))); // points down
    const Entity spot = world.create("Spot");
    LightComponent sp;
    sp.kind           = LightKind::Spot;
    sp.inner_cone_deg = 20.0f;
    sp.outer_cone_deg = 10.0f; // outer < inner: clamped to inner
    world.add<LightComponent>(spot, sp);

    const AssetId red{ 1, 1 };
    assets::MaterialData red_data;
    red_data.base_color_factor = Vec4(1, 0, 0, 1);
    cache.add_material(red, red_data);

    const Entity box = mesh_entity(world, "Box", builtin_mesh_id(BuiltinMesh::Cube), red);
    scene::set_local_position(world, box, Vec3(5, 0, 0));
    scene::set_local_scale(world, box, Vec3(2.0f));
    const Entity hidden = mesh_entity(world, "Hidden", builtin_mesh_id(BuiltinMesh::Sphere));
    scene::set_visible(world, hidden, false);
    const Entity missing = mesh_entity(world, "Missing", AssetId{ 99, 99 });
    (void)missing;

    // Two-slot runtime mesh with a per-slot override.
    assets::MeshData two = make_box_mesh();
    two.submeshes.push_back(two.submeshes[0]);
    two.submeshes[0].index_count   = 18;
    two.submeshes[1].first_index   = 18;
    two.submeshes[1].index_count   = 18;
    two.submeshes[1].material_slot = 1;
    cache.add_mesh(AssetId{ 2, 2 }, two);
    const Entity multi = mesh_entity(world, "Multi", AssetId{ 2, 2 });
    world.add<MaterialOverridesComponent>(multi, MaterialOverridesComponent{ { AssetId{}, red } });
    world.get<MeshRendererComponent>(multi).cast_shadows = false;
    world.update_transforms();

    SceneExtractOptions opt;
    opt.viewport = UVec2(1920, 1080);
    renderer::RenderScene scene_out;
    scene_out.debug_lines.push_back({}); // cleared by extraction
    const SceneExtractStats st = extract_render_scene(world, cache, opt, scene_out);

    CHECK((st.camera == cam));
    CHECK(near(scene_out.view.camera_position, Vec3(0, 0, 10)));
    CHECK(scene_out.view.near_z == 0.5f);
    CHECK(scene_out.view.far_z == 200.0f);
    CHECK(scene_out.view.viewport == UVec2(1920, 1080));
    CHECK(near(Vec3(scene_out.view.view * Vec4(0, 0, 0, 1)), Vec3(0, 0, -10)));
    CHECK(scene_out.debug_lines.empty());

    // Output order follows ECS storage order: look items up by what they are.
    REQUIRE(st.lights == 2);
    const auto find_light = [&](renderer::LightType type) -> const renderer::RenderLight& {
        for (const renderer::RenderLight& l : scene_out.lights) {
            if (l.type == type) {
                return l;
            }
        }
        FAIL("light type not found");
        return scene_out.lights.front();
    };
    const renderer::RenderLight& sun_l = find_light(renderer::LightType::Directional);
    CHECK(near(sun_l.direction, Vec3(0, -1, 0)));
    const renderer::RenderLight& spot_l = find_light(renderer::LightType::Spot);
    CHECK(spot_l.inner_cone == doctest::Approx(std::cos(20.0f * kDeg2Rad)));
    CHECK(spot_l.outer_cone == doctest::Approx(spot_l.inner_cone));

    CHECK(st.pending_meshes == 1);
    REQUIRE(st.instances == 3); // box + 2 submeshes of multi
    const auto find_instance = [&](renderer::MeshHandle mesh, u32 submesh) -> const renderer::RenderMeshInstance& {
        for (const renderer::RenderMeshInstance& i : scene_out.instances) {
            if (i.mesh == mesh && i.submesh == submesh) {
                return i;
            }
        }
        FAIL("instance not found");
        return scene_out.instances.front();
    };
    const renderer::MeshHandle cube_h  = cache.mesh(builtin_mesh_id(BuiltinMesh::Cube))->handle;
    const renderer::MeshHandle multi_h = cache.mesh(AssetId{ 2, 2 })->handle;
    const renderer::RenderMeshInstance& b = find_instance(cube_h, 0);
    CHECK(b.material == cache.material(red));
    CHECK(near(b.world_bounds.min, Vec3(4, -1, -1)));
    CHECK(near(b.world_bounds.max, Vec3(6, 1, 1)));
    CHECK(renderer::instance_flags::has(b.flags, renderer::instance_flags::kCastShadow));
    CHECK(find_instance(multi_h, 0).material == cache.default_material()); // slot 0: no override
    CHECK(find_instance(multi_h, 1).material == cache.material(red));      // slot 1: override
    CHECK(find_instance(multi_h, 0).flags == 0u);

    // Camera override wins; the primary flag picks between cameras.
    CameraOverride ov;
    ov.view  = look_at(Vec3(1, 2, 3), Vec3(0), Vec3(0, 1, 0));
    opt.camera = ov;
    const SceneExtractStats st2 = extract_render_scene(world, cache, opt, scene_out);
    CHECK((st2.camera == kNullEntity));
    CHECK(near(scene_out.view.camera_position, Vec3(1, 2, 3), 1e-3f));
    opt.camera.reset();
    world.get<CameraComponent>(cam).primary = false;
    CHECK((find_primary_camera(world) == cam)); // no primary: first visible camera
    world.get<CameraComponent>(other_cam).primary = true;
    CHECK((find_primary_camera(world) == other_cam));
}

TEST_CASE("extract: hooks for physics poses and skinning palettes") {
    MockRenderer        r;
    RenderResourceCache cache(r, nullptr);
    World               world;
    assets::MeshData    skinned = make_box_mesh();
    skinned.skin.resize(skinned.vertices.size());
    cache.add_mesh(AssetId{ 3, 3 }, skinned);
    const Entity e    = mesh_entity(world, "Skinned", AssetId{ 3, 3 });
    const Entity body = mesh_entity(world, "Body", builtin_mesh_id(BuiltinMesh::Cube));
    world.update_transforms();

    const std::vector<Mat4> palette(4, Mat4(2.0f));
    SceneExtractOptions     opt;
    opt.skinning_palette = [&](Entity which) -> std::span<const Mat4> {
        return which == e ? std::span<const Mat4>(palette) : std::span<const Mat4>{};
    };
    opt.world_matrix = [&](Entity which, Mat4& out) {
        if (which != body) {
            return false;
        }
        out = glm::translate(Mat4(1.0f), Vec3(0, 7, 0));
        return true;
    };
    renderer::RenderScene out;
    const SceneExtractStats st = extract_render_scene(world, cache, opt, out);
    REQUIRE(st.instances == 2);
    CHECK(st.skinned == 1);
    const bool skinned_first               = out.instances[0].mesh == cache.mesh(AssetId{ 3, 3 })->handle;
    const renderer::RenderMeshInstance& s = out.instances[skinned_first ? 0 : 1];
    const renderer::RenderMeshInstance& p = out.instances[skinned_first ? 1 : 0];
    CHECK(renderer::instance_flags::has(s.flags, renderer::instance_flags::kSkinned));
    CHECK(renderer::instance_flags::has(s.flags, renderer::instance_flags::kNeverCull));
    CHECK(s.first_joint == 0);
    CHECK(s.joint_count == 4);
    CHECK(out.joint_matrices.size() == 4);
    CHECK(near(Vec3(p.transform[3]), Vec3(0, 7, 0)));
    CHECK(p.joint_count == 0);
}

TEST_CASE("extract: GI volume box, probe counts and switches (ADR-0016)") {
    GIVolumeComponent v;
    v.probe_spacing = 2.0f;
    v.intensity = 0.5f;
    Mat4 m = glm::translate(Mat4(1.0f), Vec3(1, 2, 3)) * glm::rotate(Mat4(1.0f), 0.3f, Vec3(0, 1, 0)) *
             glm::scale(Mat4(1.0f), Vec3(10, 4, 1));
    const renderer::GiVolume g = gi_volume_from(m, v);
    CHECK(g.enabled);
    CHECK(glm::all(glm::epsilonEqual(g.min, Vec3(-4, 0, 2.5f), 1e-4f)));
    CHECK(glm::all(glm::epsilonEqual(g.max, Vec3(6, 4, 3.5f), 1e-4f)));
    CHECK(g.probe_counts == UVec3(6, 3, 2)); // size / spacing + 1, at least 2
    CHECK(g.intensity == 0.5f);
    v.probe_spacing = 0.0f; // clamped: at most 64 per axis
    CHECK(gi_volume_from(glm::scale(Mat4(1.0f), Vec3(100.0f)), v).probe_counts == UVec3(64));
    CHECK_FALSE(gi_volume_from(glm::scale(Mat4(1.0f), Vec3(1, 0, 1)), v).enabled);

    MockRenderer        r;
    RenderResourceCache cache(r, nullptr);
    World               world;
    const Entity        off = world.create("Off");
    world.add<GIVolumeComponent>(off, GIVolumeComponent{ 1.0f, 1.0f, false });
    renderer::RenderScene out;
    extract_render_scene(world, cache, SceneExtractOptions{}, out);
    CHECK_FALSE(out.gi.enabled);
    const Entity on = world.create("On");
    world.add<GIVolumeComponent>(on, GIVolumeComponent{});
    scene::set_local_scale(world, on, Vec3(4.0f));
    extract_render_scene(world, cache, SceneExtractOptions{}, out);
    CHECK(out.gi.enabled);
    CHECK(out.gi.probe_counts == UVec3(5));
    scene::set_visible(world, on, false);
    extract_render_scene(world, cache, SceneExtractOptions{}, out);
    CHECK_FALSE(out.gi.enabled);
}

TEST_CASE("extract: reflection probes (ADR-0017)") {
    const ReflectionProbeComponent pc{ 1.5f, 0.5f, true };
    const renderer::ReflectionProbe p =
        reflection_probe_from(glm::translate(Mat4(1.0f), Vec3(1, 2, 3)) * glm::scale(Mat4(1.0f), Vec3(4, 2, 6)), pc, 9);
    CHECK(p.id == 9);
    CHECK(p.position == Vec3(1, 2, 3));
    CHECK(glm::all(glm::epsilonEqual(p.box_min, Vec3(-1, 1, 0), 1e-4f)));
    CHECK(glm::all(glm::epsilonEqual(p.box_max, Vec3(3, 3, 6), 1e-4f)));
    CHECK(p.intensity == 1.5f);
    CHECK(p.blend_distance == 0.5f);

    MockRenderer        r;
    RenderResourceCache cache(r, nullptr);
    World               world;
    const Entity        a = world.create("A");
    world.add<ReflectionProbeComponent>(a);
    const Entity b = world.create("B");
    world.add<ReflectionProbeComponent>(b, ReflectionProbeComponent{ 1.0f, 1.0f, false });
    renderer::RenderScene out;
    extract_render_scene(world, cache, SceneExtractOptions{}, out);
    REQUIRE(out.reflection_probes.size() == 1); // disabled probes are skipped
    const u32 id = out.reflection_probes[0].id;
    CHECK(id != 0);
    extract_render_scene(world, cache, SceneExtractOptions{}, out);
    REQUIRE(out.reflection_probes.size() == 1); // cleared per extract, id stable
    CHECK(out.reflection_probes[0].id == id);
    scene::set_visible(world, a, false);
    extract_render_scene(world, cache, SceneExtractOptions{}, out);
    CHECK(out.reflection_probes.empty());
}

TEST_CASE("transform_aabb and make_render_view") {
    AABB local;
    local.min = Vec3(-1);
    local.max = Vec3(1);
    const Mat4 rot = glm::rotate(Mat4(1.0f), kPi / 4.0f, Vec3(0, 1, 0));
    const AABB w   = transform_aabb(rot, local);
    CHECK(w.max.x == doctest::Approx(std::sqrt(2.0f)));
    CHECK(w.max.y == doctest::Approx(1.0f));

    // Scale on the camera entity does not skew the view.
    const Mat4 cam = glm::scale(glm::translate(Mat4(1.0f), Vec3(0, 0, 5)), Vec3(3.0f));
    const renderer::RenderView v = make_render_view(cam, kHalfPi, 0.1f, 100.0f, UVec2(100, 50));
    CHECK(near(Vec3(v.view * Vec4(0, 0, 0, 1)), Vec3(0, 0, -5)));
    CHECK(v.proj[0][0] == doctest::Approx(0.5f)); // aspect 2, fov 90
    CHECK(v.view_proj == v.proj * v.view);
}

TEST_CASE("procedural sky: size, sun placement and sky/ground split") {
    SkySettings sky;
    sky.sun_direction = Vec3(1, 1, 0);
    const u32              w   = 64;
    const u32              h   = 32;
    const std::vector<f32> img = make_sky_equirect(w, h, sky);
    REQUIRE(img.size() == static_cast<usize>(w) * h * 4);
    // Brightest pixel lies in the sun direction under the renderer's equirect mapping.
    usize best = 0;
    for (usize i = 0; i < img.size(); i += 4) {
        if (img[i] + img[i + 1] + img[i + 2] > img[best] + img[best + 1] + img[best + 2]) {
            best = i;
        }
    }
    const usize px    = best / 4;
    const f32   u     = (static_cast<f32>(px % w) + 0.5f) / static_cast<f32>(w);
    const f32   v     = (static_cast<f32>(px / w) + 0.5f) / static_cast<f32>(h);
    const f32   theta = v * kPi;
    const f32   phi   = (u - 0.5f) * kTwoPi;
    const Vec3  dir(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
    CHECK(glm::dot(dir, glm::normalize(sky.sun_direction)) > 0.95f);
    // Zenith is bluer than the ground; everything is finite and opaque.
    CHECK(img[2] > img[(static_cast<usize>(h - 1) * w) * 4 + 2]);
    for (usize i = 0; i < img.size(); i += 4) {
        REQUIRE(std::isfinite(img[i]));
        REQUIRE(img[i + 3] == 1.0f);
    }
}
