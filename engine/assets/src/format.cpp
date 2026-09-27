// format.cpp — cooked .aeasset encode/decode, checksums.
#include "aether/assets/format.h"

#include "binary_stream.h"
#include "file_util.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <fstream>
#include <type_traits>

namespace fs = std::filesystem;

namespace aether::assets {

using detail::BinaryReader;
using detail::BinaryWriter;

// ===========================================================================
// Checksums
// ===========================================================================
namespace {
constexpr std::array<u32, 256> make_crc_table() {
    std::array<u32, 256> table{};
    for (u32 i = 0; i < 256; ++i) {
        u32 c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    return table;
}
constexpr std::array<u32, 256> kCrcTable = make_crc_table();
} // namespace

u32 crc32(ByteSpan data, u32 seed) noexcept {
    u32 c = ~seed;
    for (byte b : data) c = kCrcTable[(c ^ static_cast<u32>(b)) & 0xFFu] ^ (c >> 8);
    return ~c;
}

u64 fnv1a64(ByteSpan data, u64 seed) noexcept {
    u64 h = seed;
    for (byte b : data) {
        h ^= static_cast<u64>(b);
        h *= 0x100000001b3ull;
    }
    return h;
}

u64 fnv1a64(StringView text, u64 seed) noexcept {
    return fnv1a64(detail::bytes_of(text), seed);
}

// ===========================================================================
// Texture sizes
// ===========================================================================
u32 texture_format_bytes_per_pixel(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::RGBA8_UNORM:
    case TextureFormat::RGBA8_SRGB: return 4;
    case TextureFormat::RGBA16F: return 8;
    case TextureFormat::RGBA32F: return 16;
    case TextureFormat::BC7_SRGB:
    case TextureFormat::BC7_UNORM:
    case TextureFormat::BC5_UNORM: return 0; // block-compressed: see texture_format_block_bytes()
    }
    return 0;
}

bool is_block_compressed(TextureFormat format) noexcept {
    return format == TextureFormat::BC7_SRGB || format == TextureFormat::BC7_UNORM ||
           format == TextureFormat::BC5_UNORM;
}

u32 texture_format_block_bytes(TextureFormat format) noexcept {
    return is_block_compressed(format) ? 16u : 0u;
}

u64 texture_layer_byte_size(TextureFormat format, u32 width, u32 height, u32 mip) noexcept {
    if (mip >= 32) return 0;
    const u64 w = std::max<u64>(1, static_cast<u64>(width) >> mip);
    const u64 h = std::max<u64>(1, static_cast<u64>(height) >> mip);
    if (is_block_compressed(format)) return ((w + 3) / 4) * ((h + 3) / 4) * texture_format_block_bytes(format);
    return w * h * texture_format_bytes_per_pixel(format);
}

u64 texture_mip_offset(const TextureData& t, u32 mip) noexcept {
    u64 offset = 0;
    for (u32 m = 0; m < mip && m < 32; ++m) {
        offset += texture_layer_byte_size(t.format, t.width, t.height, m) * t.array_layers;
    }
    return offset;
}

u64 texture_byte_size(const TextureData& t) noexcept {
    return texture_mip_offset(t, t.mip_levels);
}

namespace {

constexpr bool kLittleEndian = std::endian::native == std::endian::little;
static_assert(std::is_trivially_copyable_v<Vertex> && sizeof(Vertex) == 48);
static_assert(std::is_trivially_copyable_v<SkinVertex> && sizeof(SkinVertex) == 24);

Error invalid(std::string msg) { return Error{ ErrorCode::InvalidArgument, std::move(msg) }; }
Error corrupt(std::string msg) { return Error{ ErrorCode::IoError, "corrupt cooked asset: " + std::move(msg) }; }

// ===========================================================================
// Validation (shared by encode and decode)
// ===========================================================================
Result<void> validate(const MeshData& m) {
    if (!m.skin.empty() && m.skin.size() != m.vertices.size())
        return invalid(std::format("mesh skin stream has {} entries for {} vertices", m.skin.size(),
                                   m.vertices.size()));
    for (u32 idx : m.indices) {
        if (idx >= m.vertices.size())
            return invalid(std::format("mesh index {} out of range ({} vertices)", idx, m.vertices.size()));
    }
    for (const Submesh& s : m.submeshes) {
        if (static_cast<u64>(s.first_index) + s.index_count > m.indices.size())
            return invalid("submesh index range exceeds the index buffer");
    }
    return {};
}

Result<void> validate(const TextureData& t) {
    if (static_cast<u8>(t.format) > static_cast<u8>(TextureFormat::BC5_UNORM))
        return invalid("unknown texture format");
    if (t.width == 0 || t.height == 0 || t.width > (1u << 18) || t.height > (1u << 18))
        return invalid(std::format("bad texture size {}x{}", t.width, t.height));
    if (t.array_layers == 0 || t.array_layers > 2048) return invalid("bad texture array_layers");
    if (t.mip_levels == 0 || t.mip_levels > static_cast<u32>(std::bit_width(std::max(t.width, t.height))))
        return invalid("bad texture mip_levels");
    if (t.is_cubemap && t.array_layers % 6 != 0) return invalid("cubemap layers must be a multiple of 6");
    if (t.pixels.size() != texture_byte_size(t))
        return invalid(std::format("texture has {} pixel bytes, expected {}", t.pixels.size(),
                                   texture_byte_size(t)));
    return {};
}

Result<void> validate(const MaterialData& m) {
    if (static_cast<u8>(m.alpha_mode) > static_cast<u8>(AlphaMode::Blend)) return invalid("bad alpha mode");
    return {};
}

Result<void> validate(const SkeletonData& s) {
    const usize n = s.parents.size();
    if (s.joint_names.size() != n || s.bind_local.size() != n || s.inverse_bind.size() != n)
        return invalid("skeleton arrays differ in length");
    for (usize i = 0; i < n; ++i) {
        if (s.parents[i] < -1 || s.parents[i] >= static_cast<i32>(i))
            return invalid(std::format("skeleton joint {} has parent {} (must be -1 or < index)", i,
                                       s.parents[i]));
    }
    return {};
}

Result<void> validate(const AnimationClipData& c) {
    for (const AnimationChannel& ch : c.channels) {
        if (static_cast<u8>(ch.path) > static_cast<u8>(AnimPath::Scale)) return invalid("bad channel path");
        if (static_cast<u8>(ch.interpolation) > static_cast<u8>(Interpolation::CubicSpline))
            return invalid("bad channel interpolation");
        const usize per_key = ch.interpolation == Interpolation::CubicSpline ? 3 : 1;
        if (ch.values.size() != ch.times.size() * per_key)
            return invalid(std::format("channel has {} values for {} keys", ch.values.size(), ch.times.size()));
    }
    return {};
}

Result<void> validate(const SceneData& s) {
    for (usize i = 0; i < s.nodes.size(); ++i) {
        if (s.nodes[i].parent < -1 || s.nodes[i].parent >= static_cast<i32>(i))
            return invalid(std::format("scene node {} has parent {} (must be -1 or < index)", i,
                                       s.nodes[i].parent));
    }
    return {};
}

// ===========================================================================
// Bulk arrays
// ===========================================================================
template <typename T, typename PutFn>
void put_array(BinaryWriter& w, const std::vector<T>& v, PutFn&& put_one) {
    w.put_u64(v.size());
    if constexpr (kLittleEndian && std::is_trivially_copyable_v<T> &&
                  (std::is_same_v<T, Vertex> || std::is_same_v<T, SkinVertex> ||
                   std::is_same_v<T, u32> || std::is_same_v<T, f32> || std::is_same_v<T, Vec4>)) {
        (void)put_one;
        w.put_bytes(v.data(), v.size() * sizeof(T));
    } else {
        for (const T& e : v) put_one(w, e);
    }
}

template <typename T, typename GetFn>
bool get_array(BinaryReader& r, std::vector<T>& v, usize wire_size, GetFn&& get_one) {
    u64 n = 0;
    if (!r.get_count(n, wire_size)) return false;
    v.resize(static_cast<usize>(n));
    if constexpr (kLittleEndian && std::is_trivially_copyable_v<T> &&
                  (std::is_same_v<T, Vertex> || std::is_same_v<T, SkinVertex> ||
                   std::is_same_v<T, u32> || std::is_same_v<T, f32> || std::is_same_v<T, Vec4>)) {
        (void)get_one;
        return r.get_bytes(v.data(), v.size() * sizeof(T));
    } else {
        for (T& e : v) e = get_one(r);
        return !r.failed();
    }
}

void put_vertex(BinaryWriter& w, const Vertex& v) {
    w.put_vec3(v.position);
    w.put_vec3(v.normal);
    w.put_vec4(v.tangent);
    w.put_vec2(v.uv0);
}
Vertex get_vertex(BinaryReader& r) {
    Vertex v;
    v.position = r.get_vec3();
    v.normal = r.get_vec3();
    v.tangent = r.get_vec4();
    v.uv0 = r.get_vec2();
    return v;
}
void put_skin(BinaryWriter& w, const SkinVertex& s) {
    for (u16 j : s.joints) w.put_u16(j);
    w.put_vec4(s.weights);
}
SkinVertex get_skin(BinaryReader& r) {
    SkinVertex s;
    for (u16& j : s.joints) j = r.get_u16();
    s.weights = r.get_vec4();
    return s;
}
void put_u32v(BinaryWriter& w, const u32& v) { w.put_u32(v); }
u32  get_u32v(BinaryReader& r) { return r.get_u32(); }
void put_f32v(BinaryWriter& w, const f32& v) { w.put_f32(v); }
f32  get_f32v(BinaryReader& r) { return r.get_f32(); }
void put_vec4v(BinaryWriter& w, const Vec4& v) { w.put_vec4(v); }
Vec4 get_vec4v(BinaryReader& r) { return r.get_vec4(); }

// ===========================================================================
// Payloads
// ===========================================================================
void write_payload(BinaryWriter& w, const MeshData& m) {
    put_array(w, m.vertices, put_vertex);
    put_array(w, m.skin, put_skin);
    put_array(w, m.indices, put_u32v);
    w.put_u64(m.submeshes.size());
    for (const Submesh& s : m.submeshes) {
        w.put_u32(s.first_index);
        w.put_u32(s.index_count);
        w.put_u32(s.material_slot);
        w.put_aabb(s.bounds);
    }
    w.put_aabb(m.bounds);
    w.put_id(m.skeleton);
}
void read_payload(BinaryReader& r, MeshData& m) {
    get_array(r, m.vertices, sizeof(Vertex), get_vertex);
    get_array(r, m.skin, sizeof(SkinVertex), get_skin);
    get_array(r, m.indices, 4, get_u32v);
    u64 n = 0;
    if (!r.get_count(n, 36)) return;
    m.submeshes.resize(static_cast<usize>(n));
    for (Submesh& s : m.submeshes) {
        s.first_index = r.get_u32();
        s.index_count = r.get_u32();
        s.material_slot = r.get_u32();
        s.bounds = r.get_aabb();
    }
    m.bounds = r.get_aabb();
    m.skeleton = r.get_id();
}

void write_payload(BinaryWriter& w, const TextureData& t) {
    w.put_u32(t.width);
    w.put_u32(t.height);
    w.put_u32(t.mip_levels);
    w.put_u32(t.array_layers);
    w.put_bool(t.is_cubemap);
    w.put_u8(static_cast<u8>(t.format));
    w.put_u64(t.pixels.size());
    w.put_bytes(t.pixels.data(), t.pixels.size());
}
void read_payload(BinaryReader& r, TextureData& t) {
    t.width = r.get_u32();
    t.height = r.get_u32();
    t.mip_levels = r.get_u32();
    t.array_layers = r.get_u32();
    t.is_cubemap = r.get_bool();
    t.format = static_cast<TextureFormat>(r.get_u8());
    u64 n = 0;
    if (!r.get_count(n, 1)) return;
    t.pixels.resize(static_cast<usize>(n));
    r.get_bytes(t.pixels.data(), t.pixels.size());
}

void write_payload(BinaryWriter& w, const MaterialData& m) {
    w.put_string(m.name);
    w.put_vec4(m.base_color_factor);
    w.put_vec3(m.emissive_factor);
    w.put_f32(m.metallic_factor);
    w.put_f32(m.roughness_factor);
    w.put_f32(m.normal_scale);
    w.put_f32(m.occlusion_strength);
    w.put_f32(m.alpha_cutoff);
    w.put_u8(static_cast<u8>(m.alpha_mode));
    w.put_bool(m.double_sided);
    w.put_id(m.base_color_texture);
    w.put_id(m.metallic_roughness_texture);
    w.put_id(m.normal_texture);
    w.put_id(m.occlusion_texture);
    w.put_id(m.emissive_texture);
}
void read_payload(BinaryReader& r, MaterialData& m) {
    m.name = r.get_string();
    m.base_color_factor = r.get_vec4();
    m.emissive_factor = r.get_vec3();
    m.metallic_factor = r.get_f32();
    m.roughness_factor = r.get_f32();
    m.normal_scale = r.get_f32();
    m.occlusion_strength = r.get_f32();
    m.alpha_cutoff = r.get_f32();
    m.alpha_mode = static_cast<AlphaMode>(r.get_u8());
    m.double_sided = r.get_bool();
    m.base_color_texture = r.get_id();
    m.metallic_roughness_texture = r.get_id();
    m.normal_texture = r.get_id();
    m.occlusion_texture = r.get_id();
    m.emissive_texture = r.get_id();
}

void write_payload(BinaryWriter& w, const SkeletonData& s) {
    w.put_u64(s.parents.size());
    for (usize i = 0; i < s.parents.size(); ++i) {
        w.put_string(s.joint_names[i]);
        w.put_i32(s.parents[i]);
        w.put_transform(s.bind_local[i]);
        w.put_mat4(s.inverse_bind[i]);
    }
}
void read_payload(BinaryReader& r, SkeletonData& s) {
    u64 n = 0;
    if (!r.get_count(n, 4 + 4 + 40 + 64)) return;
    const usize count = static_cast<usize>(n);
    s.joint_names.resize(count);
    s.parents.resize(count);
    s.bind_local.resize(count);
    s.inverse_bind.resize(count);
    for (usize i = 0; i < count && !r.failed(); ++i) {
        s.joint_names[i] = r.get_string();
        s.parents[i] = r.get_i32();
        s.bind_local[i] = r.get_transform();
        s.inverse_bind[i] = r.get_mat4();
    }
}

void write_payload(BinaryWriter& w, const AnimationClipData& c) {
    w.put_string(c.name);
    w.put_f32(c.duration);
    w.put_id(c.skeleton);
    w.put_u64(c.channels.size());
    for (const AnimationChannel& ch : c.channels) {
        w.put_u32(ch.joint);
        w.put_u8(static_cast<u8>(ch.path));
        w.put_u8(static_cast<u8>(ch.interpolation));
        put_array(w, ch.times, put_f32v);
        put_array(w, ch.values, put_vec4v);
    }
}
void read_payload(BinaryReader& r, AnimationClipData& c) {
    c.name = r.get_string();
    c.duration = r.get_f32();
    c.skeleton = r.get_id();
    u64 n = 0;
    if (!r.get_count(n, 4 + 2 + 8 + 8)) return;
    c.channels.resize(static_cast<usize>(n));
    for (AnimationChannel& ch : c.channels) {
        if (r.failed()) break;
        ch.joint = r.get_u32();
        ch.path = static_cast<AnimPath>(r.get_u8());
        ch.interpolation = static_cast<Interpolation>(r.get_u8());
        get_array(r, ch.times, 4, get_f32v);
        get_array(r, ch.values, 16, get_vec4v);
    }
}

void write_payload(BinaryWriter& w, const SceneData& s) {
    w.put_string(s.name);
    w.put_u64(s.nodes.size());
    for (const SceneNodeData& n : s.nodes) {
        w.put_string(n.name);
        w.put_i32(n.parent);
        w.put_transform(n.local);
        w.put_id(n.mesh);
        w.put_u64(n.materials.size());
        for (const AssetId& id : n.materials) w.put_id(id);
        w.put_id(n.skeleton);
    }
}
void read_payload(BinaryReader& r, SceneData& s) {
    s.name = r.get_string();
    u64 n = 0;
    if (!r.get_count(n, 4 + 4 + 40 + 16 + 8 + 16)) return;
    s.nodes.resize(static_cast<usize>(n));
    for (SceneNodeData& node : s.nodes) {
        if (r.failed()) break;
        node.name = r.get_string();
        node.parent = r.get_i32();
        node.local = r.get_transform();
        node.mesh = r.get_id();
        u64 m = 0;
        if (!r.get_count(m, 16)) break;
        node.materials.resize(static_cast<usize>(m));
        for (AssetId& id : node.materials) id = r.get_id();
        node.skeleton = r.get_id();
    }
}

// ===========================================================================
// Header
// ===========================================================================
constexpr usize kPayloadCrcOffset = 56;
constexpr usize kHeaderCrcOffset = 60;
constexpr usize kPayloadBytesOffset = 48;

template <typename T>
Result<std::vector<u8>> encode_impl(const CookedMeta& meta, const T& data) {
    if (auto ok = validate(data); !ok) return ok.error();

    BinaryWriter w;
    w.reserve(kCookedHeaderSize + 256);
    w.put_bytes(kCookedMagic.data(), kCookedMagic.size());
    w.put_u32(kCookedFormatVersion);
    w.put_u8(static_cast<u8>(asset_type_of_v<T>));
    w.put_zeros(7);
    w.put_id(meta.id);
    w.put_u64(meta.source_hash);
    w.put_u32(meta.importer_version);
    w.put_zeros(4);
    w.put_u64(0); // payload_bytes (patched)
    w.put_u32(0); // payload_crc32 (patched)
    w.put_u32(0); // header_crc32 (patched)

    write_payload(w, data);

    std::vector<u8>& buf = w.buffer();
    const usize      payload_size = buf.size() - kCookedHeaderSize;
    const u32 payload_crc = crc32(ByteSpan(reinterpret_cast<const byte*>(buf.data()) + kCookedHeaderSize, payload_size));
    w.patch_u64(kPayloadBytesOffset, payload_size);
    w.patch_u32(kPayloadCrcOffset, payload_crc);
    w.patch_u32(kHeaderCrcOffset, crc32(ByteSpan(reinterpret_cast<const byte*>(buf.data()), kHeaderCrcOffset)));
    return std::move(buf);
}

} // namespace

Result<std::vector<u8>> encode_asset(const CookedMeta& meta, const MeshData& data) { return encode_impl(meta, data); }
Result<std::vector<u8>> encode_asset(const CookedMeta& meta, const TextureData& data) { return encode_impl(meta, data); }
Result<std::vector<u8>> encode_asset(const CookedMeta& meta, const MaterialData& data) { return encode_impl(meta, data); }
Result<std::vector<u8>> encode_asset(const CookedMeta& meta, const SkeletonData& data) { return encode_impl(meta, data); }
Result<std::vector<u8>> encode_asset(const CookedMeta& meta, const AnimationClipData& data) { return encode_impl(meta, data); }
Result<std::vector<u8>> encode_asset(const CookedMeta& meta, const SceneData& data) { return encode_impl(meta, data); }

Result<CookedHeader> decode_header(ByteSpan bytes) {
    if (bytes.size() < kCookedHeaderSize) return corrupt(std::format("file too short ({} bytes)", bytes.size()));
    const u8* p = reinterpret_cast<const u8*>(bytes.data());
    if (std::memcmp(p, kCookedMagic.data(), kCookedMagic.size()) != 0) return corrupt("bad magic (not an .aeasset)");

    BinaryReader r(p, kCookedHeaderSize);
    r.skip(4);
    CookedHeader h;
    h.format_version = r.get_u32();
    if (h.format_version != kCookedFormatVersion) {
        return Error{ ErrorCode::Unsupported, std::format("cooked format version {} (expected {})",
                                                          h.format_version, kCookedFormatVersion) };
    }
    const u8 type = r.get_u8();
    r.skip(7);
    h.id = r.get_id();
    h.source_hash = r.get_u64();
    h.importer_version = r.get_u32();
    r.skip(4);
    h.payload_bytes = r.get_u64();
    h.payload_crc32 = r.get_u32();
    const u32 header_crc = r.get_u32();
    if (header_crc != crc32(ByteSpan(bytes.data(), kHeaderCrcOffset))) return corrupt("header CRC mismatch");
    if (type == 0 || type > static_cast<u8>(AssetType::Script)) return corrupt(std::format("bad asset type {}", type));
    h.type = static_cast<AssetType>(type);
    return h;
}

template <CookableAsset T>
Result<T> decode_asset(ByteSpan bytes, CookedHeader* out_header) {
    auto header = decode_header(bytes);
    if (!header) return header.error();
    const CookedHeader& h = *header;
    if (h.type != asset_type_of_v<T>) {
        return Error{ ErrorCode::InvalidArgument,
                      std::format("cooked asset is a {}, requested {}", asset_type_name(h.type),
                                  asset_type_name(asset_type_of_v<T>)) };
    }
    const usize payload_size = bytes.size() - kCookedHeaderSize;
    if (h.payload_bytes != payload_size) {
        return corrupt(std::format("payload is {} bytes, header says {}", payload_size, h.payload_bytes));
    }
    const ByteSpan payload = bytes.subspan(kCookedHeaderSize);
    if (crc32(payload) != h.payload_crc32) return corrupt("payload CRC mismatch");

    BinaryReader r(reinterpret_cast<const u8*>(payload.data()), payload.size());
    T            data{};
    read_payload(r, data);
    if (r.failed()) return corrupt("payload truncated or malformed");
    if (!r.at_end()) return corrupt(std::format("{} trailing payload bytes", r.remaining()));
    if (auto ok = validate(data); !ok) return corrupt(ok.error().message);
    if (out_header) *out_header = h;
    return data;
}

template <CookableAsset T>
Result<void> write_asset(const fs::path& path, const CookedMeta& meta, const T& data) {
    auto bytes = encode_asset(meta, data);
    if (!bytes) return bytes.error();
    return detail::write_file_atomic(path, detail::bytes_of(*bytes));
}

template <CookableAsset T>
Result<T> read_asset(const fs::path& path, CookedHeader* out_header) {
    auto bytes = detail::read_file_bytes(path);
    if (!bytes) return bytes.error();
    auto result = decode_asset<T>(detail::bytes_of(*bytes), out_header);
    if (!result) {
        return Error{ result.error().code, std::format("{}: {}", detail::to_utf8(path), result.error().message) };
    }
    return result;
}

Result<CookedHeader> read_asset_header(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error{ ErrorCode::NotFound, std::format("cannot open {}", detail::to_utf8(path)) };
    std::array<u8, kCookedHeaderSize> buf{};
    in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    const auto got = static_cast<usize>(in.gcount());
    return decode_header(ByteSpan(reinterpret_cast<const byte*>(buf.data()), got));
}

#define AE_ASSETS_INSTANTIATE_FORMAT(T)                                                            \
    template Result<T>    decode_asset<T>(ByteSpan, CookedHeader*);                                \
    template Result<void> write_asset<T>(const fs::path&, const CookedMeta&, const T&);            \
    template Result<T>    read_asset<T>(const fs::path&, CookedHeader*);
AE_ASSETS_INSTANTIATE_FORMAT(MeshData)
AE_ASSETS_INSTANTIATE_FORMAT(TextureData)
AE_ASSETS_INSTANTIATE_FORMAT(MaterialData)
AE_ASSETS_INSTANTIATE_FORMAT(SkeletonData)
AE_ASSETS_INSTANTIATE_FORMAT(AnimationClipData)
AE_ASSETS_INSTANTIATE_FORMAT(SceneData)
#undef AE_ASSETS_INSTANTIATE_FORMAT

} // namespace aether::assets
