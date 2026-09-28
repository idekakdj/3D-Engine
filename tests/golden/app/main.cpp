// aether-golden — golden-image render tests.
//
//   aether-golden --case <name> [--update] [--reference-dir D] [--output-dir D] [--no-mesh-shading]
//                 [shared app flags]
//   aether-golden --list
//
// Builds the case's scene with built-in meshes and runtime materials, renders it offscreen at a
// fixed 320x240 for kWarmupFrames frames (TAA, bloom and IBL settle deterministically: nothing
// in these scenes depends on wall-clock time), reads the final target back and compares it with
// <reference-dir>/<device class>/<case>.png (image_compare.h). --update (re)writes the reference.
// On a mismatch the actual image and a diff image are written to the output directory.
// --no-mesh-shading draws static meshes through the indexed indirect path instead of the
// meshlet task/mesh shaders (ADR-0010); both paths must match the same reference.
//
// Exit codes: 0 match / reference written, 1 mismatch or failure, 2 usage, 77 skipped (no
// reference recorded for this device class, or the case needs a feature the device lacks).
#include "aether/core/log.h"
#include "aether/core/paths.h"
#include "aether/golden/image_compare.h"
#include "aether/assets/asset_traits.h"
#include "aether/assets/asset_types.h"
#include "aether/gameplay/application.h"
#include "aether/gameplay/procedural_mesh.h"
#include "aether/gameplay/render_bridge.h"
#include "aether/renderer/renderer.h"
#include "aether/rhi/device.h"
#include "aether/rhi/device_ext.h"
#include "aether/scene/components.h"
#include "aether/scene/transform_utils.h"
#include "aether/scene/world.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace aether;
using namespace aether::gameplay;

namespace {

constexpr u32 kWidth        = 320;
constexpr u32 kHeight       = 240;
constexpr u64 kWarmupFrames = 40;
constexpr int kExitSkip     = 77;

// ---- scene building helpers ---------------------------------------------------------------------

Quat yaw_pitch(f32 yaw_deg, f32 pitch_deg) {
    return glm::angleAxis(glm::radians(yaw_deg), Vec3(0, 1, 0)) * glm::angleAxis(glm::radians(pitch_deg), Vec3(1, 0, 0));
}

struct SceneBuilder {
    World&               w;
    RenderResourceCache& cache;

    AssetId material(const char* name, Vec3 color, f32 metallic, f32 roughness, Vec3 emissive = Vec3(0.0f),
                     f32 alpha = 1.0f) {
        assets::MaterialData m;
        m.name              = name;
        m.base_color_factor = Vec4(color, alpha);
        m.metallic_factor   = metallic;
        m.roughness_factor  = roughness;
        m.emissive_factor   = emissive;
        if (alpha < 1.0f) {
            m.alpha_mode = assets::AlphaMode::Blend;
        }
        const AssetId id = assets::make_asset_id("golden", name);
        cache.add_material(id, m);
        return id;
    }

    Entity mesh(const char* name, BuiltinMesh mesh, const AssetId& mat, Vec3 pos, Vec3 scale = Vec3(1.0f),
                Quat rot = Quat(1, 0, 0, 0)) {
        const Entity e = w.create(name);
        MeshRendererComponent mr;
        mr.mesh     = builtin_mesh_id(mesh);
        mr.material = mat;
        w.add<MeshRendererComponent>(e, mr);
        scene::set_local_position(w, e, pos);
        scene::set_local_rotation(w, e, rot);
        scene::set_local_scale(w, e, scale);
        return e;
    }

    void camera(Vec3 pos, f32 yaw, f32 pitch, f32 fov = 50.0f) {
        const Entity e = w.create("Camera");
        w.add<CameraComponent>(e, CameraComponent{ fov, 0.1f, 200.0f, true });
        scene::set_local_position(w, e, pos);
        scene::set_local_rotation(w, e, yaw_pitch(yaw, pitch));
    }

    void light(LightKind kind, Vec3 color, f32 intensity, Vec3 pos, Quat rot = Quat(1, 0, 0, 0), f32 range = 10.0f,
               bool shadows = false) {
        const Entity   e = w.create("Light");
        LightComponent l;
        l.kind         = kind;
        l.color        = color;
        l.intensity    = intensity;
        l.range        = range;
        l.cast_shadows = kind == LightKind::Directional || shadows;
        w.add<LightComponent>(e, l);
        scene::set_local_position(w, e, pos);
        scene::set_local_rotation(w, e, rot);
    }

