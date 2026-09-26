// gameplay_test_utils.h — shared fixtures for the gameplay tests: a recording mock Renderer,
// temporary content roots seeded from the repository samples, and an AssetManager fixture.
#pragma once

#include "aether/assets/asset_manager.h"
#include "aether/core/job_system.h"
#include "aether/renderer/renderer.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace aether::gameplay::test {

// Records every registration / release; handles are sequential and never reused.
class MockRenderer final : public renderer::Renderer {
public:
    renderer::MeshHandle register_mesh(const renderer::MeshUpload& up) override {
        meshes.push_back(up.debug_name);
        mesh_vertex_counts.push_back(up.vertices.size());
        mesh_skinned.push_back(!up.skin.empty());
        return renderer::MeshHandle(next_++, 1);
    }
    renderer::TextureHandle register_texture(const renderer::TextureUpload& up) override {
        textures.push_back(up);
        textures.back().pixels = {};
        return renderer::TextureHandle(next_++, 1);
    }
    renderer::MaterialHandle register_material(const renderer::MaterialDesc& desc) override {
        materials.push_back(desc);
        return renderer::MaterialHandle(next_++, 1);
    }
    renderer::EnvHandle register_environment(const renderer::EnvironmentUpload&) override {
        return renderer::EnvHandle(next_++, 1);
    }
    void update_material(renderer::MaterialHandle h, const renderer::MaterialDesc& desc) override {
        material_updates.push_back({ h, desc });
    }
    void release(renderer::MeshHandle) override { ++released_meshes; }
    void release(renderer::TextureHandle) override { ++released_textures; }
    void release(renderer::MaterialHandle) override { ++released_materials; }
    void release(renderer::EnvHandle) override {}
    void render(const renderer::RenderScene&, rhi::CommandList&, const renderer::RenderTarget&) override {}
    void resize(UVec2) override {}
    Result<void> reload_shaders() override { return {}; }
    renderer::RendererSettings&    settings() override { return settings_; }
    const renderer::RendererStats& stats() const override { return stats_; }

    std::vector<std::string>                 meshes;
    std::vector<usize>                       mesh_vertex_counts;
    std::vector<bool>                        mesh_skinned;
    std::vector<renderer::TextureUpload>     textures;
    std::vector<renderer::MaterialDesc>      materials;
    struct MaterialUpdate {
        renderer::MaterialHandle handle;
        renderer::MaterialDesc   desc;
    };
    std::vector<MaterialUpdate> material_updates;
    u32                         released_meshes = 0, released_textures = 0, released_materials = 0;

private:
    u32                         next_ = 1;
    renderer::RendererSettings  settings_{};
    renderer::RendererStats     stats_{};
};

inline std::filesystem::path repo_content() { return std::filesystem::path(AE_TEST_CONTENT_DIR); }

class TempDir {
public:
    explicit TempDir(const char* tag) {
        static std::atomic<u32> counter{ 0 };
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("aether_gameplay_" + std::string(tag) + "_" + std::to_string(stamp) + "_" +
                 std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

inline void ensure_job_system() {
    static const bool once = [] {
        if (JobSystem::worker_count() == 0) {
            JobSystem::initialize(2);
        }
        return true;
    }();
    (void)once;
}

// A content root holding copies of the repository samples + an initialized AssetManager.
struct AssetFixture {
    TempDir              content{ "content" };
    TempDir              cooked{ "cooked" };
    assets::AssetManager manager;

    explicit AssetFixture(bool use_jobs = false) {
        ensure_job_system();
        std::filesystem::copy(repo_content() / "samples", content.path() / "samples",
                              std::filesystem::copy_options::recursive);
        assets::AssetManagerConfig cfg;
        cfg.content_root             = content.path();
        cfg.cooked_root              = cooked.path();
        cfg.mode                     = assets::AssetLoadMode::Editor;
        cfg.use_job_system           = use_jobs;
        cfg.hot_reload_poll_interval = 0.0;
        REQUIRE(manager.initialize(cfg).has_value());
    }
};

} // namespace aether::gameplay::test
