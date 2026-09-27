// import_tests.cpp — glTF / image importers against the sample content + synthetic files.
#include "aether/assets/format.h"
#include "aether/assets/importers.h"

#include "test_helpers.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <format>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace fs = std::filesystem;
using namespace aether;
using namespace aether::assets;
using namespace aether::assets::test;

namespace {

bool near(const Vec3& a, const Vec3& b, f32 eps = 1e-5f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

ImportSettings content_settings() {
    ImportSettings s;
    s.content_root = content_dir();
    return s;
}

const ImportedAsset<SkeletonData>& only_skeleton(const ImportResult& r) {
    REQUIRE(r.skeletons.size() == 1);
    return r.skeletons[0];
}

void write_png(const fs::path& path, int w, int h, const std::vector<u8>& rgba) {
    std::vector<u8> encoded;
    auto sink = [](void* ctx, void* data, int size) {
        auto* out = static_cast<std::vector<u8>*>(ctx);
        out->insert(out->end(), static_cast<u8*>(data), static_cast<u8*>(data) + size);
    };
    REQUIRE(stbi_write_png_to_func(sink, &encoded, w, h, 4, rgba.data(), w * 4) != 0);
    write_text(path, std::string(encoded.begin(), encoded.end()));
}

// Checks the tangent frame of every triangle against its UV gradients.
void check_tangent_frames(const MeshData& m) {
    for (usize t = 0; t + 2 < m.indices.size(); t += 3) {
        const Vertex& a = m.vertices[m.indices[t]];
        const Vertex& b = m.vertices[m.indices[t + 1]];
        const Vertex& c = m.vertices[m.indices[t + 2]];
        const Vec3 e1 = b.position - a.position, e2 = c.position - a.position;
        const Vec2 d1 = b.uv0 - a.uv0, d2 = c.uv0 - a.uv0;
        const f32  det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-8f) continue;
        const Vec3 dpdu = (e1 * d2.y - e2 * d1.y) / det;
        const Vec3 dpdv = (e2 * d1.x - e1 * d2.x) / det;
        for (const Vertex* v : { &a, &b, &c }) {
            const Vec3 t3(v->tangent);
            CHECK(std::fabs(glm::length(t3) - 1.0f) < 1e-4f);
            CHECK(std::fabs(glm::dot(t3, v->normal)) < 1e-4f);
            CHECK(std::fabs(v->tangent.w) == 1.0f);
            CHECK(glm::dot(t3, dpdu) > 0.0f);
            const Vec3 bitangent = glm::cross(v->normal, t3) * v->tangent.w;
            CHECK(glm::dot(bitangent, dpdv) < 0.0f); // glTF: +Y-up normal maps, top-left UV origin
        }
    }
}

} // namespace

