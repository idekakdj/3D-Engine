// gltf_importer.cpp — glTF 2.0 (.gltf / .glb) importer (the one CGLTF_IMPLEMENTATION TU).
//
// Mapping (see importers.h for sub-asset keys):
//   mesh        -> MeshData; each triangle primitive becomes a Submesh; primitives that use
//                  the same material share a material slot (slot order = first use).
//   material    -> MaterialData (metallic-roughness; spec-gloss approximated).
//   image       -> TextureData, once per (image, role) actually referenced; cooked (mips, BC7/BC5)
//                  per ImportSettings; external images shared with a standalone import are
//                  referenced, not copied (see importers.h).
//   skin        -> SkeletonData. Joints are topologically sorted (parents first) with a
//                  stable order (an already-sorted skin keeps its order). Non-joint nodes
//                  BETWEEN joints are included as extra joints (identity-free hierarchy), and
//                  if the transform from the skinned mesh node to a root joint's parent is
//                  not identity, a synthetic root joint carrying it is inserted, so that
//                  "skeleton model space" == the skinned mesh node's local space. The same
//                  remap is applied to SkinVertex joint indices and animation channels.
//   animation   -> AnimationClipData bound to the skeleton most of its channels target;
//                  channels on non-joint nodes -> "node_anim:<a>" over "node_skeleton:<s>".
//   scene       -> SceneData (depth-first, parents before children).
#include "aether/assets/importers.h"
#include "aether/assets/mesh_processing.h"
#include "aether/assets/texture_processing.h"

#include "file_util.h"
#include "math_util.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

namespace fs = std::filesystem;

namespace aether::assets {

namespace {

// ---------------------------------------------------------------------------
// cgltf plumbing
// ---------------------------------------------------------------------------
struct FileContext {
    std::vector<fs::path> dependencies;
};

cgltf_result read_file_cb(const cgltf_memory_options* memory,
                          const cgltf_file_options*   file,
                          const char*                 path,
                          cgltf_size*                 size,
                          void**                      data) {
    auto*          ctx = static_cast<FileContext*>(file->user_data);
    const fs::path p = detail::from_utf8(path);
    auto           bytes = detail::read_file_bytes(p);
    if (!bytes) {
        return bytes.error().is(ErrorCode::NotFound) ? cgltf_result_file_not_found : cgltf_result_io_error;
    }
    if (size && *size != 0 && bytes->size() < *size) return cgltf_result_data_too_short;
    const usize n = std::max<usize>(bytes->size(), 1);
    void*       out = memory->alloc_func ? memory->alloc_func(memory->user_data, n) : std::malloc(n);
    if (!out) return cgltf_result_out_of_memory;
    if (!bytes->empty()) std::memcpy(out, bytes->data(), bytes->size());
    if (size) *size = bytes->size();
    *data = out;
    if (ctx) ctx->dependencies.push_back(p);
    return cgltf_result_success;
}

void release_file_cb(const cgltf_memory_options* memory, const cgltf_file_options*, void* data) {
    if (memory->free_func) {
        memory->free_func(memory->user_data, data);
    } else {
        std::free(data);
    }
}

struct CgltfDeleter {
    void operator()(cgltf_data* d) const noexcept { cgltf_free(d); }
};

const char* cgltf_result_name(cgltf_result r) {
    switch (r) {
    case cgltf_result_success: return "success";
    case cgltf_result_data_too_short: return "data too short / out-of-range accessor";
    case cgltf_result_unknown_format: return "unknown format";
    case cgltf_result_invalid_json: return "invalid JSON";
    case cgltf_result_invalid_gltf: return "invalid glTF";
    case cgltf_result_invalid_options: return "invalid options";
    case cgltf_result_file_not_found: return "file not found";
    case cgltf_result_io_error: return "I/O error";
    case cgltf_result_out_of_memory: return "out of memory";
    case cgltf_result_legacy_gltf: return "legacy glTF 1.0 is not supported";
    default: return "unknown error";
    }
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
bool read_floats(const cgltf_accessor* acc, usize components, std::vector<f32>& out) {
    if (!acc || cgltf_num_components(acc->type) != components) return false;
    out.assign(acc->count * components, 0.0f);
    if (acc->count == 0) return true;
    return cgltf_accessor_unpack_floats(acc, out.data(), out.size()) == out.size();
}

Transform node_local_transform(const cgltf_node& node) {
    if (node.has_matrix) return detail::decompose(detail::mat4_from_floats(node.matrix));
    Transform t;
    t.position = Vec3(node.translation[0], node.translation[1], node.translation[2]);
    t.rotation = detail::normalize_quat(
        detail::quat_xyzw(node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]));
    t.scale = Vec3(node.scale[0], node.scale[1], node.scale[2]);
    return t;
}

Mat4 node_world_matrix(const cgltf_node* node) {
    if (!node) return Mat4(1.0f);
    f32 m[16];
    cgltf_node_transform_world(node, m);
    return detail::mat4_from_floats(m);
}

bool base64_decode(StringView in, std::vector<u8>& out) {
    out.clear();
    out.reserve(in.size() / 4 * 3);
    u32 acc = 0;
    int bits = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        else return false;
        acc = (acc << 6) | static_cast<u32>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((acc >> bits) & 0xFFu));
        }
    }
    return true;
}

String node_display_name(const cgltf_data* data, const cgltf_node* node) {
    if (node->name && node->name[0]) return node->name;
    return std::format("Node_{}", cgltf_node_index(data, node));
}

constexpr StringView kUnsupportedRequiredExtensions[] = {
    "KHR_draco_mesh_compression", "EXT_meshopt_compression", "KHR_meshopt_compression",
    "KHR_texture_basisu",         "EXT_texture_webp",        "EXT_texture_avif",
};

// ---------------------------------------------------------------------------
// The importer proper (one instance per import call)
// ---------------------------------------------------------------------------
class GltfImporter {
public:
    GltfImporter(const cgltf_data* data, const fs::path& path, const ImportSettings& settings,
                 FileContext& files, ImportResult& result)
        : data_(data), path_(path), settings_(settings), files_(files), out_(result) {}

