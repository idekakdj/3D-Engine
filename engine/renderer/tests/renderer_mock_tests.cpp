// renderer_mock_tests.cpp — end-to-end Renderer on the validating mock device: real shader
// compilation, resource registration, several frames with every pass enabled, state
// contract of the RenderTarget, resize, debug views, releases and hot reload.
#include "mock_device.h"

#include "aether/core/job_system.h"
#include "aether/renderer/instance_flags.h"
#include "aether/renderer/renderer.h"

#include <doctest/doctest.h>

#include <vector>

using namespace aether;
using namespace aether::renderer;

namespace {
struct CubeMesh {
    std::vector<Vertex>     vertices;
    std::vector<SkinVertex> skin;
    std::vector<u32>        indices;
};

CubeMesh make_cube() {
    CubeMesh m;
    for (int face = 0; face < 6; ++face) {
        const int  axis = face / 2;
        const f32  sign = (face % 2) ? -1.0f : 1.0f;
        Vec3       n(0.0f);
        n[axis] = sign;
        const Vec3 u = axis == 0 ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        const Vec3 v = glm::cross(n, u);
        const u32  base = static_cast<u32>(m.vertices.size());
        for (int k = 0; k < 4; ++k) {
            Vertex vx;
            const f32 a = (k & 1) ? 1.0f : -1.0f;
            const f32 b = (k & 2) ? 1.0f : -1.0f;
            vx.position = n + u * a + v * b;
            vx.normal = n;
            vx.tangent = Vec4(u, 1.0f);
            vx.uv0 = Vec2(a, b) * 0.5f + 0.5f;
            m.vertices.push_back(vx);
            SkinVertex s;
            s.joints[0] = static_cast<u16>(k & 1);
            s.weights = Vec4(1.0f, 0.0f, 0.0f, 0.0f);
            m.skin.push_back(s);
        }
        for (u32 i : { 0u, 1u, 3u, 0u, 3u, 2u }) {
            m.indices.push_back(base + i);
        }
    }
    return m;
}

RenderScene make_scene(MeshHandle mesh, MeshHandle skinned, MaterialHandle opaque, MaterialHandle masked,
                       MaterialHandle glass, EnvHandle env, u32 count) {
    RenderScene s;
    s.view.view = look_at(Vec3(0, 5, 20), Vec3(0, 0, 0), Vec3(0, 1, 0));
    s.view.proj = perspective(60.0f * kDeg2Rad, 16.0f / 9.0f, 0.1f, 500.0f);
    s.view.view_proj = s.view.proj * s.view.view;
    s.view.camera_position = Vec3(0, 5, 20);
    s.environment.skybox = env;
    for (u32 i = 0; i < count; ++i) {
        RenderMeshInstance inst;
        inst.mesh = mesh;
        inst.material = i % 3 == 0 ? opaque : (i % 3 == 1 ? masked : glass);
        inst.transform = glm::translate(Mat4(1.0f), Vec3(f32(i % 30) * 3.0f - 45.0f, 0.0f, -f32(i / 30) * 3.0f));
        inst.flags = instance_flags::kCastShadow;
        s.instances.push_back(inst);
    }
    RenderMeshInstance sk;
    sk.mesh = skinned;
    sk.material = opaque;
    sk.flags = instance_flags::kCastShadow | instance_flags::kSkinned;
    sk.first_joint = 0;
    sk.joint_count = 2;
    s.instances.push_back(sk);
    s.joint_matrices = { Mat4(1.0f), glm::translate(Mat4(1.0f), Vec3(0, 1, 0)) };

    RenderMeshInstance broken; // invalid mesh handle: must be skipped silently
    s.instances.push_back(broken);

    RenderLight sun;
    sun.type = LightType::Directional;
    sun.direction = glm::normalize(Vec3(0.3f, -1.0f, 0.2f));
    sun.intensity = 3.0f;
    sun.cast_shadows = true;
    s.lights.push_back(sun);
    for (int i = 0; i < 40; ++i) {
        RenderLight l;
        l.type = i % 2 ? LightType::Spot : LightType::Point;
        l.position = Vec3(f32(i) - 20.0f, 2.0f, -5.0f);
        l.range = 6.0f;
        s.lights.push_back(l);
    }
    s.debug_lines.push_back(RenderLine{ Vec3(0), Vec3(10, 0, 0), 0xFF0000FFu, 0xFF0000FFu });
    return s;
}
} // namespace