TEST_CASE("import: cube.gltf mesh, material, textures, scene") {
    auto r = import_gltf(sample("cube/cube.gltf"), content_settings());
    REQUIRE_MESSAGE(r.has_value(), r.error().message);
    const ImportResult& res = *r;
    CHECK(res.source_path == "samples/cube/cube.gltf");
    CHECK(res.warnings.empty());

    REQUIRE(res.meshes.size() == 1);
    const MeshData& m = res.meshes[0].data;
    CHECK(res.meshes[0].name == "Cube");
    CHECK(m.vertices.size() == 24);
    CHECK(m.indices.size() == 36);
    REQUIRE(m.submeshes.size() == 1);
    CHECK(m.submeshes[0].first_index == 0);
    CHECK(m.submeshes[0].index_count == 36);
    CHECK(m.submeshes[0].material_slot == 0);
    CHECK(near(m.bounds.min, { -0.5f, -0.5f, -0.5f }));
    CHECK(near(m.bounds.max, { 0.5f, 0.5f, 0.5f }));
    CHECK(m.skin.empty());
    CHECK_FALSE(m.skeleton.is_valid());

    // Normals from the file; tangents generated (the file has none).
    const Vec3 face_normals[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    const Vec3 face_rights[6] = { { 0, 0, -1 }, { 0, 0, 1 }, { 1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, { -1, 0, 0 } };
    for (usize i = 0; i < 24; ++i) {
        CHECK(near(m.vertices[i].normal, face_normals[i / 4]));
        CHECK(near(Vec3(m.vertices[i].tangent), face_rights[i / 4]));
        CHECK(m.vertices[i].tangent.w == 1.0f);
    }
    check_tangent_frames(m);

    REQUIRE(res.materials.size() == 1);
    const MaterialData& mat = res.materials[0].data;
    CHECK(mat.name == "CubeMaterial");
    CHECK(mat.base_color_factor.y == doctest::Approx(0.8f));
    CHECK(mat.metallic_factor == 0.0f);
    CHECK(mat.roughness_factor == doctest::Approx(0.5f));
    CHECK(mat.alpha_mode == AlphaMode::Opaque);
    CHECK_FALSE(mat.double_sided);

    REQUIRE(res.textures.size() == 2);
    const auto* base = res.find<TextureData>(mat.base_color_texture);
    const auto* mr = res.find<TextureData>(mat.metallic_roughness_texture);
    REQUIRE(base != nullptr);
    REQUIRE(mr != nullptr);
    CHECK(base->key == "texture:0:srgb");
    CHECK(base->data.format == TextureFormat::RGBA8_SRGB);
    CHECK(base->data.width == 4);
    CHECK(base->data.height == 4);
    REQUIRE(base->data.pixels.size() == 4 * 4 * 4);
    CHECK(base->data.pixels[0] == 255); // (0,0) white
    CHECK(base->data.pixels[4 + 1] == 128); // (1,0) orange: G = 128
    CHECK(base->data.pixels[4 + 2] == 0);
    CHECK(mr->key == "texture:1:linear");
    CHECK(mr->data.format == TextureFormat::RGBA8_UNORM);
    CHECK(mr->data.width == 2);
    CHECK(mr->data.pixels[1] == 128);
    CHECK_FALSE(mat.normal_texture.is_valid());

    REQUIRE(res.scenes.size() == 1);
    const SceneData& scene = res.scenes[0].data;
    CHECK(res.primary == res.scenes[0].id);
    REQUIRE(scene.nodes.size() == 1);
    CHECK(scene.nodes[0].name == "Cube");
    CHECK(scene.nodes[0].parent == -1);
    CHECK(scene.nodes[0].mesh == res.meshes[0].id);
    REQUIRE(scene.nodes[0].materials.size() == 1);
    CHECK(scene.nodes[0].materials[0] == res.materials[0].id);
}

TEST_CASE("import: AssetIds are deterministic and path/key based") {
    auto a = import_gltf(sample("cube/cube.gltf"), content_settings());
    auto b = import_gltf(sample("cube/../cube/cube.gltf"), content_settings()); // same file, other spelling
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->source_path == b->source_path);
    CHECK(a->meshes[0].id == b->meshes[0].id);
    CHECK(a->materials[0].id == b->materials[0].id);
    CHECK(a->textures[0].id == b->textures[0].id);
    CHECK(a->meshes[0].id == make_asset_id("samples/cube/cube.gltf", "mesh:0"));
    CHECK(a->meshes[0].id == AssetId::from_string("samples/cube/cube.gltf#mesh:0"));
    CHECK(a->materials[0].id == AssetId::from_string("samples/cube/cube.gltf#material:0"));
    CHECK(a->scenes[0].id == AssetId::from_string("samples/cube/cube.gltf#scene:0"));
    CHECK(a->meshes[0].id != a->materials[0].id);

    // A different content root changes the relative path and therefore the ids.
    ImportSettings other;
    other.content_root = content_dir() / "samples";
    auto c = import_gltf(sample("cube/cube.gltf"), other);
    REQUIRE(c.has_value());
    CHECK(c->source_path == "cube/cube.gltf");
    CHECK(c->meshes[0].id != a->meshes[0].id);

    // Hex encoding round trip.
    const AssetId id = a->meshes[0].id;
    CHECK(asset_id_from_hex(asset_id_to_hex(id)) == id);
    CHECK_FALSE(asset_id_from_hex("xyz").has_value());
}

TEST_CASE("import: cube.glb matches cube.gltf") {
    auto text = import_gltf(sample("cube/cube.gltf"), content_settings());
    auto bin = import_gltf(sample("cube/cube.glb"), content_settings());
    REQUIRE(text.has_value());
    REQUIRE_MESSAGE(bin.has_value(), bin.error().message);
    CHECK(bin->source_path == "samples/cube/cube.glb");
    const MeshData& a = text->meshes[0].data;
    const MeshData& b = bin->meshes[0].data;
    REQUIRE(a.vertices.size() == b.vertices.size());
    CHECK(std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(Vertex)) == 0);
    CHECK(a.indices == b.indices);
    REQUIRE(bin->textures.size() == 2);
    CHECK(bin->textures[0].data.pixels == text->textures[0].data.pixels);
    CHECK(bin->meshes[0].id != text->meshes[0].id);
}