    void sun(f32 intensity = 3.0f) {
        light(LightKind::Directional, Vec3(1.0f, 0.96f, 0.9f), intensity, Vec3(0.0f), yaw_pitch(35.0f, -50.0f));
    }

    // 5 roughness steps x 2 rows (dielectric, metal) over a floor.
    void pbr_spheres() {
        const AssetId floor = material("floor", Vec3(0.5f), 0.0f, 0.8f);
        mesh("Floor", BuiltinMesh::Plane, floor, Vec3(0.0f, -1.0f, 0.0f), Vec3(1.5f, 1.0f, 1.0f));
        for (int row = 0; row < 2; ++row) {
            for (int i = 0; i < 5; ++i) {
                const f32   rough = 0.05f + 0.2375f * f32(i);
                const std::string name = std::format("sphere_{}_{}", row, i);
                const AssetId m = material(name.c_str(), row == 0 ? Vec3(0.8f, 0.1f, 0.1f) : Vec3(0.95f, 0.75f, 0.4f),
                                           row == 0 ? 0.0f : 1.0f, rough);
                mesh(name.c_str(), BuiltinMesh::Sphere, m, Vec3(-3.0f + 1.5f * f32(i), 1.4f - 1.5f * f32(row), 0.0f),
                     Vec3(1.2f));
            }
        }
        camera(Vec3(0.0f, 0.8f, 7.5f), 0.0f, -6.0f);
        sun();
    }
};

// ---- cases --------------------------------------------------------------------------------------

struct CaseContext {
    SceneBuilder              b;
    renderer::RendererSettings& settings;
    RenderBridgeSubsystem&    bridge;
};

struct GoldenCase {
    const char*                             name;
    const char*                             what;
    bool                                    needs_shadows = false;
    std::function<void(CaseContext&)>       build;
};

void flat_environment(CaseContext& c, Vec3 ambient = Vec3(0.02f)) {
    renderer::EnvironmentSettings env;
    env.ambient_color     = ambient;
    env.ambient_intensity = 1.0f;
    c.bridge.set_environment(env);
    c.settings.ibl = false;
}

const std::vector<GoldenCase>& cases() {
    static const std::vector<GoldenCase> list = {
        { "pbr_spheres", "metal/roughness sweep, sun + procedural sky IBL, full post chain", false,
          [](CaseContext& c) { c.b.pbr_spheres(); } },
        { "pbr_spheres_no_post", "the PBR sweep without SSAO, bloom and TAA", false,
          [](CaseContext& c) {
              c.b.pbr_spheres();
              c.settings.ssao  = false;
              c.settings.bloom = false;
              c.settings.taa   = false;
          } },
        { "punctual_lights", "coloured point lights and a spot light, flat ambient, no IBL", false,
          [](CaseContext& c) {
              flat_environment(c);
              const AssetId floor = c.b.material("floor", Vec3(0.7f), 0.0f, 0.6f);
              const AssetId white = c.b.material("white", Vec3(0.9f), 0.0f, 0.4f);
              c.b.mesh("Floor", BuiltinMesh::Plane, floor, Vec3(0.0f));
              c.b.mesh("CubeA", BuiltinMesh::Cube, white, Vec3(-1.5f, 0.5f, 0.0f), Vec3(1.0f), yaw_pitch(30.0f, 0.0f));
              c.b.mesh("CubeB", BuiltinMesh::Cube, white, Vec3(1.5f, 0.5f, -0.5f), Vec3(1.0f), yaw_pitch(-20.0f, 0.0f));
              c.b.mesh("Ball", BuiltinMesh::Sphere, white, Vec3(0.0f, 0.5f, 1.2f));
              c.b.light(LightKind::Point, Vec3(1.0f, 0.2f, 0.1f), 15.0f, Vec3(-2.5f, 1.5f, 1.5f), Quat(1, 0, 0, 0), 8.0f);
              c.b.light(LightKind::Point, Vec3(0.1f, 0.4f, 1.0f), 15.0f, Vec3(2.5f, 1.5f, 1.5f), Quat(1, 0, 0, 0), 8.0f);
              c.b.light(LightKind::Spot, Vec3(1.0f, 1.0f, 0.8f), 40.0f, Vec3(0.0f, 4.0f, 1.0f), yaw_pitch(0.0f, -80.0f), 10.0f);
              c.b.camera(Vec3(0.0f, 3.0f, 6.5f), 0.0f, -22.0f);
          } },
        { "emissive_bloom", "emissive spheres in a dark scene with strong bloom", false,
          [](CaseContext& c) {
              flat_environment(c, Vec3(0.005f));
              c.settings.bloom_intensity = 0.15f;
              const AssetId floor = c.b.material("floor", Vec3(0.2f), 0.0f, 0.9f);
              c.b.mesh("Floor", BuiltinMesh::Plane, floor, Vec3(0.0f, -0.5f, 0.0f));
              const Vec3 colors[3] = { Vec3(4.0f, 0.6f, 0.2f), Vec3(0.3f, 3.0f, 0.6f), Vec3(0.4f, 0.8f, 5.0f) };
              for (int i = 0; i < 3; ++i) {
                  const std::string name = std::format("glow_{}", i);
                  const AssetId     m    = c.b.material(name.c_str(), Vec3(0.05f), 0.0f, 0.5f, colors[i]);
                  c.b.mesh(name.c_str(), BuiltinMesh::Sphere, m, Vec3(-2.0f + 2.0f * f32(i), 0.3f, 0.0f));
              }
              c.b.camera(Vec3(0.0f, 1.5f, 5.5f), 0.0f, -14.0f);
          } },
        { "translucency", "alpha-blended materials over opaque geometry", false,
          [](CaseContext& c) {
              const AssetId floor = c.b.material("floor", Vec3(0.6f), 0.0f, 0.7f);
              const AssetId blue  = c.b.material("blue", Vec3(0.1f, 0.2f, 0.9f), 0.0f, 0.4f);
              const AssetId glass = c.b.material("glass", Vec3(1.0f, 0.3f, 0.2f), 0.0f, 0.1f, Vec3(0.0f), 0.4f);
              c.b.mesh("Floor", BuiltinMesh::Plane, floor, Vec3(0.0f));
              c.b.mesh("Back", BuiltinMesh::Cube, blue, Vec3(0.0f, 0.75f, -1.0f), Vec3(1.5f));
              c.b.mesh("Glass", BuiltinMesh::Cube, glass, Vec3(0.4f, 0.6f, 0.6f), Vec3(1.2f), yaw_pitch(25.0f, 0.0f));
              c.b.camera(Vec3(0.0f, 2.2f, 5.0f), 0.0f, -18.0f);
              c.b.sun();
          } },
        { "debug_normals", "DebugView::Normals on the PBR sweep", false,
          [](CaseContext& c) {
              c.b.pbr_spheres();
              c.settings.debug_view = renderer::DebugView::Normals;
          } },
        { "debug_albedo", "DebugView::Albedo on the PBR sweep", false,
          [](CaseContext& c) {
              c.b.pbr_spheres();
              c.settings.debug_view = renderer::DebugView::Albedo;
          } },
        { "debug_roughness", "DebugView::Roughness on the PBR sweep", false,
          [](CaseContext& c) {
              c.b.pbr_spheres();
              c.settings.debug_view = renderer::DebugView::Roughness;
          } },
        { "spot_shadows", "two shadow-casting spot lights over cubes and a sphere (ADR-0012)", false,
          [](CaseContext& c) {
              flat_environment(c, Vec3(0.01f));
              const AssetId floor = c.b.material("floor", Vec3(0.75f), 0.0f, 0.8f);
              const AssetId grey  = c.b.material("grey", Vec3(0.6f), 0.0f, 0.5f);
              c.b.mesh("Floor", BuiltinMesh::Plane, floor, Vec3(0.0f));
              c.b.mesh("Pillar", BuiltinMesh::Cube, grey, Vec3(-1.2f, 1.0f, 0.0f), Vec3(0.6f, 2.0f, 0.6f));
              c.b.mesh("Box", BuiltinMesh::Cube, grey, Vec3(1.3f, 0.4f, 0.6f), Vec3(0.8f), yaw_pitch(35.0f, 0.0f));
              c.b.mesh("Ball", BuiltinMesh::Sphere, grey, Vec3(0.2f, 0.5f, -1.4f));
              c.b.light(LightKind::Spot, Vec3(1.0f, 0.95f, 0.85f), 60.0f, Vec3(-3.0f, 4.5f, 2.5f),
                        glm::quatLookAt(glm::normalize(Vec3(0.55f, -0.75f, -0.4f)), Vec3(0, 1, 0)), 14.0f, true);
              c.b.light(LightKind::Spot, Vec3(0.4f, 0.6f, 1.0f), 40.0f, Vec3(3.5f, 3.5f, -1.0f),
                        glm::quatLookAt(glm::normalize(Vec3(-0.7f, -0.7f, 0.1f)), Vec3(0, 1, 0)), 12.0f, true);
              c.b.camera(Vec3(0.0f, 5.5f, 8.0f), 0.0f, -32.0f);
          } },
        { "point_shadows", "a shadow-casting point light among pillars and a back wall (ADR-0015)", false,
          [](CaseContext& c) {
              flat_environment(c, Vec3(0.01f));
              const AssetId floor = c.b.material("floor", Vec3(0.75f), 0.0f, 0.8f);
              const AssetId grey  = c.b.material("grey", Vec3(0.6f), 0.0f, 0.5f);
              c.b.mesh("Floor", BuiltinMesh::Plane, floor, Vec3(0.0f));
              c.b.mesh("Wall", BuiltinMesh::Cube, floor, Vec3(0.0f, 1.5f, -3.2f), Vec3(9.0f, 3.0f, 0.2f));
              // Pillars all around the lamp: shadows fall across every horizontal cube face and
              // the face seams (diagonals), onto the floor and the wall.
              for (int i = 0; i < 6; ++i) {
                  const f32 a = glm::radians(30.0f + 60.0f * f32(i));
                  c.b.mesh("Pillar", BuiltinMesh::Cube, grey, Vec3(std::cos(a) * 1.6f, 0.6f, std::sin(a) * 1.6f - 0.4f),
                           Vec3(0.3f, 1.2f, 0.3f));
              }
              c.b.mesh("Ball", BuiltinMesh::Sphere, grey, Vec3(0.0f, 0.35f, -0.4f), Vec3(0.7f));
              c.b.light(LightKind::Point, Vec3(1.0f, 0.9f, 0.75f), 30.0f, Vec3(0.0f, 1.5f, -0.4f), Quat(1, 0, 0, 0), 12.0f,
                        true);
              c.b.camera(Vec3(0.0f, 5.0f, 6.5f), 0.0f, -38.0f);
          } },
        { "shadows", "cascaded sun shadows from a column of cubes", true,
          [](CaseContext& c) {
              const AssetId floor = c.b.material("floor", Vec3(0.8f), 0.0f, 0.7f);
              const AssetId grey  = c.b.material("grey", Vec3(0.5f), 0.0f, 0.5f);
              c.b.mesh("Floor", BuiltinMesh::Plane, floor, Vec3(0.0f), Vec3(2.0f, 1.0f, 2.0f));
              for (int i = 0; i < 4; ++i) {
                  c.b.mesh("Cube", BuiltinMesh::Cube, grey, Vec3(-3.0f + 2.0f * f32(i), 0.5f + 0.5f * f32(i), 0.0f),
                           Vec3(1.0f, 1.0f + f32(i), 1.0f));
              }
              c.b.camera(Vec3(0.0f, 5.0f, 9.0f), 0.0f, -28.0f);
              c.b.sun(4.0f);
          } },
    };
    return list;
}

const GoldenCase* find_case(std::string_view name) {
    for (const GoldenCase& c : cases()) {
        if (name == c.name) return &c;
    }
    return nullptr;
}

// ---- the harness app ----------------------------------------------------------------------------

struct GoldenArgs {
    std::string case_name;
    bool        update = false;
    bool        list   = false;
    bool        no_mesh_shading = false;
    fs::path    reference_dir;
    fs::path    output_dir;
    bool        usage_error = false;
};

class GoldenApp final : public Application {
public:
    GoldenApp(AppDesc desc, GoldenArgs args, const GoldenCase& c)
        : Application(std::move(desc)), args_(std::move(args)), case_(c) {}