TEST_CASE("renderer end-to-end on the mock device") {
    JobSystem::initialize(2);
    test::MockDevice device;
    {
        auto created = Renderer::create(RendererDesc{ &device, UVec2(320, 180) });
        REQUIRE(created);
        std::unique_ptr<Renderer> r = std::move(created.value());
        CHECK(device.state.errors.empty());

        const CubeMesh cube = make_cube();
        MeshUpload     mu;
        mu.vertices = cube.vertices;
        mu.indices = cube.indices;
        mu.debug_name = "Cube";
        const MeshHandle mesh = r->register_mesh(mu);
        REQUIRE(mesh.is_valid());
        mu.skin = cube.skin;
        mu.debug_name = "SkinnedCube";
        const MeshHandle skinned = r->register_mesh(mu);
        REQUIRE(skinned.is_valid());

        std::vector<byte> pixels(4 * 4 * 4, byte{ 0x80 });
        TextureUpload     tu;
        tu.width = 4;
        tu.height = 4;
        tu.pixels = pixels;
        const TextureHandle tex = r->register_texture(tu);
        REQUIRE(tex.is_valid());
        tu.pixels = ByteSpan(pixels.data(), 7); // wrong size -> rejected
        CHECK_FALSE(r->register_texture(tu).is_valid());

        MaterialDesc md;
        md.base_color_tex = tex;
        const MaterialHandle opaque = r->register_material(md);
        md.blend = BlendMode::Masked;
        md.double_sided = true;
        const MaterialHandle masked = r->register_material(md);
        md.blend = BlendMode::Translucent;
        const MaterialHandle glass = r->register_material(md);

        std::vector<f32>  hdr(32 * 16 * 4, 1.0f);
        EnvironmentUpload eu;
        eu.width = 32;
        eu.height = 16;
        eu.rgba32f = hdr;
        const EnvHandle env = r->register_environment(eu);
        CHECK(env.is_valid());
        CHECK(device.state.errors.empty());

        RenderScene scene = make_scene(mesh, skinned, opaque, masked, glass, env, 600);
        const rhi::TextureHandle swap = device.make_target(320, 180);
        RenderTarget target{ swap, rhi::Format::BGRA8Unorm, UVec2(320, 180), rhi::ResourceState::Undefined,
                             rhi::ResourceState::ColorAttachment };

        for (int frame = 0; frame < 4; ++frame) {
            device.state.texture_state[swap.value] = rhi::ResourceState::Undefined; // new backbuffer
            r->render(scene, device.cmd_, target);
            CHECK(device.cmd_.balanced());
            CHECK(device.state.texture_state[swap.value] == rhi::ResourceState::ColorAttachment);
        }
        const RendererStats st = r->stats();
        CHECK(st.instances_submitted == 602);
        CHECK(st.instances_visible > 0);
        CHECK(st.instances_visible < 602);
        CHECK(st.draw_calls > 0);
        CHECK(st.triangles > 0);
        CHECK(st.lights == 41);
        CHECK(device.state.dispatches > 0);

        // Editor-style offscreen target at a different extent, left in ShaderRead.
        r->resize(UVec2(200, 120));
        const rhi::TextureHandle viewport = device.make_target(200, 120, rhi::Format::RGBA8Srgb);
        RenderTarget vt{ viewport, rhi::Format::RGBA8Srgb, UVec2(200, 120), rhi::ResourceState::Undefined,
                         rhi::ResourceState::ShaderRead };
        for (int frame = 0; frame < 2; ++frame) {
            r->render(scene, device.cmd_, vt);
            CHECK(device.state.texture_state[viewport.value] == rhi::ResourceState::ShaderRead);
            vt.initial_state = rhi::ResourceState::ShaderRead;
        }

        // Every debug view and every feature toggle records a valid frame.
        for (u8 dv = 0; dv <= static_cast<u8>(DebugView::Overdraw); ++dv) {
            r->settings().debug_view = static_cast<DebugView>(dv);
            r->render(scene, device.cmd_, vt);
        }
        r->settings().debug_view = DebugView::None;
        r->settings().shadows = false;
        r->settings().ssao = false;
        r->settings().taa = false;
        r->settings().bloom = false;
        r->settings().frustum_culling = false;
        r->render(scene, device.cmd_, vt);
        CHECK(r->stats().instances_visible == 601); // all valid instances

        // Releases are deferred and safe while instances still reference the handles.
        r->release(tex);
        r->release(glass);
        r->release(env);
        r->release(skinned);
        for (int frame = 0; frame < 4; ++frame) {
            r->render(scene, device.cmd_, vt);
        }
        CHECK(r->reload_shaders());
        r->render(scene, device.cmd_, vt);
        CHECK(device.cmd_.balanced());
    }
    CHECK(device.state.errors.empty());
    for (const std::string& e : device.state.errors) {
        MESSAGE(e);
    }
    JobSystem::shutdown();
}

TEST_CASE("renderer: empty scene and invalid target are handled") {
    test::MockDevice device;
    auto created = Renderer::create(RendererDesc{ &device, UVec2(64, 64) });
    REQUIRE(created);
    std::unique_ptr<Renderer> r = std::move(created.value());
    RenderScene  scene;
    const rhi::TextureHandle swap = device.make_target(64, 64);
    r->render(scene, device.cmd_, RenderTarget{ swap, rhi::Format::BGRA8Unorm, UVec2(64, 64) });
    CHECK(device.state.texture_state[swap.value] == rhi::ResourceState::ColorAttachment);
    r->render(scene, device.cmd_, RenderTarget{}); // skipped with a warning
    CHECK(device.cmd_.balanced());
    CHECK(device.state.errors.empty());
    CHECK_FALSE(Renderer::create(RendererDesc{}));
}