TEST_CASE("import: skinned.gltf skeleton order, remap, inverse binds, clip") {
    auto r = import_gltf(sample("skinned/skinned.gltf"), content_settings());
    REQUIRE_MESSAGE(r.has_value(), r.error().message);
    const ImportResult& res = *r;
    CHECK(res.warnings.empty());

    const auto&         skel_asset = only_skeleton(res);
    const SkeletonData& skel = skel_asset.data;
    CHECK(skel_asset.name == "BarSkeleton");
    // Skin order in the file is [Tip, Root, Mid]; the importer sorts parents first.
    REQUIRE(skel.joint_names.size() == 3);
    CHECK(skel.joint_names == std::vector<String>{ "Bone_Root", "Bone_Mid", "Bone_Tip" });
    CHECK(skel.parents == std::vector<i32>{ -1, 0, 1 });
    CHECK(near(skel.bind_local[0].position, { 0, 0, 0 }));
    CHECK(near(skel.bind_local[1].position, { 0, 1, 0 }));
    CHECK(near(skel.bind_local[2].position, { 0, 1, 0 }));
    // Inverse binds were given in skin order and must follow the remap.
    CHECK(near(Vec3(skel.inverse_bind[0][3]), { 0, 0, 0 }));
    CHECK(near(Vec3(skel.inverse_bind[1][3]), { 0, -1, 0 }));
    CHECK(near(Vec3(skel.inverse_bind[2][3]), { 0, -2, 0 }));
    // Consistency: model-space bind * inverse bind == identity for every joint.
    std::vector<Mat4> model(3);
    for (usize j = 0; j < 3; ++j) {
        model[j] = (skel.parents[j] >= 0 ? model[static_cast<usize>(skel.parents[j])] : Mat4(1.0f)) *
                   skel.bind_local[j].to_matrix();
        const Mat4 id = model[j] * skel.inverse_bind[j];
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) CHECK(id[c][row] == doctest::Approx(c == row ? 1.0f : 0.0f));
        }
    }

    REQUIRE(res.meshes.size() == 1);
    const MeshData& mesh = res.meshes[0].data;
    CHECK(mesh.skeleton == skel_asset.id);
    REQUIRE(mesh.skin.size() == mesh.vertices.size());
    usize checked = 0;
    for (usize i = 0; i < mesh.vertices.size(); ++i) {
        const SkinVertex& s = mesh.skin[i];
        CHECK(s.weights.x + s.weights.y + s.weights.z + s.weights.w == doctest::Approx(1.0f));
        const f32 y = mesh.vertices[i].position.y;
        if (y == 1.0f) { // root 128/255 + mid 127/255
            CHECK(s.joints[0] == 0);
            CHECK(s.joints[1] == 1);
            CHECK(s.weights.x == doctest::Approx(128.0f / 255.0f));
            ++checked;
        } else if (y == 2.0f) { // mid + tip
            CHECK(s.joints[0] == 1);
            CHECK(s.joints[1] == 2);
            ++checked;
        } else if (y == 3.0f) {
            CHECK(s.joints[0] == 2);
            CHECK(s.weights.x == doctest::Approx(1.0f));
            ++checked;
        } else if (y == 0.0f) {
            CHECK(s.joints[0] == 0);
            ++checked;
        }
    }
    CHECK(checked == mesh.vertices.size());
    check_tangent_frames(mesh);

    REQUIRE(res.animations.size() == 1);
    const AnimationClipData& clip = res.animations[0].data;
    CHECK(clip.name == "Wave");
    CHECK(clip.skeleton == skel_asset.id);
    CHECK(clip.duration == doctest::Approx(2.0f));
    REQUIRE(clip.channels.size() == 3);
    const AnimationChannel& rot = clip.channels[0];
    CHECK(rot.joint == 1); // Bone_Mid after remap
    CHECK(rot.path == AnimPath::Rotation);
    CHECK(rot.interpolation == Interpolation::Linear);
    REQUIRE(rot.times.size() == 5);
    REQUIRE(rot.values.size() == 5);
    CHECK(rot.values[1].z == doctest::Approx(std::sin(15.0f * kDeg2Rad)));
    CHECK(rot.values[1].w == doctest::Approx(std::cos(15.0f * kDeg2Rad)));
    const AnimationChannel& tr = clip.channels[1];
    CHECK(tr.joint == 0); // Bone_Root
    CHECK(tr.path == AnimPath::Translation);
    CHECK(tr.interpolation == Interpolation::Step);
    REQUIRE(tr.values.size() == 3);
    CHECK(tr.values[1].y == doctest::Approx(0.1f));
    const AnimationChannel& sc = clip.channels[2];
    CHECK(sc.joint == 2); // Bone_Tip
    CHECK(sc.path == AnimPath::Scale);
    CHECK(sc.interpolation == Interpolation::CubicSpline);
    REQUIRE(sc.times.size() == 3);
    REQUIRE(sc.values.size() == 9); // (in, value, out) per key
    CHECK(sc.values[4].x == doctest::Approx(1.2f));
    CHECK(sc.values[3].x == 0.0f);

    REQUIRE(res.scenes.size() == 1);
    const SceneData& scene = res.scenes[0].data;
    REQUIRE(scene.nodes.size() == 5);
    CHECK(scene.nodes[0].name == "Armature");
    for (usize i = 0; i < scene.nodes.size(); ++i) CHECK(scene.nodes[i].parent < static_cast<i32>(i));
    bool found_skinned = false;
    for (const SceneNodeData& n : scene.nodes) {
        if (n.name == "SkinnedBar") {
            found_skinned = true;
            CHECK(n.mesh == res.meshes[0].id);
            CHECK(n.skeleton == skel_asset.id);
            CHECK(n.materials.size() == 1);
        }
    }
    CHECK(found_skinned);
}