    [[nodiscard]] int result() const noexcept { return result_; }

protected:
    Result<void> on_init() override {
        const std::string adapter = device().features().adapter_name;
        device_class_ = golden::device_class(adapter);
        if (case_.needs_shadows && !renderer().settings().shadows) {
            AE_LOG_WARN("Golden", "{}: skipped - shadows are unavailable on '{}'", case_.name, adapter);
            result_ = kExitSkip;
            request_exit(0);
            return {};
        }
        rhi::TextureDesc td;
        td.type       = rhi::TextureType::Tex2D;
        td.format     = rhi::Format::RGBA8Unorm;
        td.width      = kWidth;
        td.height     = kHeight;
        td.usage      = rhi::TextureUsage::ColorAttach | rhi::TextureUsage::TransferSrc | rhi::TextureUsage::Sampled;
        td.debug_name = "GoldenTarget";
        target_       = device().create_texture(td);
        if (!target_.is_valid()) {
            return Error{ ErrorCode::OutOfMemory, "cannot create the golden render target" };
        }
        set_render_extent(UVec2(kWidth, kHeight));
        if (args_.no_mesh_shading) {
            renderer().settings().mesh_shading = false;
        }

        auto* bridge = find_subsystem<RenderBridgeSubsystem>();
        if (bridge == nullptr || bridge->cache() == nullptr) {
            return Error{ ErrorCode::NotInitialized, "render bridge missing" };
        }
        CaseContext ctx{ SceneBuilder{ world(), *bridge->cache() }, renderer().settings(), *bridge };
        case_.build(ctx);
        AE_LOG_INFO("Golden", "case '{}' ({}) on '{}' [{}]", case_.name, case_.what, adapter, device_class_);
        return {};
    }

