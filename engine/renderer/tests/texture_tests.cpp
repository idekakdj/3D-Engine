// texture_tests.cpp — TextureUpload::mip_levels (pre-built chains) and BC1/BC3/BC5/BC7 uploads
// (ADR-0009): payload size math, subresource order (layer-major within each mip) and the
// validation rules, checked against the validating mock device.
#include "format_utils.h"
#include "mock_device.h"

#include "aether/renderer/renderer.h"

#include <doctest/doctest.h>

#include <vector>

using namespace aether;
using namespace aether::renderer;

TEST_CASE("texture payload sizes: BCn blocks, partial blocks, mip chains, layers") {
    CHECK(is_block_compressed(rhi::Format::BC7Unorm));
    CHECK(is_block_compressed(rhi::Format::BC1Srgb));
    CHECK_FALSE(is_block_compressed(rhi::Format::RGBA8Srgb));
    CHECK(bytes_per_block(rhi::Format::BC1Srgb) == 8);
    CHECK(bytes_per_block(rhi::Format::BC3Srgb) == 16);
    CHECK(bytes_per_block(rhi::Format::BC5Unorm) == 16);
    CHECK(bytes_per_block(rhi::Format::BC7Srgb) == 16);

    CHECK(subresource_size(rhi::Format::BC7Unorm, 16, 16) == 16 * 16);  // 4x4 blocks x 16 B
    CHECK(subresource_size(rhi::Format::BC1Srgb, 16, 16) == 16 * 8);
    CHECK(subresource_size(rhi::Format::BC7Unorm, 10, 6) == 3 * 2 * 16); // partial blocks round up
    CHECK(subresource_size(rhi::Format::BC7Unorm, 1, 1) == 16);           // tail mips: one block
    CHECK(subresource_size(rhi::Format::RGBA8Unorm, 10, 6) == 10 * 6 * 4);

    // 16x16 BC7, full chain (16, 8, 4, 2, 1): 16 + 4 + 1 + 1 + 1 blocks.
    CHECK(full_mip_count(16, 16) == 5);
    CHECK(texture_upload_size(rhi::Format::BC7Unorm, 16, 16, 1, 5) == (16 + 4 + 1 + 1 + 1) * 16);
    // Non-square, 6 layers (cubemap), 2 mips of RGBA16F.
    CHECK(texture_upload_size(rhi::Format::RGBA16F, 8, 4, 6, 2) == (8 * 4 + 4 * 2) * 8 * 6);
}

TEST_CASE("register_texture: pre-built mip chains and BCn go through update_texture_mip in order") {
    test::MockDevice device;
    auto created = Renderer::create(RendererDesc{ &device, UVec2(64, 64) });
    REQUIRE(created);
    std::unique_ptr<Renderer> r = std::move(created.value());

    SUBCASE("BC7 chain: every mip uploaded, texture created with the supplied mip count") {
        std::vector<byte> px(static_cast<usize>(texture_upload_size(rhi::Format::BC7Unorm, 16, 16, 1, 3)), byte{ 1 });
        TextureUpload tu;
        tu.format = rhi::Format::BC7Unorm;
        tu.width = 16;
        tu.height = 16;
        tu.mip_levels = 3;
        tu.generate_mips = true; // ignored for BCn / pre-built chains
        tu.pixels = px;
        device.state.mip_uploads.clear();
        CHECK(r->register_texture(tu).is_valid());
        REQUIRE(device.state.mip_uploads.size() == 3);
        CHECK(device.state.mip_uploads[0].mip == 0);
        CHECK(device.state.mip_uploads[0].bytes == 256);
        CHECK(device.state.mip_uploads[1].mip == 1);
        CHECK(device.state.mip_uploads[1].bytes == 64);
        CHECK(device.state.mip_uploads[2].mip == 2);
        CHECK(device.state.mip_uploads[2].bytes == 16);
        rhi::TextureHandle th;
        th.value = device.state.mip_uploads[0].texture;
        const rhi::TextureDesc* d = device.texture_desc(th);
        REQUIRE(d != nullptr);
        CHECK(d->mip_levels == 3);
        CHECK(d->format == rhi::Format::BC7Unorm);
    }
    SUBCASE("every BC format is accepted with a single mip (no generation)") {
        for (rhi::Format f : { rhi::Format::BC1Srgb, rhi::Format::BC3Srgb, rhi::Format::BC5Unorm,
                               rhi::Format::BC7Srgb, rhi::Format::BC7Unorm }) {
            std::vector<byte> px(static_cast<usize>(subresource_size(f, 8, 8)), byte{ 2 });
            TextureUpload     tu;
            tu.format = f;
            tu.width = 8;
            tu.height = 8;
            tu.pixels = px;
            const TextureHandle h = r->register_texture(tu);
            CHECK(h.is_valid());
        }
    }
    SUBCASE("cubemap chain is layer-major within each mip") {
        std::vector<byte> px(static_cast<usize>(texture_upload_size(rhi::Format::RGBA8Unorm, 4, 4, 6, 2)), byte{ 3 });
        TextureUpload tu;
        tu.format = rhi::Format::RGBA8Unorm;
        tu.width = 4;
        tu.height = 4;
        tu.array_layers = 6;
        tu.cubemap = true;
        tu.mip_levels = 2;
        tu.pixels = px;
        device.state.mip_uploads.clear();
        CHECK(r->register_texture(tu).is_valid());
        REQUIRE(device.state.mip_uploads.size() == 12);
        for (u32 i = 0; i < 12; ++i) {
            CHECK(device.state.mip_uploads[i].mip == i / 6);
            CHECK(device.state.mip_uploads[i].layer == i % 6);
            CHECK(device.state.mip_uploads[i].bytes == (i < 6 ? 64u : 16u));
        }
    }
    SUBCASE("invalid uploads are rejected") {
        std::vector<byte> px(4096, byte{ 0 });
        TextureUpload     tu;
        tu.format = rhi::Format::BC7Srgb;
        tu.width = 16;
        tu.height = 16;
        tu.mip_levels = 2;
        tu.pixels = ByteSpan(px.data(), 256); // mip 1 missing
        CHECK_FALSE(r->register_texture(tu).is_valid());
        tu.mip_levels = 6; // a 16x16 chain has 5 mips
        tu.pixels = ByteSpan(px.data(), static_cast<usize>(texture_upload_size(tu.format, 16, 16, 1, 6)));
        CHECK_FALSE(r->register_texture(tu).is_valid());
        device.mutable_features().texture_compression_bc = false;
        tu.mip_levels = 1;
        tu.pixels = ByteSpan(px.data(), 256);
        CHECK_FALSE(r->register_texture(tu).is_valid());
        device.mutable_features().texture_compression_bc = true;
        CHECK(r->register_texture(tu).is_valid());
    }
    CHECK(device.state.errors.empty());
    for (const std::string& e : device.state.errors) {
        MESSAGE(e);
    }
}