TEST_CASE("import: synthetic root joint when the skinned mesh sits outside the armature") {
    TempDir         dir("synthroot");
    std::vector<u8> buf;
    const usize     pos = append_f32(buf, { 0, 0, 0, 1, 0, 0, 0, 1, 0 });
    const usize     jnt = append_u16(buf, { 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 });
    const usize     wgt = append_f32(buf, { 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 });
    const std::string body = std::format(
        R"("bufferViews":[{{"buffer":0,"byteOffset":{},"byteLength":36}},{{"buffer":0,"byteOffset":{},"byteLength":24}},{{"buffer":0,"byteOffset":{},"byteLength":48}}],
"accessors":[{{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}},{{"bufferView":1,"componentType":5123,"count":3,"type":"VEC4"}},{{"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"}}],
"meshes":[{{"primitives":[{{"attributes":{{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2}}}}]}}],
"nodes":[{{"name":"Armature","translation":[0,0,5],"children":[1]}},{{"name":"Root","children":[2]}},{{"name":"Child","translation":[0,1,0]}},{{"name":"Mesh","mesh":0,"skin":0}}],
"skins":[{{"joints":[1,2]}}],"scenes":[{{"nodes":[0,3]}}],"scene":0)",
        pos, jnt, wgt);
    write_text(dir / "rig.gltf", make_gltf(buf, body));
    ImportSettings settings;
    settings.content_root = dir.path();
    auto r = import_gltf(dir / "rig.gltf", settings);
    REQUIRE_MESSAGE(r.has_value(), r.error().message);
    const SkeletonData& s = only_skeleton(*r).data;
    REQUIRE(s.joint_names.size() == 3);
    CHECK(s.joint_names == std::vector<String>{ "Armature", "Root", "Child" });
    CHECK(s.parents == std::vector<i32>{ -1, 0, 1 });
    CHECK(near(s.bind_local[0].position, { 0, 0, 5 }));
    CHECK(near(Vec3(s.inverse_bind[0][3]), { 0, 0, -5 }));
    CHECK(near(Vec3(s.inverse_bind[1][3]), { 0, 0, 0 })); // absent IBMs => identity
    const MeshData& m = r->meshes[0].data;
    CHECK(m.skin[0].joints[0] == 1); // skin joint 0 (Root) -> skeleton joint 1
    CHECK(m.skin[1].joints[0] == 2); // skin joint 1 (Child) -> 2
}