    void on_update(const FrameTime&) override {
        if (result_ != -1 || rendered_ < kWarmupFrames) {
            return;
        }
        // The last warm-up frame was submitted by the previous end_frame; read it back.
        device().wait_idle();
        auto rb = rhi::read_texture_rgba8(device(), target_, rhi::ResourceState::TransferSrc);
        if (!rb) {
            AE_LOG_ERROR("Golden", "readback failed: {}", rb.error().message);
            finish(1);
            return;
        }
        golden::Image actual{ rb->width, rb->height, std::move(rb->rgba8) };
        finish(evaluate(actual));
    }

    void on_render_frame(rhi::FrameInfo& frame, renderer::RenderScene& scene) override {
        rhi::CommandList& cmd = *frame.cmd;
        if (target_.is_valid()) {
            renderer().resize(UVec2(kWidth, kHeight));
            scene.view.viewport = UVec2(kWidth, kHeight);
            renderer::RenderTarget t;
            t.texture       = target_;
            t.format        = rhi::Format::RGBA8Unorm;
            t.extent        = UVec2(kWidth, kHeight);
            t.initial_state = rendered_ == 0 ? rhi::ResourceState::Undefined : rhi::ResourceState::TransferSrc;
            t.final_state   = rhi::ResourceState::TransferSrc;
            renderer().render(scene, cmd, t);
            ++rendered_;
        }
        // The swapchain image only needs to be presentable.
        cmd.barrier(frame.swapchain_image, rhi::ResourceState::Undefined, rhi::ResourceState::ColorAttachment);
        rhi::RenderingInfo clear;
        clear.render_area = frame.extent;
        clear.color       = { rhi::ColorAttachment{ frame.swapchain_image, 0, 0, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                                                    Vec4(0.0f, 0.0f, 0.0f, 1.0f) } };
        cmd.begin_rendering(clear);
        cmd.end_rendering();
    }

