// format_tests.cpp — cooked .aeasset round trips, checksums, corruption detection.
#include "aether/assets/format.h"
#include "aether/assets/importers.h"

#include "test_helpers.h"

#include <doctest/doctest.h>

#include <cstring>

namespace fs = std::filesystem;
using namespace aether;
using namespace aether::assets;
using namespace aether::assets::test;

namespace {

bool eq(const AABB& a, const AABB& b) { return a.min == b.min && a.max == b.max; }
bool eq(const Transform& a, const Transform& b) {
    return a.position == b.position && a.rotation == b.rotation && a.scale == b.scale;
}
bool eq(const MeshData& a, const MeshData& b) {
    if (a.vertices.size() != b.vertices.size() || a.skin.size() != b.skin.size()) return false;
    if (std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(Vertex)) != 0) return false;
    for (usize i = 0; i < a.skin.size(); ++i) {
        if (std::memcmp(a.skin[i].joints, b.skin[i].joints, sizeof(a.skin[i].joints)) != 0 ||
            a.skin[i].weights != b.skin[i].weights)
            return false;
    }
    if (a.indices != b.indices || a.submeshes.size() != b.submeshes.size()) return false;
    for (usize i = 0; i < a.submeshes.size(); ++i) {
        const Submesh &x = a.submeshes[i], &y = b.submeshes[i];
        if (x.first_index != y.first_index || x.index_count != y.index_count || x.material_slot != y.material_slot ||
            !eq(x.bounds, y.bounds))
            return false;
    }
    return eq(a.bounds, b.bounds) && a.skeleton == b.skeleton;
}
bool eq(const TextureData& a, const TextureData& b) {
    return a.width == b.width && a.height == b.height && a.mip_levels == b.mip_levels &&
           a.array_layers == b.array_layers && a.is_cubemap == b.is_cubemap && a.format == b.format &&
           a.pixels == b.pixels;
}
bool eq(const MaterialData& a, const MaterialData& b) {
    return a.name == b.name && a.base_color_factor == b.base_color_factor && a.emissive_factor == b.emissive_factor &&
           a.metallic_factor == b.metallic_factor && a.roughness_factor == b.roughness_factor &&
           a.normal_scale == b.normal_scale && a.occlusion_strength == b.occlusion_strength &&
           a.alpha_cutoff == b.alpha_cutoff && a.alpha_mode == b.alpha_mode && a.double_sided == b.double_sided &&
           a.base_color_texture == b.base_color_texture &&
           a.metallic_roughness_texture == b.metallic_roughness_texture && a.normal_texture == b.normal_texture &&
           a.occlusion_texture == b.occlusion_texture && a.emissive_texture == b.emissive_texture;
}
bool eq(const SkeletonData& a, const SkeletonData& b) {
    if (a.joint_names != b.joint_names || a.parents != b.parents || a.inverse_bind != b.inverse_bind) return false;
    for (usize i = 0; i < a.bind_local.size(); ++i) {
        if (!eq(a.bind_local[i], b.bind_local[i])) return false;
    }
    return a.bind_local.size() == b.bind_local.size();
}
bool eq(const AnimationClipData& a, const AnimationClipData& b) {
    if (a.name != b.name || a.duration != b.duration || a.skeleton != b.skeleton || a.channels.size() != b.channels.size())
        return false;
    for (usize i = 0; i < a.channels.size(); ++i) {
        const AnimationChannel &x = a.channels[i], &y = b.channels[i];
        if (x.joint != y.joint || x.path != y.path || x.interpolation != y.interpolation || x.times != y.times ||
            x.values != y.values)
            return false;
    }
    return true;
}
bool eq(const SceneData& a, const SceneData& b) {
    if (a.name != b.name || a.nodes.size() != b.nodes.size()) return false;
    for (usize i = 0; i < a.nodes.size(); ++i) {
        const SceneNodeData &x = a.nodes[i], &y = b.nodes[i];
        if (x.name != y.name || x.parent != y.parent || !eq(x.local, y.local) || x.mesh != y.mesh ||
            x.materials != y.materials || x.skeleton != y.skeleton)
            return false;
    }
    return true;
}

