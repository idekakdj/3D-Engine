// vk_device.h — the Vulkan 1.3 implementation of rhi::Device (private to aether.rhi).
//
// Frame model
//   * frames_in_flight slots. Each slot owns a command pool, the frame's primary command
//     buffer, lazily-begun upload command buffers, a staging arena, a timestamp query
//     pool and a BINARY acquire semaphore.
//   * One TIMELINE semaphore paces the CPU: every frame submission signals ++timeline_value_
//     and a slot is reused only after its last value completed (prepare_slot()).
//   * Present waits on per-SWAPCHAIN-IMAGE binary semaphores (reused only when that image
//     is re-acquired), avoiding the classic present-semaphore reuse hazard.
//   * update_buffer/update_texture record into the current slot's upload command buffer,
//     which is submitted in the same vkQueueSubmit2 batch right before the frame (or
//     flushed by immediate_submit), so data is visible to the next submitted GPU work.
//   * destroy()/unregister_*() are deferred until the timeline passes the value of the
//     next frame submission (the frame that may still reference the object).
#pragma once

#include "aether/rhi/device.h"
#include "aether/rhi/device_ext.h"
#include "resource_pool.h"
#include "vk_common.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aether::rhi::vk {

class VulkanCommandList;

struct BufferRecord {
    VkBuffer        buffer     = VK_NULL_HANDLE;
    VmaAllocation   allocation = nullptr;
    void*           mapped     = nullptr; // persistent mapping (host-visible buffers)
    VkDeviceAddress address    = 0;
    BufferDesc      desc;
};

struct TextureRecord {
    VkImage                  image      = VK_NULL_HANDLE;
    VmaAllocation            allocation = nullptr; // null for swapchain images
    VkFormat                 format     = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags       aspect     = VK_IMAGE_ASPECT_COLOR_BIT; // barriers / attachments
    VkImageView              view       = VK_NULL_HANDLE;            // full view (sampling)
    std::vector<VkImageView> attachment_views; // [mip * array_layers + layer]; may alias `view`
    std::vector<VkImageView> storage_views;    // one per mip (all layers)
    TextureDesc              desc;
    bool                     owns_image  = true;
    bool                     initialized = false; // left Undefined via an upload helper
};

struct SamplerRecord {
    VkSampler   sampler = VK_NULL_HANDLE;
    SamplerDesc desc;
};

struct ShaderRecord {
    VkShaderModule module = VK_NULL_HANDLE;
    ShaderStage    stage  = ShaderStage::Vertex;
    std::string    entry_point;
    std::string    debug_name;
};

struct PipelineRecord {
    VkPipeline          pipeline   = VK_NULL_HANDLE;
    VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    std::string         debug_name;
};

struct StagingAlloc {
    VkBuffer     buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    void*        ptr    = nullptr;
};

// Linear host-visible upload memory, recycled when its frame slot is reused.
class StagingArena {
public:
    void         init(VmaAllocator allocator, VkDevice device, u64 chunk_size);
    StagingAlloc allocate(u64 size, u64 alignment);
    void         reset();
    void         destroy();

private:
    struct Chunk {
        VkBuffer      buffer     = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        u8*           mapped     = nullptr;
        u64           size       = 0;
        u64           offset     = 0;
    };
    std::vector<Chunk> chunks_;
    VmaAllocator       allocator_  = nullptr;
    VkDevice           device_     = VK_NULL_HANDLE;
    u64                chunk_size_ = 0;
};

struct FrameSlot {
    VkCommandPool                      pool = VK_NULL_HANDLE;
    VkCommandBuffer                    cmd  = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer>       upload_cmds;
    u32                                upload_cmds_used = 0;
    VkCommandBuffer                    upload_open      = VK_NULL_HANDLE;
    VkSemaphore                        acquire          = VK_NULL_HANDLE;
    u64                                submitted_value  = 0;
    bool                               prepared         = false;
    StagingArena                       staging;
    VkQueryPool                        timestamps         = VK_NULL_HANDLE;
    bool                               timestamps_written = false;
    FrameStats                         recorded_stats{};
    std::unique_ptr<VulkanCommandList> list;
};

class VulkanDevice final : public Device {
public:
    static Result<std::unique_ptr<Device>> create(const DeviceDesc& desc);
    ~VulkanDevice() override;

    VulkanDevice(const VulkanDevice&)            = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    // ---- rhi::Device ----
    const DeviceFeatures& features() const override { return features_; }
    u32                   frames_in_flight() const override { return static_cast<u32>(frames_.size()); }

    BufferHandle   create_buffer(const BufferDesc&) override;
    TextureHandle  create_texture(const TextureDesc&) override;
    SamplerHandle  create_sampler(const SamplerDesc&) override;
    ShaderHandle   create_shader(const ShaderDesc&) override;
    PipelineHandle create_graphics_pipeline(const GraphicsPipelineDesc&) override;
    PipelineHandle create_compute_pipeline(const ComputePipelineDesc&) override;

