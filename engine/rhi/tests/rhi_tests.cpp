// rhi_tests.cpp — GPU-free tests: shader compiler, handle pool, ResourceState table.
#include <doctest/doctest.h>

#include "aether/core/paths.h"
#include "aether/core/paths_ext.h"
#include "aether/rhi/shader_compiler.h"
#include "resource_pool.h"
#include "vk_convert.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace aether;
using namespace aether::rhi;
namespace fs = std::filesystem;

namespace {

constexpr const char* kVertex = R"(#version 460
layout(location = 0) out vec3 v_color;
void main() {
    vec2 p[3] = vec2[](vec2(0.0, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
    v_color = vec3(1.0);
}
)";

void write_file(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

fs::path test_dir() {
    return paths::cache_dir() / "rhi_tests";
}

} // namespace

TEST_CASE("compile_glsl produces SPIR-V") {
    auto r = compile_glsl(ShaderStage::Vertex, kVertex, "tri.vert");
    REQUIRE_MESSAGE(r.has_value(), (r ? "" : r.error().message));
    REQUIRE(r->size() > 5);
    CHECK((*r)[0] == 0x07230203u);           // SPIR-V magic
    CHECK(((*r)[1] >> 16) == 1);             // major version 1
    CHECK((((*r)[1] >> 8) & 0xFF) == 6);     // SPIR-V 1.6
}

TEST_CASE("compile errors carry <file>:<line>") {
    const char* src = "#version 460\nvoid main() {\n    undeclared_thing = 1;\n}\n";
    auto        r   = compile_glsl(ShaderStage::Fragment, src, "broken.frag");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ErrorCode::CompilationFailed);
    CHECK_MESSAGE(r.error().message.find("broken.frag:3:") != std::string::npos, r.error().message);
    CHECK(r.error().message.find("undeclared_thing") != std::string::npos);

    auto bad_stage = compile_glsl(ShaderStage::AllGraphics, kVertex, "x");
    CHECK_FALSE(bad_stage.has_value());
}

TEST_CASE("defines are injected after #version") {
    const char* src = "#version 460\n"
                      "#if !defined(AE_FLAG) || AE_VALUE != 7\n#error defines missing\n#endif\n"
                      "void main() { gl_Position = vec4(float(AE_VALUE)); }\n";
    ShaderCompileOptions opt;
    opt.defines = { { "AE_FLAG", "" }, { "AE_VALUE", "7" } };
    auto ok = compile_glsl(ShaderStage::Vertex, src, "defines.vert", opt);
    CHECK_MESSAGE(ok.has_value(), (ok ? "" : ok.error().message));
    auto missing = compile_glsl(ShaderStage::Vertex, src, "defines.vert");
    REQUIRE_FALSE(missing.has_value());
    // User line numbers are preserved despite the injected preamble (#error is on line 3).
    CHECK_MESSAGE(missing.error().message.find("defines.vert:3:") != std::string::npos, missing.error().message);
}

TEST_CASE("#include resolution: includer dir, include_dirs, shader_dir; errors name the header") {
    const fs::path dir = test_dir();
    write_file(dir / "sub" / "local.glsl", "#include \"nested.glsl\"\nfloat local_value() { return nested_value(); }\n");
    write_file(dir / "sub" / "nested.glsl", "float nested_value() { return 2.0; }\n");
    write_file(dir / "extra" / "extra.glsl", "float extra_value() { return 3.0; }\n");
    write_file(dir / "sub" / "bad.glsl", "float bad() {\n  return nope;\n}\n");
    write_file(dir / "sub" / "main.frag", "#version 460\n"
                                          "#include \"local.glsl\"\n"      // includer's directory
                                          "#include \"extra.glsl\"\n"      // options.include_dirs
                                          "#include \"common/bindless.glsl\"\n" // paths::shader_dir()
                                          "layout(location = 0) out vec4 o;\n"
                                          "void main() { o = vec4(local_value() + extra_value()); }\n");
    ShaderCompileOptions opt;
    opt.include_dirs = { paths::to_utf8(dir / "extra") };
    auto r = compile_glsl_file(ShaderStage::Fragment, dir / "sub" / "main.frag", opt);
    CHECK_MESSAGE(r.has_value(), (r ? "" : r.error().message));

    write_file(dir / "sub" / "uses_bad.frag", "#version 460\n#include \"bad.glsl\"\nvoid main() {}\n");
    auto bad = compile_glsl_file(ShaderStage::Fragment, dir / "sub" / "uses_bad.frag");
    REQUIRE_FALSE(bad.has_value());
    CHECK_MESSAGE(bad.error().message.find("bad.glsl:2:") != std::string::npos, bad.error().message);

    write_file(dir / "sub" / "missing.frag", "#version 460\n#include \"does_not_exist.glsl\"\nvoid main() {}\n");
    auto missing = compile_glsl_file(ShaderStage::Fragment, dir / "sub" / "missing.frag");
    REQUIRE_FALSE(missing.has_value());
    CHECK_MESSAGE(missing.error().message.find("does_not_exist.glsl") != std::string::npos, missing.error().message);

    auto nofile = compile_glsl_file(ShaderStage::Fragment, dir / "nope.frag");
    REQUIRE_FALSE(nofile.has_value());
    CHECK(nofile.error().code == ErrorCode::NotFound);
    fs::remove_all(dir);
}

TEST_CASE("the frozen bindless.glsl compiles for every stage that uses it") {
    const std::string body = "#version 460\n#include \"common/bindless.glsl\"\n";
    auto frag = compile_glsl(ShaderStage::Fragment,
                             body + "layout(location=0) in vec2 uv; layout(location=0) out vec4 o;\n"
                                    "layout(push_constant) uniform P { uint tex; } pc;\n"
                                    "void main() { o = ae_sample_or(pc.tex, uv, vec4(1)); }\n",
                             "bindless_test.frag");
    CHECK_MESSAGE(frag.has_value(), (frag ? "" : frag.error().message));
    auto comp = compile_glsl(ShaderStage::Compute,
                             body + "layout(local_size_x=8, local_size_y=8) in;\n"
                                    "layout(push_constant) uniform P { uint img; } pc;\n"
                                    "void main() { imageStore(ae_img2d_rgba8[nonuniformEXT(pc.img)], ivec2(gl_GlobalInvocationID.xy), vec4(1)); }\n",
                             "bindless_test.comp");
    CHECK_MESSAGE(comp.has_value(), (comp ? "" : comp.error().message));
}

TEST_CASE("shader_stage_from_path") {
    CHECK(*shader_stage_from_path("a/b/tri.vert") == ShaderStage::Vertex);
    CHECK(*shader_stage_from_path("tri.frag") == ShaderStage::Fragment);
    CHECK(*shader_stage_from_path("x.comp") == ShaderStage::Compute);
    CHECK(*shader_stage_from_path("x.geom") == ShaderStage::Geometry);
    CHECK(*shader_stage_from_path("x.task") == ShaderStage::Task);
    CHECK(*shader_stage_from_path("x.mesh") == ShaderStage::Mesh);
    CHECK(*shader_stage_from_path("x.frag.glsl") == ShaderStage::Fragment);
    CHECK_FALSE(shader_stage_from_path("common.glsl").has_value());
    CHECK_FALSE(shader_stage_from_path("x.txt").has_value());
}

TEST_CASE("ResourcePool: generations reject stale handles") {
    struct Tag;
    vk::ResourcePool<Tag, int> pool;
    auto                        a = pool.allocate(10);
    auto                        b = pool.allocate(20);
    REQUIRE(a.is_valid());
    CHECK(a.generation() != 0);
    CHECK(*pool.get(a) == 10);
    CHECK(*pool.get(b) == 20);
    CHECK(pool.live_count() == 2);

    int out = 0;
    CHECK(pool.release(a, out));
    CHECK(out == 10);
    CHECK(pool.get(a) == nullptr);           // stale
    CHECK_FALSE(pool.release(a, out));       // double free rejected
    auto c = pool.allocate(30);              // reuses a's slot with a new generation
    CHECK(c.index() == a.index());
    CHECK(c.generation() != a.generation());
    CHECK(pool.get(a) == nullptr);
    CHECK(*pool.get(c) == 30);
    CHECK(pool.get(Handle<Tag>{}) == nullptr);
    CHECK(pool.get(Handle<Tag>(0, 0)) == nullptr); // zeroed handle never validates

    // Generation wraps (8 bits) but skips 0.
    for (int i = 0; i < 600; ++i) {
        pool.release(c, out);
        c = pool.allocate(int{ i });
        CHECK(c.generation() != 0);
    }
    // Chunk growth keeps earlier addresses stable.
    int* stable = pool.get(b);
    for (int i = 0; i < 5000; ++i) {
        (void)pool.allocate(int{ i });
    }
    CHECK(pool.get(b) == stable);
}

TEST_CASE("ResourceState -> sync2 mapping") {
    using vk::texture_state;
    using vk::buffer_state;
    CHECK(texture_state(ResourceState::Undefined, true).layout == VK_IMAGE_LAYOUT_UNDEFINED);
    CHECK(texture_state(ResourceState::Undefined, true).stages == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    CHECK(texture_state(ResourceState::ColorAttachment, false).layout == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL);
    CHECK(texture_state(ResourceState::DepthStencilAttachment, false).layout == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL);
    CHECK(texture_state(ResourceState::ShaderRead, false).layout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
    CHECK(texture_state(ResourceState::ShaderWrite, false).layout == VK_IMAGE_LAYOUT_GENERAL);
    CHECK(texture_state(ResourceState::TransferDst, false).layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CHECK(texture_state(ResourceState::TransferSrc, false).layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK(texture_state(ResourceState::Present, false).layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // As a source only writes are made available; reads are never in srcAccessMask.
    CHECK(texture_state(ResourceState::ShaderRead, true).access == VK_ACCESS_2_NONE);
    CHECK(texture_state(ResourceState::ColorAttachment, true).access == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    CHECK((texture_state(ResourceState::ColorAttachment, false).access & VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT) != 0);
    CHECK(buffer_state(ResourceState::IndirectArgument, false).stages == VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);
    CHECK(buffer_state(ResourceState::IndirectArgument, false).access == VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    CHECK((buffer_state(ResourceState::ShaderRead, false).stages & VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT) != 0);

    // Barriers always cover every mip and layer.
    const auto b = vk::make_image_barrier(VK_NULL_HANDLE, VK_IMAGE_ASPECT_COLOR_BIT, ResourceState::Undefined,
                                          ResourceState::ColorAttachment);
    CHECK(b.subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS);
    CHECK(b.subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS);
    CHECK(b.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
    CHECK(b.newLayout == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL);

    CHECK(vk::is_write_state(ResourceState::ShaderWrite));
    CHECK_FALSE(vk::is_write_state(ResourceState::ShaderRead));
    CHECK(vk::subresource_bytes(Format::RGBA8Unorm, 4, 4, 1) == 64);
    CHECK(vk::subresource_bytes(Format::BC1Srgb, 5, 5, 1) == 4 * 8); // 2x2 blocks
    CHECK(vk::to_vk(Format::BGRA8Unorm) == VK_FORMAT_B8G8R8A8_UNORM);
    CHECK(vk::from_vk(VK_FORMAT_D32_SFLOAT) == Format::D32F);
}

TEST_CASE("ADR-0009: BC7Unorm mapping and BCn sizes") {
    CHECK(vk::to_vk(Format::BC7Unorm) == VK_FORMAT_BC7_UNORM_BLOCK);
    CHECK(vk::from_vk(VK_FORMAT_BC7_UNORM_BLOCK) == Format::BC7Unorm);
    CHECK(vk::from_vk(VK_FORMAT_BC7_SRGB_BLOCK) == Format::BC7Srgb);
    CHECK(vk::is_block_compressed(Format::BC7Unorm));
    CHECK_FALSE(vk::is_srgb(Format::BC7Unorm));
    CHECK(vk::subresource_bytes(Format::BC7Unorm, 16, 16, 1) == 16 * 16);
    CHECK(vk::subresource_bytes(Format::BC5Unorm, 2, 2, 1) == 16); // tail mip: one block
}

TEST_CASE("ADR-0009: VertexAttribute::binding selects the source binding") {
    GraphicsPipelineDesc d;
    d.vertex_bindings = { VertexBinding{ 0, 48, false }, VertexBinding{ 1, 16, true } };
    bool declared = false;
    CHECK(vk::vertex_attribute_binding(d, VertexAttribute{ 0, 0, Format::RGB32F }, &declared) == 0);
    CHECK(declared);
    CHECK(vk::vertex_attribute_binding(d, VertexAttribute{ 4, 0, Format::RGBA32F, 1 }, &declared) == 1);
    CHECK(declared);
    CHECK(vk::vertex_attribute_binding(d, VertexAttribute{ 5, 0, Format::RGBA32F, 7 }, &declared) == 0);
    CHECK_FALSE(declared); // undeclared -> first declared binding

    // Pre-M2 layouts: one binding numbered != 0 and attributes that never set `binding`.
    GraphicsPipelineDesc legacy;
    legacy.vertex_bindings = { VertexBinding{ 3, 24, false } };
    CHECK(vk::vertex_attribute_binding(legacy, VertexAttribute{ 0, 0, Format::RGB32F }) == 3);
}