    Result<void> run() {
        out_.source_path = canonical_source_path(path_, settings_.content_root);
        import_materials();
        import_skins();
        if (auto r = import_meshes(); !r) return r;
        import_animations();
        import_scenes();
        return {};
    }

private:
    template <typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) {
        out_.warnings.push_back(std::format(fmt, std::forward<Args>(args)...));
    }

    AssetId id_for(StringView key) const { return make_asset_id(out_.source_path, key); }

    // ---------------------------------------------------------------- textures
    bool image_bytes(const cgltf_image& image, std::vector<u8>& storage, ByteSpan& bytes) {
        if (image.buffer_view) {
            const u8* p = cgltf_buffer_view_data(image.buffer_view);
            if (!p) return false;
            bytes = ByteSpan(reinterpret_cast<const byte*>(p), image.buffer_view->size);
            return true;
        }
        if (!image.uri) return false;
        const StringView uri(image.uri);
        if (uri.starts_with("data:")) {
            const usize comma = uri.find(',');
            if (comma == StringView::npos || uri.substr(0, comma).find(";base64") == StringView::npos) return false;
            if (!base64_decode(uri.substr(comma + 1), storage)) return false;
            bytes = detail::bytes_of(storage);
            return true;
        }
        if (uri.find("://") != StringView::npos) return false; // remote URIs are not fetched
        String decoded(uri);
        cgltf_decode_uri(decoded.data());
        decoded.resize(std::strlen(decoded.c_str()));
        const fs::path file = path_.parent_path() / detail::from_utf8(decoded);
        auto           read = detail::read_file_bytes(file);
        if (!read) return false;
        files_.dependencies.push_back(file);
        storage = std::move(*read);
        bytes = detail::bytes_of(storage);
        return true;
    }

    String image_name(const cgltf_image& image, usize index) const {
        if (image.name && image.name[0]) return image.name;
        if (image.uri && !StringView(image.uri).starts_with("data:")) {
            return detail::to_utf8(detail::from_utf8(image.uri).stem());
        }
        return std::format("Image_{}", index);
    }

    static StringView role_suffix(TextureRole role) {
        switch (role) {
        case TextureRole::Color: return "srgb";
        case TextureRole::NormalMap: return "normal";
        case TextureRole::Data: break;
        }
        return "linear";
    }

    // De-duplication (importers.h): the standalone-image asset of an external image file inside
    // the content root when its standalone import yields the same variant, else nothing.
    AssetId shared_standalone_texture(const cgltf_image& image, TextureRole role) {
        if (settings_.content_root.empty() || image.buffer_view || !image.uri) return {};
        const StringView uri(image.uri);
        if (uri.starts_with("data:") || uri.find("://") != StringView::npos) return {};
        String decoded(uri);
        cgltf_decode_uri(decoded.data());
        decoded.resize(std::strlen(decoded.c_str()));
        const fs::path  file = path_.parent_path() / detail::from_utf8(decoded);
        std::error_code ec;
        if (!is_image_source(file) || !fs::is_regular_file(file, ec)) return {};
        const String rel = canonical_source_path(file, settings_.content_root);
        if (rel.empty() || detail::from_utf8(rel).is_absolute() || rel.starts_with("..")) return {};
        if (standalone_image_role(file, settings_) != role) return {};
        if (std::find(out_.referenced_sources.begin(), out_.referenced_sources.end(), file) ==
            out_.referenced_sources.end()) {
            out_.referenced_sources.push_back(file);
        }
        return make_asset_id(rel, "texture:0");
    }

    AssetId texture_for(const cgltf_texture_view& view, TextureRole role, StringView material_name, StringView slot) {
        if (!view.texture) return {};
        const cgltf_image* image = view.texture->image;
        if (!image) {
            warn("material '{}' {}: texture has no supported image source (basisu/webp not supported)",
                 material_name, slot);
            return {};
        }
        if (view.texcoord != 0) {
            warn("material '{}' {}: uses TEXCOORD_{}; only TEXCOORD_0 is imported", material_name, slot,
                 view.texcoord);
        }
        if (view.has_transform) warn("material '{}' {}: KHR_texture_transform is ignored", material_name, slot);

        const usize image_index = cgltf_image_index(data_, image);
        const u64   cache_key = (static_cast<u64>(image_index) << 2) | static_cast<u64>(role);
        if (auto it = texture_cache_.find(cache_key); it != texture_cache_.end()) return it->second;

        if (const AssetId shared = shared_standalone_texture(*image, role); shared.is_valid()) {
            texture_cache_[cache_key] = shared;
            return shared;
        }

        const String  key = std::format("texture:{}:{}", image_index, role_suffix(role));
        const AssetId id = id_for(key);
        if (!settings_.import_textures) {
            texture_cache_[cache_key] = id; // ids stay deterministic even without pixel data
            return id;
        }
        const String    name = image_name(*image, image_index);
        std::vector<u8> storage;
        ByteSpan        bytes;
        if (!image_bytes(*image, storage, bytes)) {
            warn("image '{}': cannot read image data", name);
            texture_cache_[cache_key] = AssetId{};
            return {};
        }
        auto tex = decode_image(bytes, role == TextureRole::Color, name);
        if (!tex) {
            warn("image '{}': {}", name, tex.error().message);
            texture_cache_[cache_key] = AssetId{};
            return {};
        }
        if (settings_.generate_mips || settings_.compress_textures) {
            const TextureCookOptions options{ true, settings_.compress_textures, settings_.mip_filter };
            if (auto r = cook_texture(*tex, role, options); !r) {
                warn("image '{}': texture cooking failed ({}); imported uncooked", name, r.error().message);
            }
        }
        out_.textures.push_back({ id, key, name, std::move(*tex) });
        texture_cache_[cache_key] = id;
        return id;
    }

    // --------------------------------------------------------------- materials
    void import_materials() {
        material_ids_.resize(data_->materials_count);
        for (usize mi = 0; mi < data_->materials_count; ++mi) {
            const cgltf_material& m = data_->materials[mi];
            MaterialData          md;
            md.name = (m.name && m.name[0]) ? String(m.name) : std::format("Material_{}", mi);

            if (m.has_pbr_metallic_roughness || !m.has_pbr_specular_glossiness) {
                const cgltf_pbr_metallic_roughness& pbr = m.pbr_metallic_roughness;
                md.base_color_factor = Vec4(pbr.base_color_factor[0], pbr.base_color_factor[1],
                                            pbr.base_color_factor[2], pbr.base_color_factor[3]);
                md.metallic_factor = pbr.metallic_factor;
                md.roughness_factor = pbr.roughness_factor;
                md.base_color_texture = texture_for(pbr.base_color_texture, TextureRole::Color, md.name, "baseColor");
                md.metallic_roughness_texture =
                    texture_for(pbr.metallic_roughness_texture, TextureRole::Data, md.name, "metallicRoughness");
            } else {
                const cgltf_pbr_specular_glossiness& sg = m.pbr_specular_glossiness;
                warn("material '{}': KHR_materials_pbrSpecularGlossiness approximated as metallic-roughness",
                     md.name);
                md.base_color_factor = Vec4(sg.diffuse_factor[0], sg.diffuse_factor[1], sg.diffuse_factor[2],
                                            sg.diffuse_factor[3]);
                md.metallic_factor = 0.0f;
                md.roughness_factor = 1.0f - sg.glossiness_factor;
                md.base_color_texture = texture_for(sg.diffuse_texture, TextureRole::Color, md.name, "diffuse");
            }
            md.normal_texture = texture_for(m.normal_texture, TextureRole::NormalMap, md.name, "normal");
            md.normal_scale = m.normal_texture.texture ? m.normal_texture.scale : 1.0f;
            md.occlusion_texture = texture_for(m.occlusion_texture, TextureRole::Data, md.name, "occlusion");
            md.occlusion_strength = m.occlusion_texture.texture ? m.occlusion_texture.scale : 1.0f;
            md.emissive_texture = texture_for(m.emissive_texture, TextureRole::Color, md.name, "emissive");
            const f32 emissive_strength = m.has_emissive_strength ? m.emissive_strength.emissive_strength : 1.0f;
            md.emissive_factor = Vec3(m.emissive_factor[0], m.emissive_factor[1], m.emissive_factor[2]) *
                                 emissive_strength;
            md.alpha_cutoff = m.alpha_cutoff;
            md.double_sided = m.double_sided != 0;
            switch (m.alpha_mode) {
            case cgltf_alpha_mode_mask: md.alpha_mode = AlphaMode::Mask; break;
            case cgltf_alpha_mode_blend: md.alpha_mode = AlphaMode::Blend; break;
            default: md.alpha_mode = AlphaMode::Opaque; break;
            }
            if (m.unlit) warn("material '{}': KHR_materials_unlit is imported as a lit material", md.name);

            const String key = std::format("material:{}", mi);
            material_ids_[mi] = id_for(key);
            String name = md.name;
            out_.materials.push_back({ material_ids_[mi], key, std::move(name), std::move(md) });
        }
    }

    // ------------------------------------------------------------------- skins
    struct SkinInfo {
        AssetId                                     id;
        std::vector<u16>                            remap; // skin joint index -> skeleton joint
        std::unordered_map<const cgltf_node*, u32> node_to_joint;
    };

    void import_skins() {
        skins_.resize(data_->skins_count);
        for (usize si = 0; si < data_->skins_count; ++si) import_skin(si);
    }

    void import_skin(usize si) {
        const cgltf_skin& skin = data_->skins[si];
        const String      skin_name = (skin.name && skin.name[0]) ? String(skin.name) : std::format("Skeleton_{}", si);
        const usize       joint_count = skin.joints_count;
        if (joint_count == 0 || joint_count > 0xFFFF) {
            warn("skin '{}': {} joints (must be 1..65535); skipped", skin_name, joint_count);
            return;
        }

        std::unordered_map<const cgltf_node*, usize> skin_index; // node -> skin joint index
        for (usize k = 0; k < joint_count; ++k) {
            if (!skin.joints[k] || !skin_index.emplace(skin.joints[k], k).second) {
                warn("skin '{}': null or duplicate joint {}; skipped", skin_name, k);
                return;
            }
        }

        // Non-joint nodes lying between two joints become joints too.
        std::unordered_set<const cgltf_node*> in_skeleton;
        for (usize k = 0; k < joint_count; ++k) in_skeleton.insert(skin.joints[k]);
        std::vector<const cgltf_node*> intermediates;
        for (usize k = 0; k < joint_count; ++k) {
            std::vector<const cgltf_node*> path;
            const cgltf_node*              p = skin.joints[k]->parent;
            while (p && !skin_index.contains(p)) {
                path.push_back(p);
                p = p->parent;
            }
            if (!p) continue; // root joint: the non-joint chain above it is handled below
            for (const cgltf_node* n : path) {
                if (in_skeleton.insert(n).second) intermediates.push_back(n);
            }
        }
        std::sort(intermediates.begin(), intermediates.end(), [&](const cgltf_node* a, const cgltf_node* b) {
            return cgltf_node_index(data_, a) < cgltf_node_index(data_, b);
        });

        // Skeleton model space = local space of the (first) node skinned by this skin.
        const cgltf_node* mesh_node = nullptr;
        for (usize ni = 0; ni < data_->nodes_count && !mesh_node; ++ni) {
            if (data_->nodes[ni].skin == &skin) mesh_node = &data_->nodes[ni];
        }
        const Mat4 inv_mesh_world = detail::inverse(node_world_matrix(mesh_node));

        struct SkelNode {
            const cgltf_node* node = nullptr;          // null for synthetic roots
            const cgltf_node* synthetic_for = nullptr; // glTF parent represented by a synthetic root
            i32               parent = -1;             // index into `list`
            u64               order_key = 0;
            Transform         local;
            Mat4              offset{ 1.0f };          // synthetic: mesh-space transform of synthetic_for
            bool              has_ibm = false;
            Mat4              ibm{ 1.0f };
            String            name;
        };
        std::vector<SkelNode> list;

        auto nearest_skeleton_ancestor = [&](const cgltf_node* n) -> const cgltf_node* {
            const cgltf_node* p = n->parent;
            while (p && !in_skeleton.contains(p)) p = p->parent;
            return p;
        };

        // Synthetic roots (inserted first so they sort first).
        std::unordered_map<const cgltf_node*, i32> synthetic_of_parent;
        auto consider_root = [&](const cgltf_node* n) {
            if (nearest_skeleton_ancestor(n)) return;
            const cgltf_node* parent = n->parent;
            if (synthetic_of_parent.contains(parent)) return;
            const Mat4 offset = inv_mesh_world * node_world_matrix(parent);
            if (detail::approx_identity(offset)) {
                synthetic_of_parent[parent] = -1;
                return;
            }
            SkelNode s;
            s.synthetic_for = parent;
            s.offset = offset;
            s.local = detail::decompose(offset);
            s.name = parent ? node_display_name(data_, parent) : String("__skin_root");
            s.order_key = list.size();
            synthetic_of_parent[parent] = static_cast<i32>(list.size());
            list.push_back(std::move(s));
        };
        for (usize k = 0; k < joint_count; ++k) consider_root(skin.joints[k]);
        for (const cgltf_node* n : intermediates) consider_root(n);
        const usize synthetic_count = list.size();

        // Skin joints (skin order), then intermediates.
        std::vector<f32> ibm_floats;
        const bool       have_ibms = skin.inverse_bind_matrices &&
                               skin.inverse_bind_matrices->count >= joint_count &&
                               read_floats(skin.inverse_bind_matrices, 16, ibm_floats);
        if (skin.inverse_bind_matrices && !have_ibms) {
            warn("skin '{}': unreadable inverseBindMatrices; using identity", skin_name);
        }
        for (usize k = 0; k < joint_count; ++k) {
            SkelNode j;
            j.node = skin.joints[k];
            j.local = node_local_transform(*j.node);
            j.name = node_display_name(data_, j.node);
            j.order_key = synthetic_count + k;
            j.has_ibm = true;
            j.ibm = have_ibms ? detail::mat4_from_floats(&ibm_floats[k * 16]) : Mat4(1.0f);
            list.push_back(std::move(j));
        }
        for (usize k = 0; k < intermediates.size(); ++k) {
            SkelNode j;
            j.node = intermediates[k];
            j.local = node_local_transform(*j.node);
            j.name = node_display_name(data_, j.node);
            j.order_key = synthetic_count + joint_count + k;
            list.push_back(std::move(j));
        }

        // Resolve parents.
        std::unordered_map<const cgltf_node*, i32> list_index;
        for (usize i = 0; i < list.size(); ++i) {
            if (list[i].node) list_index[list[i].node] = static_cast<i32>(i);
        }
        for (SkelNode& n : list) {
            if (!n.node) continue;
            if (const cgltf_node* a = nearest_skeleton_ancestor(n.node)) {
                n.parent = list_index.at(a);
            } else if (auto it = synthetic_of_parent.find(n.node->parent); it != synthetic_of_parent.end()) {
                n.parent = it->second;
            }
        }

        // Stable topological sort (Kahn, smallest order_key first).
        std::vector<std::vector<u32>> children(list.size());
        using QueueItem = std::pair<u64, u32>;
        std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<>> ready;
        for (u32 i = 0; i < list.size(); ++i) {
            if (list[i].parent >= 0) {
                children[static_cast<usize>(list[i].parent)].push_back(i);
            } else {
                ready.emplace(list[i].order_key, i);
            }
        }
        std::vector<u32> order;
        order.reserve(list.size());
        while (!ready.empty()) {
            const u32 i = ready.top().second;
            ready.pop();
            order.push_back(i);
            for (u32 c : children[i]) ready.emplace(list[c].order_key, c);
        }
        if (order.size() != list.size()) {
            warn("skin '{}': joint hierarchy is cyclic; skipped", skin_name);
            return;
        }
        std::vector<i32> new_index(list.size(), -1);
        for (u32 pos = 0; pos < order.size(); ++pos) new_index[order[pos]] = static_cast<i32>(pos);

        SkeletonData skel;
        const usize  n = list.size();
        skel.joint_names.resize(n);
        skel.parents.resize(n);
        skel.bind_local.resize(n);
        skel.inverse_bind.resize(n);
        std::vector<Mat4> model(n, Mat4(1.0f));
        for (usize pos = 0; pos < n; ++pos) {
            const SkelNode& src = list[order[pos]];
            const i32       parent = src.parent >= 0 ? new_index[static_cast<usize>(src.parent)] : -1;
            skel.joint_names[pos] = src.name;
            skel.parents[pos] = parent;
            skel.bind_local[pos] = src.local;
            model[pos] = src.node ? (parent >= 0 ? model[static_cast<usize>(parent)] : Mat4(1.0f)) *
                                        src.local.to_matrix()
                                  : src.offset;
            skel.inverse_bind[pos] = src.has_ibm ? src.ibm : detail::inverse(model[pos]);
        }

        SkinInfo& info = skins_[si];
        info.remap.resize(joint_count);
        for (usize k = 0; k < joint_count; ++k) {
            info.remap[k] = static_cast<u16>(new_index[synthetic_count + k]);
        }
        for (usize i = 0; i < list.size(); ++i) {
            if (list[i].node) info.node_to_joint[list[i].node] = static_cast<u32>(new_index[i]);
        }
        if (synthetic_count > 0) {
            warn("skin '{}': inserted {} synthetic root joint(s) for non-identity transforms above the "
                 "root joint(s)", skin_name, synthetic_count);
        }
        const String key = std::format("skeleton:{}", si);
        info.id = id_for(key);
        out_.skeletons.push_back({ info.id, key, skin_name, std::move(skel) });
    }

    // ------------------------------------------------------------------ meshes
    struct Primitive {
        std::vector<Vertex>     vertices;
        std::vector<SkinVertex> skin;
        std::vector<u32>        indices;
    };

    // Reads JOINTS_n/WEIGHTS_n (n = 0, 1) into up to 8 influences, keeps the 4 largest,
    // remaps to skeleton joints and normalises. Returns false if the data is unusable.
    bool read_skin(const cgltf_primitive& prim, usize count, const SkinInfo& skin, StringView mesh_name,
                   std::vector<SkinVertex>& out) {
        std::vector<f32> joints[2], weights[2];
        int              sets = 0;
        for (int s = 0; s < 2; ++s) {
            const cgltf_accessor* j = cgltf_find_accessor(&prim, cgltf_attribute_type_joints, s);
            const cgltf_accessor* w = cgltf_find_accessor(&prim, cgltf_attribute_type_weights, s);
            if (!j || !w) break;
            if (j->count != count || w->count != count || !read_floats(j, 4, joints[s]) ||
                !read_floats(w, 4, weights[s])) {
                warn("mesh '{}': unreadable JOINTS_{}/WEIGHTS_{}", mesh_name, s, s);
                return false;
            }
            ++sets;
        }
        if (sets == 0) return false;
        if (sets > 1) warn("mesh '{}': more than 4 influences per vertex; keeping the 4 largest", mesh_name);

        out.resize(count);
        bool warned_range = false, warned_zero = false;
        for (usize v = 0; v < count; ++v) {
            std::pair<f32, u32> inf[8];
            usize               used = 0;
            for (int s = 0; s < sets; ++s) {
                for (usize c = 0; c < 4; ++c) {
                    f32       w = weights[s][v * 4 + c];
                    const f32 jf = joints[s][v * 4 + c];
                    u32       j = 0;
                    if (!(jf >= 0.0f) || jf >= static_cast<f32>(skin.remap.size())) {
                        if (w > 0.0f && !warned_range) {
                            warn("mesh '{}': joint index out of range for its skin; weight dropped", mesh_name);
                            warned_range = true;
                        }
                        w = 0.0f;
                    } else {
                        j = skin.remap[static_cast<usize>(jf + 0.5f)];
                    }
                    inf[used++] = { std::isfinite(w) && w > 0.0f ? w : 0.0f, j };
                }
            }
            std::stable_sort(inf, inf + used, [](const auto& a, const auto& b) { return a.first > b.first; });
            SkinVertex sv;
            for (usize c = 0; c < 4; ++c) {
                sv.joints[c] = static_cast<u16>(inf[c].second);
                sv.weights[static_cast<int>(c)] = inf[c].first;
            }
            if (!normalize_skin_weights(sv) && !warned_zero) {
                warn("mesh '{}': vertex with zero skin weights bound fully to its first joint", mesh_name);
                warned_zero = true;
            }
            out[v] = sv;
        }
        return true;
    }

    // Returns false (with a warning) for primitives that are skipped.
    bool read_primitive(const cgltf_primitive& prim, StringView mesh_name, usize prim_index,
                        const SkinInfo* skin, Primitive& out) {
        if (prim.has_draco_mesh_compression) {
            warn("mesh '{}' primitive {}: Draco compression is not supported; skipped", mesh_name, prim_index);
            return false;
        }
        const cgltf_accessor* pos = cgltf_find_accessor(&prim, cgltf_attribute_type_position, 0);
        std::vector<f32>      positions;
        if (!pos || !read_floats(pos, 3, positions)) {
            warn("mesh '{}' primitive {}: missing or unreadable POSITION; skipped", mesh_name, prim_index);
            return false;
        }
        const usize count = pos->count;
        if (count == 0 || count > 0xFFFFFFFFull) {
            warn("mesh '{}' primitive {}: bad vertex count; skipped", mesh_name, prim_index);
            return false;
        }

        // ---- indices (triangulated) ----
        std::vector<u32> raw;
        if (prim.indices) {
            raw.resize(prim.indices->count);
            bool ok = false;
            if (!prim.indices->is_sparse && prim.indices->buffer_view) {
                ok = cgltf_accessor_unpack_indices(prim.indices, raw.data(), sizeof(u32), raw.size()) == raw.size();
            } else {
                std::vector<f32> f;
                ok = read_floats(prim.indices, 1, f);
                for (usize i = 0; ok && i < f.size(); ++i) raw[i] = static_cast<u32>(f[i]);
            }
            if (!ok) {
                warn("mesh '{}' primitive {}: unreadable indices; skipped", mesh_name, prim_index);
                return false;
            }
        } else {
            raw.resize(count);
            for (usize i = 0; i < count; ++i) raw[i] = static_cast<u32>(i);
        }
        for (u32 idx : raw) {
            if (idx >= count) {
                warn("mesh '{}' primitive {}: index {} out of range; skipped", mesh_name, prim_index, idx);
                return false;
            }
        }
        switch (prim.type) {
        case cgltf_primitive_type_triangles:
            out.indices = std::move(raw);
            if (out.indices.size() % 3 != 0) {
                warn("mesh '{}' primitive {}: index count not a multiple of 3; truncated", mesh_name, prim_index);
                out.indices.resize(out.indices.size() / 3 * 3);
            }
            break;
        case cgltf_primitive_type_triangle_strip: out.indices = triangle_strip_to_list(raw); break;
        case cgltf_primitive_type_triangle_fan: out.indices = triangle_fan_to_list(raw); break;
        default:
            warn("mesh '{}' primitive {}: point/line primitives are not supported; skipped", mesh_name, prim_index);
            return false;
        }
        if (out.indices.empty()) {
            warn("mesh '{}' primitive {}: no triangles; skipped", mesh_name, prim_index);
            return false;
        }

        // ---- attributes ----
        out.vertices.resize(count);
        for (usize i = 0; i < count; ++i) {
            out.vertices[i].position = Vec3(positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]);
        }
        std::vector<f32> tmp;
        const cgltf_accessor* nrm = cgltf_find_accessor(&prim, cgltf_attribute_type_normal, 0);
        const bool has_normals = nrm && nrm->count == count && read_floats(nrm, 3, tmp);
        if (has_normals) {
            for (usize i = 0; i < count; ++i) {
                out.vertices[i].normal =
                    detail::normalize_or(Vec3(tmp[i * 3], tmp[i * 3 + 1], tmp[i * 3 + 2]), Vec3(0.0f, 1.0f, 0.0f));
            }
        }
        const cgltf_accessor* uv = cgltf_find_accessor(&prim, cgltf_attribute_type_texcoord, 0);
        if (uv && uv->count == count && read_floats(uv, 2, tmp)) {
            for (usize i = 0; i < count; ++i) out.vertices[i].uv0 = Vec2(tmp[i * 2], tmp[i * 2 + 1]);
        }
        // glTF: when normals are absent, provided tangents MUST be ignored.
        const cgltf_accessor* tan = cgltf_find_accessor(&prim, cgltf_attribute_type_tangent, 0);
        const bool has_tangents = has_normals && tan && tan->count == count && read_floats(tan, 4, tmp);
        if (has_tangents) {
            for (usize i = 0; i < count; ++i) {
                const Vec3 t = detail::normalize_or(Vec3(tmp[i * 4], tmp[i * 4 + 1], tmp[i * 4 + 2]),
                                                    detail::any_perpendicular(out.vertices[i].normal));
                out.vertices[i].tangent = Vec4(t, tmp[i * 4 + 3] < 0.0f ? -1.0f : 1.0f);
            }
        }

        if (skin) {
            if (!read_skin(prim, count, *skin, mesh_name, out.skin)) {
                warn("mesh '{}' primitive {}: skinned mesh primitive without JOINTS_0/WEIGHTS_0; bound to "
                     "the first joint", mesh_name, prim_index);
                SkinVertex sv;
                sv.joints[0] = skin->remap.empty() ? u16{ 0 } : skin->remap[0];
                out.skin.assign(count, sv);
            }
        }

        // ---- derived data ----
        if (!has_normals) {
            if (settings_.generate_normals) {
                if (settings_.normal_generation == NormalGeneration::Flat) {
                    generate_flat_normals(out.vertices, out.indices, skin ? &out.skin : nullptr);
                } else {
                    generate_smooth_normals(out.vertices, out.indices);
                }
            } else {
                warn("mesh '{}' primitive {}: no normals and generation disabled", mesh_name, prim_index);
            }
        }
        if (!has_tangents && settings_.generate_tangents) {
            generate_tangents_mikktspace(out.vertices, out.indices, skin ? &out.skin : nullptr);
        }
        if (prim.targets_count > 0) {
            warn("mesh '{}' primitive {}: morph targets are not supported (ignored)", mesh_name, prim_index);
        }
        return true;
    }

    Result<void> import_meshes() {
        // Skin binding per mesh: the first node that instantiates the mesh with a skin.
        std::vector<i32> mesh_skin(data_->meshes_count, -1);
        for (usize ni = 0; ni < data_->nodes_count; ++ni) {
            const cgltf_node& node = data_->nodes[ni];
            if (!node.mesh || !node.skin) continue;
            const usize mi = cgltf_mesh_index(data_, node.mesh);
            const i32   si = static_cast<i32>(cgltf_skin_index(data_, node.skin));
            if (mesh_skin[mi] < 0) {
                mesh_skin[mi] = si;
            } else if (mesh_skin[mi] != si) {
                warn("mesh {} is instanced with several skins; it is bound to skin {}", mi, mesh_skin[mi]);
            }
        }

        mesh_ids_.assign(data_->meshes_count, AssetId{});
        mesh_slots_.resize(data_->meshes_count);
        for (usize mi = 0; mi < data_->meshes_count; ++mi) {
            const cgltf_mesh& mesh = data_->meshes[mi];
            const String      name = (mesh.name && mesh.name[0]) ? String(mesh.name) : std::format("Mesh_{}", mi);
            const SkinInfo*   skin = nullptr;
            if (mesh_skin[mi] >= 0 && skins_[static_cast<usize>(mesh_skin[mi])].id.is_valid()) {
                skin = &skins_[static_cast<usize>(mesh_skin[mi])];
            } else if (mesh_skin[mi] < 0) {
                for (usize p = 0; p < mesh.primitives_count; ++p) {
                    if (cgltf_find_accessor(&mesh.primitives[p], cgltf_attribute_type_joints, 0)) {
                        warn("mesh '{}': has JOINTS_0 but no node binds it to a skin; skin data dropped", name);
                        break;
                    }
                }
            }

            MeshData                            md;
            std::vector<const cgltf_material*>  slots;
            for (usize p = 0; p < mesh.primitives_count; ++p) {
                Primitive prim;
                if (!read_primitive(mesh.primitives[p], name, p, skin, prim)) continue;
                const u64 new_vertex_count = md.vertices.size() + prim.vertices.size();
                if (new_vertex_count > 0xFFFFFFFFull) {
                    return Error{ ErrorCode::Unsupported, std::format("mesh '{}' exceeds 2^32 vertices", name) };
                }
                const u32 base = static_cast<u32>(md.vertices.size());
                Submesh   sub;
                sub.first_index = static_cast<u32>(md.indices.size());
                sub.index_count = static_cast<u32>(prim.indices.size());
                auto slot_it = std::find(slots.begin(), slots.end(), mesh.primitives[p].material);
                sub.material_slot = static_cast<u32>(slot_it - slots.begin());
                if (slot_it == slots.end()) slots.push_back(mesh.primitives[p].material);
                sub.bounds = compute_bounds(prim.vertices);
                for (u32 idx : prim.indices) md.indices.push_back(base + idx);
                md.vertices.insert(md.vertices.end(), prim.vertices.begin(), prim.vertices.end());
                if (skin) md.skin.insert(md.skin.end(), prim.skin.begin(), prim.skin.end());
                md.submeshes.push_back(sub);
            }
            if (md.submeshes.empty()) {
                warn("mesh '{}': no importable triangle primitives; skipped", name);
                continue;
            }
            md.bounds = compute_bounds(md.vertices);
            if (skin) md.skeleton = skin->id;

            for (const cgltf_material* m : slots) {
                mesh_slots_[mi].push_back(m ? material_ids_[cgltf_material_index(data_, m)] : AssetId{});
            }
            const String key = std::format("mesh:{}", mi);
            mesh_ids_[mi] = id_for(key);
            out_.meshes.push_back({ mesh_ids_[mi], key, name, std::move(md) });
        }
        return {};
    }

    // -------------------------------------------------------------- animations
    void import_animations() {
        for (usize ai = 0; ai < data_->animations_count; ++ai) {
            const cgltf_animation& anim = data_->animations[ai];
            const String name = (anim.name && anim.name[0]) ? String(anim.name) : std::format("Animation_{}", ai);

            // Bind to the skeleton targeted by the most channels (first on ties).
            i32   best = -1;
            usize best_votes = 0;
            for (usize si = 0; si < skins_.size(); ++si) {
                if (!skins_[si].id.is_valid()) continue;
                usize votes = 0;
                for (usize c = 0; c < anim.channels_count; ++c) {
                    const cgltf_animation_channel& ch = anim.channels[c];
                    if (ch.target_node && skins_[si].node_to_joint.contains(ch.target_node)) ++votes;
                }
                if (votes > best_votes) {
                    best_votes = votes;
                    best = static_cast<i32>(si);
                }
            }
            const SkinInfo* skin = best >= 0 ? &skins_[static_cast<usize>(best)] : nullptr;

            AnimationClipData clip; // skeletal part (channels targeting the bound skin's joints)
            clip.name = name;
            clip.skeleton = skin ? skin->id : AssetId{};
            AnimationClipData node_clip; // node part (channels targeting other scene nodes)
            node_clip.name = name;
            usize dropped = 0, outside_scene = 0;
            for (usize c = 0; c < anim.channels_count; ++c) {
                const cgltf_animation_channel& ch = anim.channels[c];
                if (!ch.target_node || !ch.sampler) continue;
                AnimationChannel out;
                if (!read_channel(ch, name, c, out)) continue;
                if (skin) {
                    if (auto joint = skin->node_to_joint.find(ch.target_node); joint != skin->node_to_joint.end()) {
                        out.joint = joint->second;
                        clip.duration = std::max(clip.duration, out.times.back());
                        clip.channels.push_back(std::move(out));
                        continue;
                    }
                }
                if (is_skin_joint(ch.target_node)) { // a joint of another skin
                    ++dropped;
                    continue;
                }
                const i32 node = default_scene_node_index(ch.target_node);
                if (node < 0) {
                    ++outside_scene;
                    continue;
                }
                out.joint = static_cast<u32>(node);
                node_clip.duration = std::max(node_clip.duration, out.times.back());
                node_clip.channels.push_back(std::move(out));
            }
            if (dropped > 0) {
                warn("animation '{}': {} channel(s) target joints of a skin other than the bound one (dropped)",
                     name, dropped);
            }
            if (outside_scene > 0) {
                warn("animation '{}': {} channel(s) target nodes outside the default scene (dropped)", name,
                     outside_scene);
            }
            if (clip.channels.empty() && node_clip.channels.empty()) {
                warn("animation '{}': no usable channels; skipped", name);
                continue;
            }
            if (!clip.channels.empty()) {
                const String key = std::format("anim:{}", ai);
                out_.animations.push_back({ id_for(key), key, name, std::move(clip) });
            }
            if (!node_clip.channels.empty()) {
                node_clip.skeleton = node_skeleton_id();
                const String key = std::format("node_anim:{}", ai);
                out_.animations.push_back({ id_for(key), key, name, std::move(node_clip) });
            }
        }
    }

    // Reads one TRS channel's sampler (joint left for the caller). False = skipped (warned).
    bool read_channel(const cgltf_animation_channel& ch, StringView name, usize c, AnimationChannel& out) {
        usize components;
        switch (ch.target_path) {
        case cgltf_animation_path_type_translation: out.path = AnimPath::Translation; components = 3; break;
        case cgltf_animation_path_type_rotation: out.path = AnimPath::Rotation; components = 4; break;
        case cgltf_animation_path_type_scale: out.path = AnimPath::Scale; components = 3; break;
        default:
            warn("animation '{}': morph-weight channels are not supported (ignored)", name);
            return false;
        }
        switch (ch.sampler->interpolation) {
        case cgltf_interpolation_type_step: out.interpolation = Interpolation::Step; break;
        case cgltf_interpolation_type_cubic_spline: out.interpolation = Interpolation::CubicSpline; break;
        default: out.interpolation = Interpolation::Linear; break;
        }
        std::vector<f32> values;
        if (!read_floats(ch.sampler->input, 1, out.times) || !read_floats(ch.sampler->output, components, values)) {
            warn("animation '{}' channel {}: unreadable sampler data; skipped", name, c);
            return false;
        }
        const usize per_key = out.interpolation == Interpolation::CubicSpline ? 3 : 1;
        const usize keys = out.times.size();
        if (keys == 0 || values.size() != keys * per_key * components) {
            warn("animation '{}' channel {}: key/value count mismatch; skipped", name, c);
            return false;
        }
        for (usize k = 1; k < keys; ++k) {
            if (!(out.times[k] > out.times[k - 1])) {
                warn("animation '{}' channel {}: key times are not strictly increasing", name, c);
                break;
            }
        }
        out.values.resize(keys * per_key);
        for (usize v = 0; v < out.values.size(); ++v) {
            const f32* src = &values[v * components];
            out.values[v] = components == 4 ? Vec4(src[0], src[1], src[2], src[3]) : Vec4(src[0], src[1], src[2], 0.0f);
            if (out.path == AnimPath::Rotation && per_key == 1) {
                const f32 len = detail::length(out.values[v]);
                out.values[v] = len > 1e-20f ? out.values[v] / len : Vec4(0.0f, 0.0f, 0.0f, 1.0f);
            }
        }
        return true;
    }

    bool is_skin_joint(const cgltf_node* node) const {
        for (const SkinInfo& s : skins_) {
            if (s.node_to_joint.contains(node)) return true;
        }
        return false;
    }

    // ------------------------------------------------------------------ scenes
    // Index of the default scene (the one "primary" points at; 0 for the implicit scene).
    usize default_scene_index() const {
        return data_->scene && data_->scenes_count > 0 ? cgltf_scene_index(data_, data_->scene) : 0;
    }

    std::vector<const cgltf_node*> scene_roots(usize si) const {
        if (si < data_->scenes_count) {
            const cgltf_scene& s = data_->scenes[si];
            return { s.nodes, s.nodes + s.nodes_count };
        }
        std::vector<const cgltf_node*> roots; // implicit scene: every parentless node
        for (usize ni = 0; ni < data_->nodes_count; ++ni) {
            if (!data_->nodes[ni].parent) roots.push_back(&data_->nodes[ni]);
        }
        return roots;
    }

    // Depth-first order (parents before children) with parent indices: the SceneData layout.
    static std::vector<std::pair<const cgltf_node*, i32>> scene_order(const std::vector<const cgltf_node*>& roots) {
        std::vector<std::pair<const cgltf_node*, i32>> order;
        std::unordered_set<const cgltf_node*>          visited;
        std::vector<std::pair<const cgltf_node*, i32>> stack;
        for (auto it = roots.rbegin(); it != roots.rend(); ++it) stack.emplace_back(*it, -1);
        while (!stack.empty()) {
            const auto [node, parent] = stack.back();
            stack.pop_back();
            if (!node || !visited.insert(node).second) continue;
            const i32 index = static_cast<i32>(order.size());
            order.emplace_back(node, parent);
            for (usize c = node->children_count; c-- > 0;) stack.emplace_back(node->children[c], index);
        }
        return order;
    }

    // SceneData node index of `node` in the default scene (-1 if absent). Builds the order once.
    i32 default_scene_node_index(const cgltf_node* node) {
        if (!default_order_built_) {
            default_order_ = scene_order(scene_roots(default_scene_index()));
            for (usize i = 0; i < default_order_.size(); ++i) default_index_[default_order_[i].first] = static_cast<i32>(i);
            default_order_built_ = true;
        }
        auto it = default_index_.find(node);
        return it == default_index_.end() ? -1 : it->second;
    }

    // The default scene's node skeleton (joint i == SceneData node i); emitted on first use.
    AssetId node_skeleton_id() {
        const String key = std::format("node_skeleton:{}", default_scene_index());
        const AssetId id = id_for(key);
        if (node_skeleton_emitted_) return id;
        node_skeleton_emitted_ = true;
        (void)default_scene_node_index(nullptr); // make sure the order exists
        SkeletonData skel;
        const usize  n = default_order_.size();
        skel.joint_names.resize(n);
        skel.parents.resize(n);
        skel.bind_local.resize(n);
        skel.inverse_bind.resize(n);
        std::vector<Mat4> model(n, Mat4(1.0f));
        for (usize i = 0; i < n; ++i) {
            const auto [node, parent] = default_order_[i];
            skel.joint_names[i] = node_display_name(data_, node);
            skel.parents[i] = parent;
            skel.bind_local[i] = node_local_transform(*node);
            model[i] = (parent >= 0 ? model[static_cast<usize>(parent)] : Mat4(1.0f)) * skel.bind_local[i].to_matrix();
            skel.inverse_bind[i] = detail::inverse(model[i]);
        }
        const String name = std::format("{}_Nodes", detail::to_utf8(path_.stem()));
        out_.skeletons.push_back({ id, key, name, std::move(skel) });
        return id;
    }

    SceneData build_scene(String name, const std::vector<const cgltf_node*>& roots) {
        SceneData scene;
        scene.name = std::move(name);
        for (const auto& [node, parent] : scene_order(roots)) {
            SceneNodeData sn;
            sn.name = node_display_name(data_, node);
            sn.parent = parent;
            sn.local = node_local_transform(*node);
            if (node->mesh) {
                const usize mi = cgltf_mesh_index(data_, node->mesh);
                sn.mesh = mesh_ids_[mi];
                if (sn.mesh.is_valid()) sn.materials = mesh_slots_[mi];
            }
            if (node->skin) sn.skeleton = skins_[cgltf_skin_index(data_, node->skin)].id;
            scene.nodes.push_back(std::move(sn));
        }
        return scene;
    }

    void import_scenes() {
        for (usize si = 0; si < data_->scenes_count; ++si) {
            const cgltf_scene& s = data_->scenes[si];
            const String name = (s.name && s.name[0]) ? String(s.name) : std::format("Scene_{}", si);
            const String key = std::format("scene:{}", si);
            out_.scenes.push_back({ id_for(key), key, name, build_scene(name, scene_roots(si)) });
        }
        if (data_->scenes_count == 0 && data_->nodes_count > 0) {
            const String name = detail::to_utf8(path_.stem());
            out_.scenes.push_back({ id_for("scene:0"), "scene:0", name, build_scene(name, scene_roots(0)) });
        }

        if (!out_.scenes.empty()) {
            out_.primary = out_.scenes[std::min(default_scene_index(), out_.scenes.size() - 1)].id;
        } else if (!out_.meshes.empty()) {
            out_.primary = out_.meshes.front().id;
        } else if (!out_.materials.empty()) {
            out_.primary = out_.materials.front().id;
        }
    }

    const cgltf_data*     data_;
    const fs::path&       path_;
    const ImportSettings& settings_;
    FileContext&          files_;
    ImportResult&         out_;

    std::unordered_map<u64, AssetId>  texture_cache_;
    std::vector<AssetId>              material_ids_;
    std::vector<SkinInfo>             skins_;
    std::vector<AssetId>              mesh_ids_;
    std::vector<std::vector<AssetId>> mesh_slots_;

    // Default-scene node order (node animation), built on first use.
    bool                                           default_order_built_ = false;
    std::vector<std::pair<const cgltf_node*, i32>> default_order_;
    std::unordered_map<const cgltf_node*, i32>     default_index_;
    bool                                           node_skeleton_emitted_ = false;
};

} // namespace

