// render_graph_tests.cpp — render graph compile: ordering, dead-pass culling, barrier
// sequence, usage derivation, transient aliasing/pooling (GPU-free, mock device).
#include "mock_device.h"
#include "render_graph.h"

#include <doctest/doctest.h>

using namespace aether;
using namespace aether::renderer;
using rhi::ResourceState;

namespace {
struct Fixture {
    test::MockDevice device;
    RGResourcePool   pool{ device, { rhi::SamplerHandle(900, 1), rhi::SamplerHandle(901, 1) }, 2 };
    RenderGraph      graph{ device, pool };

    RGTextureDesc color(u32 w = 64, u32 h = 64) {
        RGTextureDesc d;
        d.format = rhi::Format::RGBA16F;
        d.width = w;
        d.height = h;
        return d;
    }
};

std::vector<std::string_view> alive_names(const RenderGraph& g) {
    std::vector<std::string_view> out;
    for (const auto& p : g.pass_infos()) {
        if (!p.culled) {
            out.push_back(p.name);
        }
    }
    return out;
}
} // namespace

TEST_CASE("render graph: declaration order is execution order; unused passes are culled") {
    Fixture f;
    const rhi::TextureHandle target = f.device.make_target(64, 64);
    const RGTexture out = f.graph.import_texture(target, f.color(), ResourceState::Undefined,
                                                 ResourceState::ColorAttachment, "Target");
    const RGTexture a = f.graph.create_texture(f.color(), "A");
    const RGTexture b = f.graph.create_texture(f.color(), "B");
    const RGTexture unused = f.graph.create_texture(f.color(), "Unused");

    std::vector<std::string> executed;
    f.graph.add_pass("WriteA").write(a, ResourceState::ColorAttachment).execute([&](RGContext&) { executed.push_back("WriteA"); });
    f.graph.add_pass("Dead").write(unused, ResourceState::ShaderWrite).execute([&](RGContext&) { executed.push_back("Dead"); });
    f.graph.add_pass("AtoB").read(a).write(b, ResourceState::ShaderWrite).execute([&](RGContext&) { executed.push_back("AtoB"); });
    f.graph.add_pass("Final").read(b).write(out, ResourceState::ColorAttachment).execute([&](RGContext&) { executed.push_back("Final"); });

    f.graph.compile();
    CHECK(alive_names(f.graph) == std::vector<std::string_view>{ "WriteA", "AtoB", "Final" });

    f.graph.execute(f.device.cmd_);
    CHECK(executed == std::vector<std::string>{ "WriteA", "AtoB", "Final" });
    CHECK(f.device.cmd_.balanced());
    CHECK(f.device.state.errors.empty());
    CHECK(f.device.state.texture_state[target.value] == ResourceState::ColorAttachment);
}

TEST_CASE("render graph: culling is transitive and respects side effects") {
    Fixture f;
    const RGTexture t1 = f.graph.create_texture(f.color(), "T1");
    const RGTexture t2 = f.graph.create_texture(f.color(), "T2");
    const RGTexture t3 = f.graph.create_texture(f.color(), "T3");
    f.graph.add_pass("P1").write(t1, ResourceState::ShaderWrite);
    f.graph.add_pass("P2").read(t1).write(t2, ResourceState::ShaderWrite); // T2 never read
    f.graph.add_pass("Readback").write(t3, ResourceState::ShaderWrite).side_effect();
    f.graph.compile();
    const auto infos = f.graph.pass_infos();
    CHECK(infos[0].culled);
    CHECK(infos[1].culled);
    CHECK_FALSE(infos[2].culled);

    // With culling disabled everything runs.
    f.graph.reset();
    f.graph.set_culling_enabled(false);
    const RGTexture u = f.graph.create_texture(f.color(), "U");
    f.graph.add_pass("Orphan").write(u, ResourceState::ShaderWrite);
    f.graph.compile();
    CHECK_FALSE(f.graph.pass_infos()[0].culled);
}

TEST_CASE("render graph: partial writes (read-modify-write) keep earlier writers alive") {
    Fixture f;
    const rhi::TextureHandle target = f.device.make_target(64, 64);
    const RGTexture out = f.graph.import_texture(target, f.color(), ResourceState::Undefined,
                                                 ResourceState::ColorAttachment, "Target");
    RGTextureDesc sd;
    sd.type = rhi::TextureType::Tex2DArray;
    sd.format = rhi::Format::D32F;
    sd.width = sd.height = 32;
    sd.array_layers = 2;
    const RGTexture shadow = f.graph.create_texture(sd, "Shadow");
    f.graph.add_pass("Layer0").write(shadow, ResourceState::DepthStencilAttachment);
    f.graph.add_pass("Layer1").write(shadow, ResourceState::DepthStencilAttachment);
    f.graph.add_pass("Use").read(shadow).write(out, ResourceState::ColorAttachment);
    f.graph.compile();
    CHECK(alive_names(f.graph).size() == 3);
}

