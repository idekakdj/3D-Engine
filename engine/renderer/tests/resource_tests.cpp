// resource_tests.cpp — range allocator, handle pool, frame arena, geometry arena growth.
#include "frame_arena.h"
#include "geometry_arena.h"
#include "handle_pool.h"
#include "mock_device.h"
#include "range_allocator.h"

#include <doctest/doctest.h>

#include <vector>

using namespace aether;
using namespace aether::renderer;

TEST_CASE("range allocator: first fit, alignment, coalescing, growth") {
    RangeAllocator a(100);
    const auto r0 = a.allocate(10);
    const auto r1 = a.allocate(20);
    const auto r2 = a.allocate(30);
    REQUIRE(r0);
    REQUIRE(r1);
    REQUIRE(r2);
    CHECK(r0->offset == 0);
    CHECK(r1->offset == 10);
    CHECK(r2->offset == 30);
    CHECK(a.used() == 60);
    CHECK_FALSE(a.allocate(41));

    a.free(*r1);
    const auto r3 = a.allocate(15); // reuses the hole
    REQUIRE(r3);
    CHECK(r3->offset == 10);
    a.free(*r3);
    a.free(*r0);
    CHECK(a.free_block_count() == 2); // [0,30) and [60,100)
    a.free(*r2);
    CHECK(a.free_block_count() == 1);
    CHECK(a.largest_free() == 100);
    CHECK(a.used() == 0);

    const auto x = a.allocate(3);
    const auto y = a.allocate(8, 16);
    REQUIRE(y);
    CHECK(y->offset % 16 == 0);
    CHECK(a.largest_free() >= 60);
    (void)x;

    a.grow(400);
    CHECK(a.capacity() == 400);
    const auto big = a.allocate(300);
    REQUIRE(big);
    std::vector<RangeAllocator::Range> live;
    a.for_each_allocated([&](RangeAllocator::Range r) { live.push_back(r); });
    CHECK(live.size() >= 2);
}

TEST_CASE("handle pool: generations reject stale handles") {
    struct Tag;
    HandlePool<Tag, int> pool;
    const auto h0 = pool.insert(7);
    const auto h1 = pool.insert(9);
    REQUIRE(pool.get(h0));
    CHECK(*pool.get(h0) == 7);
    CHECK(pool.size() == 2);
    CHECK(pool.remove(h0).value() == 7);
    CHECK(pool.get(h0) == nullptr);
    const auto h2 = pool.insert(11); // recycles slot 0 with a new generation
    CHECK(h2.index() == h0.index());
    CHECK(h2.generation() != h0.generation());
    CHECK(pool.get(h0) == nullptr);
    CHECK(*pool.get(h2) == 11);
    CHECK(pool.get(Handle<Tag>{}) == nullptr);
    CHECK_FALSE(pool.remove(h0).has_value());
    CHECK(*pool.get(h1) == 9);
}

TEST_CASE("frame arena: aligned bump allocation and growth") {
    test::MockDevice device;
    FrameArena       arena;
    arena.init(device, 2, 1024);
    REQUIRE(arena.begin_frame(0, 512));
    const auto a = arena.allocate(10);
    const auto b = arena.allocate(100);
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    CHECK(b.offset % FrameArena::kAlignment == 0);
    CHECK(b.gpu - a.gpu == b.offset - a.offset);
    CHECK_FALSE(arena.allocate(4096).valid());
    REQUIRE(arena.begin_frame(1, 8192)); // slot 1 grows
    CHECK(arena.capacity(1) >= 8192);
    CHECK(arena.capacity(0) == 1024);
    CHECK(arena.allocate(8000).valid());
    arena.shutdown();
    CHECK(device.state.errors.empty());
}

TEST_CASE("geometry arena: uploads, lockstep skin stream, deferred growth copy") {
    test::MockDevice device;
    GeometryArena    arena;
    GeometryArena::Capacities caps;
    caps.indices = 12;
    caps.static_vertices = 8;
    caps.skinned_vertices = 4;
    REQUIRE(arena.init(device, caps));

    std::vector<Vertex>     verts(6);
    std::vector<SkinVertex> skin(6);
    std::vector<u32>        idx = { 0, 1, 2, 3, 4, 5 };
    const auto a = arena.upload(verts, {}, idx);
    REQUIRE(a);
    CHECK_FALSE(a->skinned);
    const auto s = arena.upload(verts, skin, idx); // needs growth of the skinned arena
    REQUIRE(s);
    CHECK(s->skinned);
    CHECK(arena.has_pending_copies());
    const auto b = arena.upload(verts, {}, idx);   // static + index growth
    REQUIRE(b);
    CHECK(b->vertices.offset >= 6);

    test::MockCommandList cmd(device.state);
    arena.record_pending_copies(cmd);
    CHECK_FALSE(arena.has_pending_copies());
    CHECK(cmd.balanced());
    CHECK(device.state.errors.empty());

    CHECK_FALSE(arena.upload(verts, std::vector<SkinVertex>(3), idx)); // mismatched skin
    arena.release(*a);
    arena.shutdown();
}