Result<ImportResult> import_gltf(const fs::path& path, const ImportSettings& settings) {
    const String path_utf8 = detail::to_utf8(path);
    auto         bytes = detail::read_file_bytes(path);
    if (!bytes) return bytes.error();

    FileContext   files;
    cgltf_options options{};
    options.file.read = &read_file_cb;
    options.file.release = &release_file_cb;
    options.file.user_data = &files;

    // `bytes` must outlive `data` (a .glb's BIN chunk is referenced in place).
    cgltf_data*  raw = nullptr;
    cgltf_result r = cgltf_parse(&options, bytes->data(), bytes->size(), &raw);
    std::unique_ptr<cgltf_data, CgltfDeleter> data(raw);
    if (r != cgltf_result_success) {
        return Error{ ErrorCode::InvalidArgument, std::format("{}: parse failed: {}", path_utf8, cgltf_result_name(r)) };
    }
    for (usize i = 0; i < data->extensions_required_count; ++i) {
        const StringView ext(data->extensions_required[i]);
        for (StringView unsupported : kUnsupportedRequiredExtensions) {
            if (ext == unsupported) {
                return Error{ ErrorCode::Unsupported,
                              std::format("{}: required extension {} is not supported", path_utf8, ext) };
            }
        }
    }
    r = cgltf_load_buffers(&options, data.get(), path_utf8.c_str());
    if (r != cgltf_result_success) {
        return Error{ ErrorCode::IoError, std::format("{}: loading buffers failed: {}", path_utf8, cgltf_result_name(r)) };
    }
    r = cgltf_validate(data.get());
    if (r != cgltf_result_success) {
        return Error{ ErrorCode::InvalidArgument, std::format("{}: validation failed: {}", path_utf8, cgltf_result_name(r)) };
    }

    ImportResult result;
    GltfImporter importer(data.get(), path, settings, files, result);
    if (auto ok = importer.run(); !ok) return ok.error();

    // Deduplicate dependencies (an external .bin may back several buffers).
    for (const fs::path& dep : files.dependencies) {
        if (std::find(result.dependencies.begin(), result.dependencies.end(), dep) == result.dependencies.end()) {
            result.dependencies.push_back(dep);
        }
    }
    return result;
}

} // namespace aether::assets