    void on_shutdown() override {
        if (target_.is_valid()) {
            device().wait_idle();
            device().destroy(target_);
        }
        if (result_ == -1) {
            AE_LOG_ERROR("Golden", "{}: exited before the capture frame", case_.name);
            result_ = 1;
        }
    }

private:
    void finish(int code) {
        result_ = code;
        request_exit(0);
    }

    int evaluate(const golden::Image& actual) {
        const fs::path ref_file = args_.reference_dir / device_class_ / (std::string(case_.name) + ".png");
        if (args_.update) {
            if (auto s = golden::save_png(ref_file, actual); !s) {
                AE_LOG_ERROR("Golden", "{}", s.error().message);
                return 1;
            }
            AE_LOG_INFO("Golden", "{}: reference written to {}", case_.name, ref_file.generic_string());
            return 0;
        }
        auto reference = golden::load_png(ref_file);
        if (!reference) {
            AE_LOG_WARN("Golden", "{}: skipped - no reference for device class '{}' ({}); record one with --update",
                        case_.name, device_class_, ref_file.generic_string());
            save_outputs(actual, nullptr);
            return kExitSkip;
        }
        const golden::CompareResult r = golden::compare_images(actual, *reference);
        if (r.passed) {
            AE_LOG_INFO("Golden", "{}: {}", case_.name, r.summary);
            return 0;
        }
        AE_LOG_ERROR("Golden", "{}: {}", case_.name, r.summary);
        save_outputs(actual, &*reference);
        return 1;
    }