TEST_CASE("import: missing normals, generated indices, strips, flat option") {
    TempDir         dir("normals");
    std::vector<u8> buf;
    // Quad in the XY plane, no NORMAL, with UVs and u16 indices.
    const usize pos = append_f32(buf, { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0 });
    const usize uv = append_f32(buf, { 0, 1, 1, 1, 1, 0, 0, 0 });
    const usize idx = append_u16(buf, { 0, 1, 2, 0, 2, 3 });
    const auto  views = std::format(
        R"("bufferViews":[{{"buffer":0,"byteOffset":{},"byteLength":48}},{{"buffer":0,"byteOffset":{},"byteLength":32}},{{"buffer":0,"byteOffset":{},"byteLength":12}}],
"accessors":[{{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}},{{"bufferView":1,"componentType":5126,"count":4,"type":"VEC2"}},{{"bufferView":2,"componentType":5123,"count":6,"type":"SCALAR"}}],)",
        pos, uv, idx);
    write_text(dir / "quad.gltf",
               make_gltf(buf, views + R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}])"));
    // A non-indexed triangle strip (0,1,2 then 2,1,3 -> two CCW triangles).
    std::vector<u8> sbuf;
    const usize     spos = append_f32(sbuf, { 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0 });
    write_text(dir / "strip.gltf",
               make_gltf(sbuf, std::format(R"("bufferViews":[{{"buffer":0,"byteOffset":{},"byteLength":48}}],
"accessors":[{{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}}],
"meshes":[{{"primitives":[{{"attributes":{{"POSITION":0}},"mode":5}}]}}])", spos)));

    ImportSettings settings;
    settings.content_root = dir.path();
    auto quad = import_gltf(dir / "quad.gltf", settings);
    REQUIRE_MESSAGE(quad.has_value(), quad.error().message);
    const MeshData& q = quad->meshes[0].data;
    REQUIRE(q.vertices.size() == 4);
    for (const Vertex& v : q.vertices) {
        CHECK(near(v.normal, { 0, 0, 1 }));
        CHECK(near(Vec3(v.tangent), { 1, 0, 0 }));
        CHECK(v.tangent.w == 1.0f);
    }
    CHECK_FALSE(quad->scenes[0].data.nodes[0].materials.empty()); // one slot, default material
    CHECK_FALSE(quad->scenes[0].data.nodes[0].materials[0].is_valid());

    auto strip = import_gltf(dir / "strip.gltf", settings);
    REQUIRE_MESSAGE(strip.has_value(), strip.error().message);
    const MeshData& s = strip->meshes[0].data;
    CHECK(s.indices == std::vector<u32>{ 0, 1, 2, 2, 1, 3 });
    for (const Vertex& v : s.vertices) CHECK(near(v.normal, { 0, 0, 1 })); // both triangles face +Z
    CHECK(strip->scenes.empty()); // no nodes -> no scene; the mesh becomes the primary asset
    CHECK(strip->primary == strip->meshes[0].id);

    settings.normal_generation = NormalGeneration::Flat;
    auto flat = import_gltf(dir / "quad.gltf", settings);
    REQUIRE(flat.has_value());
    CHECK(flat->meshes[0].data.vertices.size() == 6); // un-welded
    CHECK(flat->meshes[0].data.indices == std::vector<u32>{ 0, 1, 2, 3, 4, 5 });
}

TEST_CASE("import: images (png sRGB/linear heuristics, hdr float)") {
    TempDir dir("images");
    const std::vector<u8> px{ 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 10, 20, 30, 40 };
    write_png(dir / "albedo.png", 2, 2, px);
    write_png(dir / "rock_normal.png", 2, 2, px);
    write_png(dir / "brick_N.png", 2, 2, px);

    auto albedo = import_image(dir / "albedo.png");
    REQUIRE_MESSAGE(albedo.has_value(), albedo.error().message);
    CHECK(albedo->format == TextureFormat::RGBA8_SRGB);
    CHECK(albedo->width == 2);
    CHECK(albedo->height == 2);
    CHECK(albedo->mip_levels == 1);
    CHECK(albedo->pixels == px);

    auto normal = import_image(dir / "rock_normal.png");
    REQUIRE(normal.has_value());
    CHECK(normal->format == TextureFormat::RGBA8_UNORM);
    CHECK_FALSE(guess_image_is_srgb(dir / "brick_N.png"));
    CHECK(guess_image_is_srgb(dir / "Brick_Diffuse.png"));

    ImportSettings linear;
    linear.image_color_space = ImageColorSpace::Linear;
    CHECK(import_image(dir / "albedo.png", linear)->format == TextureFormat::RGBA8_UNORM);

    // Radiance HDR -> RGBA32F.
    const f32       hdr[6] = { 1.0f, 0.5f, 2.0f, 4.0f, 0.25f, 1.0f };
    std::vector<u8> encoded;
    auto sink = [](void* ctx, void* data, int size) {
        auto* out = static_cast<std::vector<u8>*>(ctx);
        out->insert(out->end(), static_cast<u8*>(data), static_cast<u8*>(data) + size);
    };
    REQUIRE(stbi_write_hdr_to_func(sink, &encoded, 2, 1, 3, hdr) != 0);
    write_text(dir / "sky.hdr", std::string(encoded.begin(), encoded.end()));
    auto sky = import_image(dir / "sky.hdr");
    REQUIRE_MESSAGE(sky.has_value(), sky.error().message);
    CHECK(sky->format == TextureFormat::RGBA32F);
    REQUIRE(sky->pixels.size() == 2 * 1 * 16);
    f32 texel[8];
    std::memcpy(texel, sky->pixels.data(), sizeof(texel));
    CHECK(texel[0] == doctest::Approx(1.0f));
    CHECK(texel[2] == doctest::Approx(2.0f));
    CHECK(texel[3] == doctest::Approx(1.0f)); // alpha
    CHECK(texel[4] == doctest::Approx(4.0f));

    // import_file wraps a standalone image as a one-texture ImportResult.
    ImportSettings settings;
    settings.content_root = dir.path();
    auto wrapped = import_file(dir / "albedo.png", settings);
    REQUIRE(wrapped.has_value());
    REQUIRE(wrapped->textures.size() == 1);
    CHECK(wrapped->primary == make_asset_id("albedo.png", "texture:0"));
}

TEST_CASE("import: errors are reported, never thrown") {
    TempDir dir("errors");
    auto    missing = import_gltf(dir / "nope.gltf");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);

    write_text(dir / "bad.gltf", "{ this is not json");
    CHECK_FALSE(import_gltf(dir / "bad.gltf").has_value());

    write_text(dir / "bad.png", "not a png at all");
    CHECK_FALSE(import_image(dir / "bad.png").has_value());

    write_text(dir / "model.fbx", "x");
    auto fbx = import_file(dir / "model.fbx");
    REQUIRE_FALSE(fbx.has_value());
    CHECK(fbx.error().code == ErrorCode::Unsupported);
    CHECK(is_supported_source("a/B.GLTF"));
    CHECK(is_supported_source("x.HDR"));
    CHECK_FALSE(is_supported_source("x.bin"));
}

