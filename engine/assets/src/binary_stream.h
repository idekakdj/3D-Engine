// binary_stream.h — little-endian binary writer/reader for the cooked format (private).
//
// Portable by construction: integers are assembled byte-by-byte, floats go through
// std::bit_cast, and bulk POD arrays use memcpy only on little-endian hosts. The reader
// is bounds-checked: any overrun latches failed() and yields zeros, never UB.
#pragma once

#include "aether/core/handle.h"
#include "aether/core/math.h"
#include "aether/core/types.h"

#include <bit>
#include <cstring>
#include <string>
#include <vector>

namespace aether::assets::detail {

class BinaryWriter {
public:
    void reserve(usize n) { buf_.reserve(n); }
    [[nodiscard]] std::vector<u8>&       buffer() noexcept { return buf_; }
    [[nodiscard]] const std::vector<u8>& buffer() const noexcept { return buf_; }
    [[nodiscard]] usize                  size() const noexcept { return buf_.size(); }

    void put_u8(u8 v) { buf_.push_back(v); }
    void put_u16(u16 v) { put_le(v, 2); }
    void put_u32(u32 v) { put_le(v, 4); }
    void put_u64(u64 v) { put_le(v, 8); }
    void put_i32(i32 v) { put_u32(static_cast<u32>(v)); }
    void put_f32(f32 v) { put_u32(std::bit_cast<u32>(v)); }
    void put_bool(bool v) { put_u8(v ? 1 : 0); }
    void put_bytes(const void* data, usize n) {
        if (n == 0) return;
        const usize at = buf_.size();
        buf_.resize(at + n);
        std::memcpy(buf_.data() + at, data, n);
    }
    void put_zeros(usize n) { buf_.insert(buf_.end(), n, u8{ 0 }); }

    void put_string(StringView s) {
        put_u32(static_cast<u32>(s.size()));
        put_bytes(s.data(), s.size());
    }
    void put_vec2(const Vec2& v) { put_f32(v.x); put_f32(v.y); }
    void put_vec3(const Vec3& v) { put_f32(v.x); put_f32(v.y); put_f32(v.z); }
    void put_vec4(const Vec4& v) { put_f32(v.x); put_f32(v.y); put_f32(v.z); put_f32(v.w); }
    void put_quat(const Quat& q) { put_f32(q.x); put_f32(q.y); put_f32(q.z); put_f32(q.w); }
    void put_mat4(const Mat4& m) {
        for (int c = 0; c < 4; ++c) put_vec4(m[c]);
    }
    void put_id(AssetId id) { put_u64(id.hi); put_u64(id.lo); }
    void put_aabb(const AABB& b) { put_vec3(b.min); put_vec3(b.max); }
    void put_transform(const Transform& t) {
        put_vec3(t.position);
        put_quat(t.rotation);
        put_vec3(t.scale);
    }

    // Overwrite a previously written u32/u64 (header back-patching).
    void patch_u32(usize at, u32 v) { patch_le(at, v, 4); }
    void patch_u64(usize at, u64 v) { patch_le(at, v, 8); }

private:
    void put_le(u64 v, int bytes) {
        for (int i = 0; i < bytes; ++i) buf_.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void patch_le(usize at, u64 v, int bytes) {
        for (int i = 0; i < bytes; ++i) buf_[at + static_cast<usize>(i)] = static_cast<u8>(v >> (8 * i));
    }
    std::vector<u8> buf_;
};

class BinaryReader {
public:
    BinaryReader(const u8* data, usize size) : data_(data), size_(size) {}

    [[nodiscard]] bool  failed() const noexcept { return failed_; }
    [[nodiscard]] usize remaining() const noexcept { return size_ - pos_; }
    [[nodiscard]] usize position() const noexcept { return pos_; }
    [[nodiscard]] bool  at_end() const noexcept { return pos_ == size_; }
    void                fail() noexcept { failed_ = true; }

    u8   get_u8() { return static_cast<u8>(get_le(1)); }
    u16  get_u16() { return static_cast<u16>(get_le(2)); }
    u32  get_u32() { return static_cast<u32>(get_le(4)); }
    u64  get_u64() { return get_le(8); }
    i32  get_i32() { return static_cast<i32>(get_u32()); }
    f32  get_f32() { return std::bit_cast<f32>(get_u32()); }
    bool get_bool() { return get_u8() != 0; }

    bool get_bytes(void* out, usize n) {
        if (!ensure(n)) {
            if (n) std::memset(out, 0, n);
            return false;
        }
        if (n) std::memcpy(out, data_ + pos_, n);
        pos_ += n;
        return true;
    }
    bool skip(usize n) {
        if (!ensure(n)) return false;
        pos_ += n;
        return true;
    }

    String get_string() {
        const u32 n = get_u32();
        if (!ensure(n)) return {};
        String s(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return s;
    }
    Vec2 get_vec2() { const f32 x = get_f32(); const f32 y = get_f32(); return { x, y }; }
    Vec3 get_vec3() {
        const f32 x = get_f32(); const f32 y = get_f32(); const f32 z = get_f32();
        return { x, y, z };
    }
    Vec4 get_vec4() {
        const f32 x = get_f32(); const f32 y = get_f32(); const f32 z = get_f32(); const f32 w = get_f32();
        return { x, y, z, w };
    }
    Quat get_quat() {
        const f32 x = get_f32(); const f32 y = get_f32(); const f32 z = get_f32(); const f32 w = get_f32();
        return Quat(w, x, y, z);
    }
    Mat4 get_mat4() {
        Mat4 m(1.0f);
        for (int c = 0; c < 4; ++c) m[c] = get_vec4();
        return m;
    }
    AssetId get_id() {
        AssetId id;
        id.hi = get_u64();
        id.lo = get_u64();
        return id;
    }
    AABB get_aabb() {
        AABB b;
        b.min = get_vec3();
        b.max = get_vec3();
        return b;
    }
    Transform get_transform() {
        Transform t;
        t.position = get_vec3();
        t.rotation = get_quat();
        t.scale = get_vec3();
        return t;
    }

    // Reads an element count and checks that `count * min_element_bytes` bytes remain,
    // so corrupt counts can never trigger huge allocations.
    bool get_count(u64& count, usize min_element_bytes) {
        count = get_u64();
        if (failed_) return false;
        if (min_element_bytes > 0 && count > remaining() / min_element_bytes) {
            failed_ = true;
            count = 0;
            return false;
        }
        return true;
    }

private:
    bool ensure(usize n) {
        if (failed_ || n > size_ - pos_) {
            failed_ = true;
            return false;
        }
        return true;
    }
    u64 get_le(int bytes) {
        if (!ensure(static_cast<usize>(bytes))) return 0;
        u64 v = 0;
        for (int i = 0; i < bytes; ++i) v |= static_cast<u64>(data_[pos_ + static_cast<usize>(i)]) << (8 * i);
        pos_ += static_cast<usize>(bytes);
        return v;
    }

    const u8* data_ = nullptr;
    usize     size_ = 0;
    usize     pos_ = 0;
    bool      failed_ = false;
};

} // namespace aether::assets::detail