template <typename T>
void check_round_trip(const T& data, const char* what) {
    CAPTURE(what);
    const CookedMeta meta{ AssetId{ 0x1122334455667788ull, 0x99aabbccddeeff00ull }, 0xfeedfacecafebeefull, 7 };
    auto             bytes = encode_asset(meta, data);
    REQUIRE_MESSAGE(bytes.has_value(), bytes.error().message);
    CookedHeader header;
    auto         back = decode_asset<T>(as_bytes_view(*bytes), &header);
    REQUIRE_MESSAGE(back.has_value(), back.error().message);
    CHECK(eq(data, *back));
    CHECK(header.type == asset_type_of_v<T>);
    CHECK(header.id == meta.id);
    CHECK(header.source_hash == meta.source_hash);
    CHECK(header.importer_version == 7u);
    CHECK(header.format_version == kCookedFormatVersion);
    CHECK(header.payload_bytes == bytes->size() - kCookedHeaderSize);
    // Deterministic encoding.
    CHECK(*encode_asset(meta, *back) == *bytes);
}

ImportResult import_sample(const char* rel) {
    ImportSettings s;
    s.content_root = content_dir();
    auto r = import_gltf(sample(rel), s);
    REQUIRE_MESSAGE(r.has_value(), r.error().message);
    return std::move(*r);
}

} // namespace

TEST_CASE("format: checksum reference values") {
    const char* text = "123456789";
    CHECK(crc32(ByteSpan(reinterpret_cast<const byte*>(text), 9)) == 0xCBF43926u);
    CHECK(crc32(ByteSpan{}) == 0u);
    // Incremental CRC equals one-shot CRC.
    const u32 part = crc32(ByteSpan(reinterpret_cast<const byte*>(text), 4));
    CHECK(crc32(ByteSpan(reinterpret_cast<const byte*>(text) + 4, 5), part) == 0xCBF43926u);
    CHECK(assets::fnv1a64(StringView("")) == 0xcbf29ce484222325ull);
    CHECK(assets::fnv1a64(StringView("a")) == 0xaf63dc4c8601ec8cull);
}

TEST_CASE("format: round trip of every asset type") {
    const ImportResult cube = import_sample("cube/cube.gltf");
    const ImportResult skinned = import_sample("skinned/skinned.gltf");

    check_round_trip(cube.meshes[0].data, "static mesh");
    check_round_trip(skinned.meshes[0].data, "skinned mesh");
    check_round_trip(cube.textures[0].data, "texture rgba8");
    check_round_trip(cube.materials[0].data, "material");
    check_round_trip(skinned.skeletons[0].data, "skeleton");
    check_round_trip(skinned.animations[0].data, "animation clip");
    check_round_trip(skinned.scenes[0].data, "scene");

    // Hand-built values exercising the remaining fields.
    TextureData cube_map;
    cube_map.width = 4;
    cube_map.height = 4;
    cube_map.mip_levels = 3; // 4x4, 2x2, 1x1
    cube_map.array_layers = 6;
    cube_map.is_cubemap = true;
    cube_map.format = TextureFormat::RGBA16F;
    cube_map.pixels.resize(texture_byte_size(cube_map));
    CHECK(cube_map.pixels.size() == (16 + 4 + 1) * 6 * 8);
    for (usize i = 0; i < cube_map.pixels.size(); ++i) cube_map.pixels[i] = static_cast<u8>(i * 31);
    check_round_trip(cube_map, "cubemap with mips");

    MaterialData mat;
    mat.name = "Glass \xE2\x9C\x93"; // UTF-8
    mat.alpha_mode = AlphaMode::Blend;
    mat.double_sided = true;
    mat.emissive_factor = Vec3(1.0f, 2.0f, 3.0f);
    mat.emissive_texture = AssetId{ 1, 2 };
    mat.occlusion_texture = AssetId{ 3, 4 };
    check_round_trip(mat, "material fields");

    check_round_trip(MeshData{}, "empty mesh");
    check_round_trip(SceneData{}, "empty scene");
}