    void destroy(BufferHandle) override;
    void destroy(TextureHandle) override;
    void destroy(SamplerHandle) override;
    void destroy(ShaderHandle) override;
    void destroy(PipelineHandle) override;

    const TextureDesc* texture_desc(TextureHandle) const override;
    const BufferDesc*  buffer_desc(BufferHandle) const override;
    u64                buffer_device_address(BufferHandle) override;

    void  update_buffer(BufferHandle, ByteSpan data, u64 dst_offset) override;
    void* map(BufferHandle) override;
    void  unmap(BufferHandle) override;
    void  update_texture(TextureHandle, ByteSpan data, bool gen_mips) override;
    void  update_texture_mip(TextureHandle, u32 mip, u32 layer, ByteSpan data) override;

    DescriptorHandle register_texture(TextureHandle, SamplerHandle) override;
    DescriptorHandle register_storage_texture(TextureHandle, u32 mip) override;
    void             unregister_texture(DescriptorHandle) override;
    void             unregister_storage_texture(DescriptorHandle) override;

    FrameInfo  begin_frame() override;
    void       end_frame(const FrameInfo&) override;
    void       on_resize(u32 width, u32 height) override;
    void       wait_idle() override;
    void       immediate_submit(const std::function<void(CommandList&)>& record) override;
    FrameStats last_frame_stats() const override { return last_stats_; }

    Result<std::vector<u32>> compile_glsl(ShaderStage stage, StringView source, StringView name,
                                          const ShaderCompileOptions& options) override;
    Result<std::vector<u32>> compile_glsl_file(ShaderStage stage, const std::filesystem::path& file,
                                               const ShaderCompileOptions& options) override;

    // ---- backend internals (command lists, ImGui bridge, device_ext) ----
    VkInstance       vk_instance() const { return instance_; }
    VkPhysicalDevice vk_physical_device() const { return gpu_; }
    VkDevice         vk_device() const { return device_; }
    VkQueue          vk_queue() const { return queue_; }
    u32              queue_family() const { return queue_family_; }
    u32              api_version() const { return api_version_; }
    VkPipelineLayout pipeline_layout() const { return pipeline_layout_; }
    VkDescriptorSet  bindless_set() const { return bindless_set_; }
    VkPipelineCache  pipeline_cache() const { return pipeline_cache_; }
    VkFormat         swapchain_vk_format() const { return swapchain_format_; }
    u32              swapchain_image_count() const { return static_cast<u32>(swapchain_images_.size()); }
    u32              min_image_count() const { return min_image_count_; }
    bool             debug_utils_enabled() const { return debug_utils_; }

    const BufferRecord*   buffer(BufferHandle h) const { return buffers_.get(h); }
    const TextureRecord*  texture(TextureHandle h) const { return textures_.get(h); }
    const SamplerRecord*  sampler(SamplerHandle h) const { return samplers_.get(h); }
    const PipelineRecord* pipeline(PipelineHandle h) const { return pipelines_.get(h); }
    VkImageView           attachment_view(const TextureRecord& t, u32 mip, u32 layer) const;

    // Runs `fn` once all GPU work that may reference current resources has completed.
    void defer(std::function<void()> fn);
    // Called after each successful present (ImGui multi-viewport platform windows).
    void set_post_present_hook(std::function<void()> hook) { post_present_hook_ = std::move(hook); }
    void set_object_name(VkObjectType type, u64 handle, const std::string& name) const;

    void       set_vsync(bool enabled);
    bool       vsync() const { return vsync_; }
    DeviceInfo info() const;

private:
    VulkanDevice() = default;

    // init (vk_device.cpp)
    Result<void> init(const DeviceDesc& desc);
    Result<void> create_instance(const DeviceDesc& desc);
    Result<void> create_surface();
    Result<void> pick_physical_device();
    Result<void> create_logical_device();
    Result<void> create_allocator();
    void         create_frames();
    void         teardown();
    void         query_timestamps(FrameSlot& slot);

    // bindless + layout (vk_bindless.cpp)
    void create_bindless();
    void destroy_bindless();
    void write_sampled_descriptor(u32 index, VkImageView view, VkSampler sampler);
    void write_storage_descriptor(u32 index, VkImageView view);

    // pipeline cache (vk_pipeline_cache.cpp)
    void load_pipeline_cache();
    void save_pipeline_cache();

    // swapchain (vk_swapchain.cpp)
    bool recreate_swapchain();
    void destroy_swapchain(bool keep_handle);