TEST_CASE("render graph: barrier sequence follows per-resource state") {
    Fixture f;
    const rhi::TextureHandle target = f.device.make_target(64, 64);
    const RGTexture out = f.graph.import_texture(target, f.color(), ResourceState::Undefined,
                                                 ResourceState::ShaderRead, "Target");
    RGTextureDesc dd = f.color();
    dd.format = rhi::Format::D32F;
    const RGTexture depth = f.graph.create_texture(dd, "Depth");
    const RGTexture hdr = f.graph.create_texture(f.color(), "HDR");

    f.graph.add_pass("Prepass").write(depth, ResourceState::DepthStencilAttachment);
    f.graph.add_pass("Forward").write(hdr, ResourceState::ColorAttachment).read(depth, ResourceState::DepthStencilAttachment);
    f.graph.add_pass("Sky").write(hdr, ResourceState::ColorAttachment).read(depth, ResourceState::DepthStencilAttachment);
    f.graph.add_pass("Post").read(hdr).read(depth).write(out, ResourceState::ColorAttachment);
    f.graph.compile();

    const auto p = f.graph.pass_infos();
    REQUIRE(p.size() == 4);
    // Prepass: fresh depth Undefined -> DSA.
    REQUIRE(p[0].barriers.size() == 1);
    CHECK(p[0].barriers[0].from == ResourceState::Undefined);
    CHECK(p[0].barriers[0].to == ResourceState::DepthStencilAttachment);
    // Forward: HDR Undefined -> CA, depth DSA -> DSA (read after write hazard).
    REQUIRE(p[1].barriers.size() == 2);
    CHECK(p[1].barriers[0].to == ResourceState::ColorAttachment);
    CHECK(p[1].barriers[1].from == ResourceState::DepthStencilAttachment);
    CHECK(p[1].barriers[1].to == ResourceState::DepthStencilAttachment);
    // Sky: HDR CA -> CA (write after write), depth read after read: none.
    REQUIRE(p[2].barriers.size() == 1);
    CHECK(p[2].barriers[0].from == ResourceState::ColorAttachment);
    CHECK(p[2].barriers[0].to == ResourceState::ColorAttachment);
    // Post: HDR -> ShaderRead, depth DSA -> ShaderRead, target Undefined -> CA.
    REQUIRE(p[3].barriers.size() == 3);
    CHECK(p[3].barriers[0].to == ResourceState::ShaderRead);
    CHECK(p[3].barriers[1].from == ResourceState::DepthStencilAttachment);
    CHECK(p[3].barriers[2].to == ResourceState::ColorAttachment);
    // Imported target ends in its final state.
    REQUIRE(f.graph.final_barriers().size() == 1);
    CHECK(f.graph.final_barriers()[0].from == ResourceState::ColorAttachment);
    CHECK(f.graph.final_barriers()[0].to == ResourceState::ShaderRead);

    f.graph.execute(f.device.cmd_);
    CHECK(f.device.state.errors.empty());
    CHECK(f.device.state.texture_state[target.value] == ResourceState::ShaderRead);
}

TEST_CASE("render graph: usage is derived from accesses") {
    Fixture f;
    const rhi::TextureHandle target = f.device.make_target(64, 64);
    const RGTexture out = f.graph.import_texture(target, f.color(), ResourceState::Undefined,
                                                 ResourceState::ColorAttachment, "Target");
    const RGTexture t = f.graph.create_texture(f.color(), "T");
    f.graph.add_pass("Raster").write(t, ResourceState::ColorAttachment);
    f.graph.add_pass("Compute").read(t).write(out, ResourceState::ColorAttachment);
    f.graph.compile();
    const rhi::TextureUsage u = f.graph.derived_usage(t);
    CHECK(any(u & rhi::TextureUsage::ColorAttach));
    CHECK(any(u & rhi::TextureUsage::Sampled));
    CHECK_FALSE(any(u & rhi::TextureUsage::Storage));
    const rhi::TextureDesc* desc = f.device.texture_desc(f.graph.physical_texture(t));
    REQUIRE(desc != nullptr);
    CHECK(desc->usage == u);
}