TEST_CASE("format: corruption and misuse are detected") {
    const ImportResult cube = import_sample("cube/cube.gltf");
    const CookedMeta   meta{ cube.meshes[0].id, 42, kImporterVersion };
    auto               encoded = encode_asset(meta, cube.meshes[0].data);
    REQUIRE(encoded.has_value());
    const std::vector<u8>& good = *encoded;

    SUBCASE("payload bit flip -> CRC error") {
        std::vector<u8> bad = good;
        bad[kCookedHeaderSize + (bad.size() - kCookedHeaderSize) / 2] ^= 0x10;
        auto r = decode_asset<MeshData>(as_bytes_view(bad));
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().message.find("CRC") != String::npos);
    }
    SUBCASE("header bit flip -> header CRC error") {
        std::vector<u8> bad = good;
        bad[20] ^= 0x01; // inside the AssetId
        REQUIRE_FALSE(decode_header(as_bytes_view(bad)).has_value());
    }
    SUBCASE("truncation") {
        std::vector<u8> bad(good.begin(), good.end() - 3);
        CHECK_FALSE(decode_asset<MeshData>(as_bytes_view(bad)).has_value());
        std::vector<u8> tiny(good.begin(), good.begin() + 10);
        CHECK_FALSE(decode_asset<MeshData>(as_bytes_view(tiny)).has_value());
    }
    SUBCASE("bad magic / version / type") {
        std::vector<u8> bad = good;
        bad[0] = 'X';
        CHECK_FALSE(decode_header(as_bytes_view(bad)).has_value());
        std::vector<u8> ver = good;
        ver[4] = 99;
        auto v = decode_header(as_bytes_view(ver));
        REQUIRE_FALSE(v.has_value());
        CHECK(v.error().code == ErrorCode::Unsupported);
        auto wrong_type = decode_asset<TextureData>(as_bytes_view(good));
        REQUIRE_FALSE(wrong_type.has_value());
        CHECK(wrong_type.error().code == ErrorCode::InvalidArgument);
    }
    SUBCASE("invalid data is rejected at encode time") {
        MeshData m = cube.meshes[0].data;
        m.indices.push_back(1000);
        CHECK(encode_asset(meta, m).error().code == ErrorCode::InvalidArgument);
        MeshData s = cube.meshes[0].data;
        s.skin.resize(3);
        CHECK_FALSE(encode_asset(meta, s).has_value());
        TextureData t = cube.textures[0].data;
        t.pixels.pop_back();
        CHECK_FALSE(encode_asset(meta, t).has_value());
        SkeletonData k;
        k.joint_names = { "a", "b" };
        k.parents = { 1, -1 }; // child before parent
        k.bind_local.resize(2);
        k.inverse_bind.resize(2);
        CHECK_FALSE(encode_asset(meta, k).has_value());
        AnimationClipData c;
        c.channels.resize(1);
        c.channels[0].interpolation = Interpolation::CubicSpline;
        c.channels[0].times = { 0.0f };
        c.channels[0].values.resize(1); // needs 3
        CHECK_FALSE(encode_asset(meta, c).has_value());
    }
}

TEST_CASE("format: file write/read and header-only read") {
    TempDir            dir("format");
    const ImportResult skinned = import_sample("skinned/skinned.gltf");
    const auto&        clip = skinned.animations[0];
    const fs::path     file = dir / "nested/dir/clip.aeasset";
    REQUIRE(write_asset(file, CookedMeta{ clip.id, 123, kImporterVersion }, clip.data).has_value());
    CHECK_FALSE(fs::exists(fs::path(file).concat(".tmp")));

    auto header = read_asset_header(file);
    REQUIRE(header.has_value());
    CHECK(header->type == AssetType::AnimationClip);
    CHECK(header->id == clip.id);
    CHECK(header->source_hash == 123u);

    CookedHeader h2;
    auto         back = read_asset<AnimationClipData>(file, &h2);
    REQUIRE(back.has_value());
    CHECK(eq(clip.data, *back));
    CHECK(h2.id == clip.id);

    auto missing = read_asset<MeshData>(dir / "missing.aeasset");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);
}