    // frames / uploads (vk_frame.cpp)
    FrameSlot&      current_slot() { return frames_[frame_index_ % frames_.size()]; }
    void            prepare_slot(FrameSlot& slot);
    void            process_deferred(u64 completed_value, bool everything);
    void            generate_mips(VkCommandBuffer cmd, const TextureRecord& t);
    bool            format_supports_blit(VkFormat format, bool& linear) const;
    VkCommandBuffer upload_cmd();
    VkCommandBuffer close_upload_cmd(FrameSlot& slot); // VK_NULL_HANDLE if none open
    StagingAlloc    stage(ByteSpan data, u64 alignment);
    u64             completed_timeline_value() const;

    // resources (vk_resources.cpp)
    void destroy_texture_record(TextureRecord& t);
    bool create_texture_views(TextureRecord& t);

    DeviceDesc desc_{};
    Window*    window_ = nullptr;

    VkInstance               instance_        = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_       = VK_NULL_HANDLE;
    bool                     debug_utils_     = false;
    bool                     validation_      = false;
    VkSurfaceKHR             surface_         = VK_NULL_HANDLE;
    VkPhysicalDevice         gpu_             = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    std::string              driver_name_;
    std::string              driver_info_;
    u32                      api_version_     = 0;
    VkDevice                 device_          = VK_NULL_HANDLE;
    u32                      queue_family_    = 0;
    VkQueue                  queue_           = VK_NULL_HANDLE;
    VmaAllocator             allocator_       = nullptr;
    DeviceFeatures           features_{};
    bool                     memory_budget_   = false;
    bool                     depth_bias_clamp_ = false;
    u64                      device_local_bytes_ = 0;
    f32                      timestamp_period_ = 0.0f;
    bool                     timestamps_supported_ = false;

    VkPipelineCache       pipeline_cache_  = VK_NULL_HANDLE;
    VkPipelineLayout      pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout bindless_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool      bindless_pool_   = VK_NULL_HANDLE;
    VkDescriptorSet       bindless_set_    = VK_NULL_HANDLE;
    u32                   sampled_capacity_ = 0;
    u32                   storage_capacity_ = 0;
    std::mutex            bindless_mutex_;
    std::vector<u32>      sampled_free_;
    std::vector<u32>      storage_free_;
    std::vector<u8>       sampled_gen_;
    std::vector<u8>       storage_gen_;
    std::vector<u8>       sampled_live_;
    std::vector<u8>       storage_live_;
    TextureHandle         fallback_texture_;
    TextureHandle         fallback_storage_;
    SamplerHandle         fallback_sampler_;

    ResourcePool<BufferTag, BufferRecord>     buffers_;
    ResourcePool<TextureTag, TextureRecord>   textures_;
    ResourcePool<SamplerTag, SamplerRecord>   samplers_;
    ResourcePool<ShaderTag, ShaderRecord>     shaders_;
    ResourcePool<PipelineTag, PipelineRecord> pipelines_;

    VkSwapchainKHR             swapchain_        = VK_NULL_HANDLE;
    VkFormat                   swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkColorSpaceKHR            color_space_      = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D                 swapchain_extent_{ 0, 0 };
    VkPresentModeKHR           present_mode_     = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage>       swapchain_images_;
    std::vector<TextureHandle> swapchain_textures_;
    std::vector<VkSemaphore>   render_finished_; // per swapchain image
    u32                        min_image_count_ = 2;
    bool                       swapchain_dirty_ = true;
    bool                       vsync_           = true;

    std::vector<FrameSlot> frames_;
    u64                    frame_index_    = 0; // valid frames begun so far
    VkSemaphore            timeline_       = VK_NULL_HANDLE;
    std::atomic<u64>       timeline_value_{ 0 }; // last value submitted (read by defer())
    bool                   frame_open_     = false;
    u32                    current_image_  = 0;

    struct Deferred {
        u64                   value = 0;
        std::function<void()> fn;
    };
    std::mutex           deferred_mutex_;
    std::deque<Deferred> deferred_;

    VkCommandPool   immediate_pool_  = VK_NULL_HANDLE;
    VkCommandBuffer immediate_cmd_   = VK_NULL_HANDLE;
    VkFence         immediate_fence_ = VK_NULL_HANDLE;
    std::unique_ptr<VulkanCommandList> immediate_list_;

    FrameStats            last_stats_{};
    std::function<void()> post_present_hook_;
};

// Downcast helper for the additive free-function APIs (single backend).
inline VulkanDevice&       as_vulkan(Device& d) { return static_cast<VulkanDevice&>(d); }
inline const VulkanDevice& as_vulkan(const Device& d) { return static_cast<const VulkanDevice&>(d); }

// vk_debug.cpp
VkDebugUtilsMessengerCreateInfoEXT make_messenger_info();
void                               mark_validation_active(bool active);
void                               record_leaks(u32 resources, u32 allocations);

} // namespace aether::rhi::vk