TEST_CASE("render graph: equal-desc transients with disjoint lifetimes alias one texture") {
    Fixture f;
    const rhi::TextureHandle target = f.device.make_target(64, 64);
    const RGTexture out = f.graph.import_texture(target, f.color(), ResourceState::Undefined,
                                                 ResourceState::ColorAttachment, "Target");
    const RGTexture a = f.graph.create_texture(f.color(), "A");
    const RGTexture b = f.graph.create_texture(f.color(), "B");
    const RGTexture c = f.graph.create_texture(f.color(), "C");
    f.graph.add_pass("WriteA").write(a, ResourceState::ShaderWrite);
    f.graph.add_pass("AtoB").read(a).write(b, ResourceState::ShaderWrite);
    f.graph.add_pass("BtoC").read(b).write(c, ResourceState::ShaderWrite); // A is dead here
    f.graph.add_pass("Out").read(c).write(out, ResourceState::ColorAttachment);
    f.graph.compile();
    CHECK(f.graph.physical_texture(a) == f.graph.physical_texture(c));
    CHECK(f.graph.physical_texture(a) != f.graph.physical_texture(b));
    // The aliased reuse transitions from the previous user's state, not from Undefined.
    const auto p = f.graph.pass_infos();
    bool found = false;
    for (const RGBarrier& bar : p[2].barriers) {
        if (bar.texture == f.graph.physical_texture(c)) {
            CHECK(bar.from == ResourceState::ShaderRead);
            CHECK(bar.to == ResourceState::ShaderWrite);
            found = true;
        }
    }
    CHECK(found);
    f.graph.execute(f.device.cmd_);
    CHECK(f.device.state.errors.empty());
}

TEST_CASE("render graph: pool reuses across frames and releases after N unused frames") {
    Fixture f; // max_unused_frames = 2
    const rhi::TextureHandle target = f.device.make_target(64, 64);
    rhi::TextureHandle first{};
    for (int frame = 0; frame < 3; ++frame) {
        f.graph.reset();
        const RGTexture out = f.graph.import_texture(target, f.color(), ResourceState::Undefined,
                                                     ResourceState::ColorAttachment, "Target");
        const RGTexture t = f.graph.create_texture(f.color(), "T");
        f.graph.add_pass("W").write(t, ResourceState::ShaderWrite);
        f.graph.add_pass("R").read(t).write(out, ResourceState::ColorAttachment);
        f.graph.execute(f.device.cmd_);
        if (frame == 0) {
            first = f.graph.physical_texture(t);
        } else {
            CHECK(f.graph.physical_texture(t) == first);
        }
        f.pool.end_frame();
    }
    CHECK(f.device.state.textures_created == 2); // target + one pooled transient
    CHECK(f.pool.live_texture_count() == 1);
    CHECK(f.device.state.errors.empty());

    // Different desc (resize) -> new texture; the old one is released after 2 idle frames.
    for (int frame = 0; frame < 4; ++frame) {
        f.graph.reset();
        const RGTexture out = f.graph.import_texture(target, f.color(), ResourceState::Undefined,
                                                     ResourceState::ColorAttachment, "Target");
        const RGTexture t = f.graph.create_texture(f.color(128, 128), "T");
        f.graph.add_pass("W").write(t, ResourceState::ShaderWrite);
        f.graph.add_pass("R").read(t).write(out, ResourceState::ColorAttachment);
        f.graph.execute(f.device.cmd_);
        f.pool.end_frame();
    }
    CHECK(f.pool.live_texture_count() == 1);
    CHECK(f.device.state.textures_destroyed == 1);
    CHECK(f.device.state.errors.empty());
}

TEST_CASE("render graph: transient buffers get addresses and barriers") {
    Fixture f;
    const rhi::TextureHandle target = f.device.make_target(64, 64);
    const RGTexture out = f.graph.import_texture(target, f.color(), ResourceState::Undefined,
                                                 ResourceState::ColorAttachment, "Target");
    RGBufferDesc bd;
    bd.size = 1024;
    const RGBuffer buf = f.graph.create_buffer(bd, "Grid");
    u64 addr = 0;
    f.graph.add_pass("Cull").write(buf, ResourceState::ShaderWrite).execute([&](RGContext& ctx) { addr = ctx.address(buf); });
    f.graph.add_pass("Shade").read(buf).write(out, ResourceState::ColorAttachment);
    f.graph.execute(f.device.cmd_);
    CHECK(addr != 0);
    const auto p = f.graph.pass_infos();
    REQUIRE(p[1].barriers.size() == 2);
    CHECK_FALSE(p[1].barriers[0].is_texture);
    CHECK(p[1].barriers[0].from == ResourceState::ShaderWrite);
    CHECK(p[1].barriers[0].to == ResourceState::ShaderRead);
}