TEST_CASE("import: animation of non-joint nodes becomes a node clip over the scene") {
    TempDir         dir("node_anim");
    std::vector<u8> buf;
    const usize     times = append_f32(buf, { 0.0f, 2.0f });
    const usize     rot = append_f32(buf, { 0, 0, 0, 1, 0, 0.70710678f, 0, 0.70710678f });
    const usize     tr = append_f32(buf, { 0, 0, 0, 0, 3, 0 });
    const std::string body = std::format(
        R"("bufferViews":[{{"buffer":0,"byteOffset":{},"byteLength":8}},{{"buffer":0,"byteOffset":{},"byteLength":32}},{{"buffer":0,"byteOffset":{},"byteLength":24}}],
"accessors":[{{"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[2]}},{{"bufferView":1,"componentType":5126,"count":2,"type":"VEC4"}},{{"bufferView":2,"componentType":5126,"count":2,"type":"VEC3"}}],
"nodes":[{{"name":"Root","children":[1,2]}},{{"name":"Lamp","translation":[0,2,0]}},{{"name":"Spinner","translation":[1,0,0],"children":[3]}},{{"name":"Blade"}},{{"name":"Orphan"}}],
"scenes":[{{"nodes":[0]}}],"scene":0,
"animations":[{{"name":"Spin","samplers":[{{"input":0,"output":1}},{{"input":0,"output":2,"interpolation":"STEP"}},{{"input":0,"output":2}}],
"channels":[{{"sampler":0,"target":{{"node":2,"path":"rotation"}}}},{{"sampler":1,"target":{{"node":1,"path":"translation"}}}},{{"sampler":2,"target":{{"node":4,"path":"translation"}}}}]}}])",
        times, rot, tr);
    write_text(dir / "props.gltf", make_gltf(buf, body));
    ImportSettings settings;
    settings.content_root = dir.path();
    auto r = import_gltf(dir / "props.gltf", settings);
    REQUIRE_MESSAGE(r.has_value(), r.error().message);

    // Scene order: Root(0) Lamp(1) Spinner(2) Blade(3); "Orphan" is not in the default scene.
    const SceneData& scene = r->scenes[0].data;
    REQUIRE(scene.nodes.size() == 4);
    CHECK(scene.nodes[2].name == "Spinner");

    REQUIRE(r->animations.size() == 1);
    const auto& clip = r->animations[0];
    CHECK(clip.key == "node_anim:0");
    CHECK(clip.id == make_asset_id("props.gltf", "node_anim:0"));
    CHECK(clip.data.duration == doctest::Approx(2.0f));
    REQUIRE(clip.data.channels.size() == 2); // Orphan's channel dropped (outside the scene)
    CHECK(clip.data.channels[0].joint == 2);
    CHECK(clip.data.channels[0].path == AnimPath::Rotation);
    CHECK(clip.data.channels[1].joint == 1);
    CHECK(clip.data.channels[1].interpolation == Interpolation::Step);
    bool warned = false;
    for (const String& w : r->warnings) warned |= w.find("outside the default scene") != String::npos;
    CHECK(warned);

    // The node skeleton mirrors the scene: joint i == scene node i.
    const SkeletonData& skel = only_skeleton(*r).data;
    CHECK(clip.data.skeleton == r->skeletons[0].id);
    CHECK(r->skeletons[0].key == "node_skeleton:0");
    REQUIRE(skel.joint_names.size() == scene.nodes.size());
    for (usize i = 0; i < scene.nodes.size(); ++i) {
        CHECK(skel.joint_names[i] == scene.nodes[i].name);
        CHECK(skel.parents[i] == scene.nodes[i].parent);
    }
    CHECK(near(skel.bind_local[2].position, { 1, 0, 0 }));
    CHECK(near(Vec3(skel.inverse_bind[3][3]), { -1, 0, 0 })); // Blade sits at the Spinner's origin

    // Round trip through the cooked format (skeleton validation: parents precede children).
    CHECK(encode_asset(CookedMeta{ clip.id, 0, kImporterVersion }, clip.data).has_value());
    CHECK(encode_asset(CookedMeta{ r->skeletons[0].id, 0, kImporterVersion }, skel).has_value());
}

TEST_CASE("import: cooked glTF textures (roles -> BC7 sRGB / BC7 linear / BC5, full mips)") {
    TempDir               dir("cooked_textures");
    const std::vector<u8> px(8 * 8 * 4, u8{ 128 });
    write_png(dir / "a.png", 8, 8, px);
    write_png(dir / "b.png", 8, 8, px);
    write_png(dir / "c.png", 8, 8, px);
    write_text(dir / "m.gltf", R"({"asset":{"version":"2.0"},
"images":[{"uri":"a.png"},{"uri":"b.png"},{"uri":"c.png"}],
"textures":[{"source":0},{"source":1},{"source":2}],
"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":1}},
"normalTexture":{"index":2}}]})");
    ImportSettings settings = ImportSettings::cooking(); // no content_root: no de-duplication
    auto           r = import_gltf(dir / "m.gltf", settings);
    REQUIRE_MESSAGE(r.has_value(), r.error().message);
    REQUIRE(r->textures.size() == 3);
    CHECK(r->referenced_sources.empty());
    auto find_key = [&](const char* key) -> const TextureData* {
        for (const auto& t : r->textures) {
            if (t.key == key) return &t.data;
        }
        return nullptr;
    };
    const TextureData* base = find_key("texture:0:srgb");
    const TextureData* mr = find_key("texture:1:linear");
    const TextureData* nrm = find_key("texture:2:normal");
    REQUIRE(base);
    REQUIRE(mr);
    REQUIRE(nrm);
    CHECK(base->format == TextureFormat::BC7_SRGB);
    CHECK(mr->format == TextureFormat::BC7_UNORM);
    CHECK(nrm->format == TextureFormat::BC5_UNORM);
    for (const TextureData* t : { base, mr, nrm }) {
        CHECK(t->mip_levels == 4);
        CHECK(t->pixels.size() == texture_byte_size(*t));
    }
    CHECK(r->materials[0].data.normal_texture == make_asset_id("m.gltf", "texture:2:normal"));

    // Standalone images follow their name: *_normal -> BC5, else BC7 sRGB.
    write_png(dir / "rock_normal.png", 8, 8, px);
    CHECK(standalone_image_role(dir / "rock_normal.png", settings) == TextureRole::NormalMap);
    CHECK(standalone_image_role(dir / "a.png", settings) == TextureRole::Color);
    CHECK(standalone_image_role(dir / "wood_roughness.png", settings) == TextureRole::Data);
    CHECK(import_image(dir / "rock_normal.png", settings)->format == TextureFormat::BC5_UNORM);
    CHECK(import_image(dir / "a.png", settings)->format == TextureFormat::BC7_SRGB);
    ImportSettings mips_only;
    mips_only.generate_mips = true;
    auto plain = import_image(dir / "a.png", mips_only);
    REQUIRE(plain.has_value());
    CHECK(plain->format == TextureFormat::RGBA8_SRGB);
    CHECK(plain->mip_levels == 4);
    CHECK(settings.fingerprint() != ImportSettings{}.fingerprint());
    CHECK(mips_only.fingerprint() != ImportSettings{}.fingerprint());
}