    void save_outputs(const golden::Image& actual, const golden::Image* reference) {
        const fs::path base = args_.output_dir / device_class_;
        const fs::path act  = base / (std::string(case_.name) + ".actual.png");
        if (golden::save_png(act, actual)) {
            AE_LOG_INFO("Golden", "actual image: {}", act.generic_string());
        }
        if (reference != nullptr) {
            const golden::Image diff = golden::make_diff_image(actual, *reference);
            const fs::path      dif  = base / (std::string(case_.name) + ".diff.png");
            if (diff.valid() && golden::save_png(dif, diff)) {
                AE_LOG_INFO("Golden", "diff image: {}", dif.generic_string());
            }
        }
    }

    GoldenArgs         args_;
    const GoldenCase&  case_;
    std::string        device_class_;
    rhi::TextureHandle target_;
    u64                rendered_ = 0;
    int                result_   = -1;
};

GoldenArgs parse_args(int argc, char** argv) {
    GoldenArgs a;
    for (int i = 1; i < argc; ++i) {
        const char* s = argv[i];
        auto        value = [&](fs::path& out) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "aether-golden: %s needs a value\n", s);
                a.usage_error = true;
                return;
            }
            out = argv[++i];
        };
        if (std::strcmp(s, "--case") == 0) {
            fs::path p;
            value(p);
            a.case_name = p.string();
        } else if (std::strcmp(s, "--update") == 0) {
            a.update = true;
        } else if (std::strcmp(s, "--list") == 0) {
            a.list = true;
        } else if (std::strcmp(s, "--no-mesh-shading") == 0) {
            a.no_mesh_shading = true;
        } else if (std::strcmp(s, "--reference-dir") == 0) {
            value(a.reference_dir);
        } else if (std::strcmp(s, "--output-dir") == 0) {
            value(a.output_dir);
        }
    }
    if (a.reference_dir.empty()) a.reference_dir = paths::engine_root() / "tests/golden/reference";
    if (a.output_dir.empty()) a.output_dir = paths::executable_dir() / "golden_out";
    return a;
}

} // namespace

int main(int argc, char** argv) {
    GoldenArgs args = parse_args(argc, argv);
    if (args.usage_error) {
        return 2;
    }
    if (args.list) {
        for (const GoldenCase& c : cases()) {
            std::printf("%-22s %s%s\n", c.name, c.what, c.needs_shadows ? " [needs shadows]" : "");
        }
        return 0;
    }
    const GoldenCase* c = find_case(args.case_name);
    if (c == nullptr) {
        std::fprintf(stderr, "aether-golden: unknown or missing --case '%s' (see --list)\n", args.case_name.c_str());
        return 2;
    }

    AppDesc defaults;
    defaults.window.title     = std::string("Aether golden - ") + c->name;
    defaults.window.width     = kWidth;
    defaults.window.height    = kHeight;
    defaults.window.resizable = false;
    defaults.imgui            = false;
    defaults.vsync            = false;
    defaults.max_frames       = kWarmupFrames + 10; // safety net
    const AppDesc desc = parse_command_line(argc, argv, defaults);

    GoldenApp app(desc, std::move(args), *c);
    if (auto init = app.initialize(); !init) {
        AE_LOG_ERROR("Golden", "initialization failed: {}", init.error().message);
        return 1;
    }
    app.run();
    return app.result();
}
