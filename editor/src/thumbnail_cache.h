// thumbnail_cache.h — PRIVATE: asset-browser thumbnails (see aether/editor/thumbnail.h).
//
// One 128x128 RGBA8 texture (registered with ImGui) per content file, created on first request:
//   * images: decoded + area-downscaled on the CPU in update() (<= kImagesPerFrame per frame);
//   * models / prefabs / scenes: one job at a time. update() instantiates the file into a private
//     scratch World (never the editor's), resolves its meshes / materials / textures through a
//     private RenderResourceCache, and once everything is resident frames the bounds; render()
//     then draws it with a dedicated thumbnail Renderer (created lazily, TAA/bloom/GPU culling off
//     so one frame is final) into the thumbnail's texture. At most one thumbnail is rendered per
//     frame, so the thumbnail renderer honours the renderer's once-per-device-frame contract.
// Entries are re-generated when their file's modification time changes. Textures are released
// with a frames-in-flight delay. Main thread only.
#pragma once

#include "aether/core/types.h"
#include "aether/editor/thumbnail.h"
#include "aether/renderer/render_scene.h"
#include "aether/rhi/resources.h"

#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aether {
class World;
}
namespace aether::assets {
class AssetManager;
}
namespace aether::rhi {
class Device;
class CommandList;
} // namespace aether::rhi
namespace aether::renderer {
class Renderer;
}
namespace aether::gameplay {
class RenderResourceCache;
}

namespace aether::editor {

class ThumbnailCache {
public:
    static constexpr u32 kSize           = 128;
    static constexpr u32 kImagesPerFrame = 2;
    static constexpr u32 kJobTimeoutFrames = 600; // render with whatever is resident after this

    enum class State : u8 { None = 0, Pending, Ready, Failed };

    struct Stats {
        u32 ready = 0, pending = 0, failed = 0;
        u32 rendered = 0; // 3D thumbnails rendered since creation
        u32 decoded = 0;  // image thumbnails decoded since creation
    };

    // `shadows` mirrors the main renderer (software rasterisers turn them off, ADR-0004).
    ThumbnailCache(rhi::Device& device, assets::AssetManager& assets, std::filesystem::path content_root,
                   rhi::SamplerHandle sampler, bool shadows);
    ~ThumbnailCache();
    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    // ImGui texture id of `content_relative`'s thumbnail, or 0 while it is pending / has none.
    // Queues generation on first use.
    [[nodiscard]] u64   get(const std::filesystem::path& content_relative);
    [[nodiscard]] State state(const std::filesystem::path& content_relative) const;
    // The thumbnail texture (ShaderRead) for tests / readback; invalid unless Ready.
    [[nodiscard]] rhi::TextureHandle texture(const std::filesystem::path& content_relative) const;
    [[nodiscard]] Stats stats() const;

    // Once per frame before rendering (on_update): decodes images, advances the 3D job, frees
    // retired textures and re-queues files that changed on disk.
    void update(u64 frame_index);
    // Once per frame inside the frame's command recording, outside any rendering scope
    // (on_render_frame): renders the current 3D job if it is ready.
    void render(rhi::CommandList& cmd);
    // Drops every thumbnail (they regenerate on the next request). Requires the GPU to be idle
    // or the frames-in-flight delay to pass (textures are retired, not destroyed).
    void clear();

    // Releases everything. Call with the GPU idle.
    void shutdown();

private:
    struct Entry {
        ThumbnailKind                   kind = ThumbnailKind::None;
        State                           state = State::None;
        std::filesystem::file_time_type mtime{};
        rhi::TextureHandle              texture;
        u64                             imgui_id = 0;
        std::string                     error;
    };
    struct Retired {
        rhi::TextureHandle texture;
        u64                imgui_id = 0;
        u64                frame = 0;
    };
    struct Job {
        std::string            key;
        std::unique_ptr<World> world;
        renderer::RenderScene  scene;
        u32                    frames = 0;
        bool                   ready = false;
    };

    bool ensure_renderer();
    void start_job(const std::string& key);
    void advance_job();
    void finish_job(bool ok, const std::string& error = {});
    void decode_image(const std::string& key, Entry& e);
    void publish(Entry& e, rhi::TextureHandle texture);
    void retire(Entry& e);
    void check_file_changes();
    rhi::TextureHandle create_texture(bool render_target, const char* name);

    rhi::Device&                                    device_;
    assets::AssetManager&                           assets_;
    std::filesystem::path                           root_;
    rhi::SamplerHandle                              sampler_;
    bool                                            shadows_ = false;
    std::unordered_map<std::string, Entry>          entries_; // key: generic content-relative path
    std::deque<std::string>                         image_queue_;
    std::deque<std::string>                         render_queue_;
    std::unique_ptr<renderer::Renderer>             renderer_;
    std::unique_ptr<gameplay::RenderResourceCache>  cache_;
    renderer::EnvHandle                             sky_;
    bool                                            renderer_failed_ = false;
    Job                                             job_;
    std::vector<Retired>                            retired_;
    u64                                             frame_ = 0;
    Stats                                           counters_{};
};

} // namespace aether::editor
