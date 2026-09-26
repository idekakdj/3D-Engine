// vk_pipeline_cache.cpp — VkPipelineCache persisted under paths::cache_dir().
//
// File = AeHeader + raw vkGetPipelineCacheData blob. Before the blob is handed back to the
// driver it is validated twice: our header (magic, version, vendor/device id, driver
// version, pipelineCacheUUID, size, XXH64 of the payload) and Vulkan's own
// VkPipelineCacheHeaderVersionOne. Driver updates (common on Intel Arc) simply invalidate
// the cache. Writes go to a temp file that is renamed over the old one.
#include "aether/core/hash.h"
#include "aether/core/paths.h"
#include "aether/core/paths_ext.h"
#include "vk_device.h"

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <system_error>

namespace aether::rhi::vk {
namespace {

constexpr u32 kMagic   = 0x43504541; // "AEPC"
constexpr u32 kVersion = 1;

struct AeHeader {
    u32 magic          = kMagic;
    u32 version        = kVersion;
    u32 vendor_id      = 0;
    u32 device_id      = 0;
    u32 driver_version = 0;
    u32 reserved       = 0;
    u8  uuid[VK_UUID_SIZE]{};
    u64 data_size = 0;
    u64 data_hash = 0;
};
static_assert(sizeof(AeHeader) == 56);

std::filesystem::path cache_file(const VkPhysicalDeviceProperties& p) {
    return paths::cache_dir() / std::format("pipeline_cache_{:04x}_{:04x}.bin", p.vendorID, p.deviceID);
}

} // namespace

void VulkanDevice::load_pipeline_cache() {
    const std::filesystem::path file = cache_file(props_);
    std::vector<byte>           blob;
    std::error_code             ec;
    if (std::filesystem::is_regular_file(file, ec)) {
        blob = paths::read_file(file);
    }

    const void* initial      = nullptr;
    usize       initial_size = 0;
    const char* status       = "none on disk";
    if (!blob.empty()) {
        AeHeader h;
        status = "rejected (stale driver/device or corrupt)";
        if (blob.size() >= sizeof(AeHeader)) {
            std::memcpy(&h, blob.data(), sizeof(h));
            const byte* data      = blob.data() + sizeof(AeHeader);
            const usize data_size = blob.size() - sizeof(AeHeader);
            bool        ok = h.magic == kMagic && h.version == kVersion && h.vendor_id == props_.vendorID &&
                      h.device_id == props_.deviceID && h.driver_version == props_.driverVersion &&
                      std::memcmp(h.uuid, props_.pipelineCacheUUID, VK_UUID_SIZE) == 0 && h.data_size == data_size &&
                      hash64(data, data_size) == h.data_hash;
            // Vulkan's own header (VkPipelineCacheHeaderVersionOne) must agree as well.
            if (ok && data_size >= sizeof(VkPipelineCacheHeaderVersionOne)) {
                VkPipelineCacheHeaderVersionOne vh{};
                std::memcpy(&vh, data, sizeof(vh));
                ok = vh.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && vh.vendorID == props_.vendorID &&
                     vh.deviceID == props_.deviceID &&
                     std::memcmp(vh.pipelineCacheUUID, props_.pipelineCacheUUID, VK_UUID_SIZE) == 0;
            } else {
                ok = false;
            }
            if (ok) {
                initial      = data;
                initial_size = data_size;
                status       = "loaded";
            }
        }
    }

    VkPipelineCacheCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    ci.initialDataSize = initial_size;
    ci.pInitialData    = initial;
    if (vkCreatePipelineCache(device_, &ci, nullptr, &pipeline_cache_) != VK_SUCCESS) {
        ci.initialDataSize = 0; // driver refused the blob: start empty
        ci.pInitialData    = nullptr;
        VK_CHECK(vkCreatePipelineCache(device_, &ci, nullptr, &pipeline_cache_));
        status = "rejected by driver";
    }
    AE_LOG_INFO("RHI", "pipeline cache {} ({} bytes): {}", paths::to_utf8(file), initial_size, status);
}

void VulkanDevice::save_pipeline_cache() {
    if (!pipeline_cache_) {
        return;
    }
    usize size = 0;
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr) != VK_SUCCESS || size == 0) {
        return;
    }
    std::vector<byte> data(size);
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, data.data()) != VK_SUCCESS) {
        return;
    }
    data.resize(size);

    AeHeader h;
    h.vendor_id      = props_.vendorID;
    h.device_id      = props_.deviceID;
    h.driver_version = props_.driverVersion;
    std::memcpy(h.uuid, props_.pipelineCacheUUID, VK_UUID_SIZE);
    h.data_size = data.size();
    h.data_hash = hash64(data.data(), data.size());

    const std::filesystem::path file = cache_file(props_);
    std::filesystem::path       tmp  = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            AE_LOG_WARN("RHI", "cannot write pipeline cache '{}'", paths::to_utf8(tmp));
            return;
        }
        out.write(reinterpret_cast<const char*>(&h), sizeof(h));
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) {
            AE_LOG_WARN("RHI", "short write on pipeline cache '{}'", paths::to_utf8(tmp));
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec); // atomic replace
    if (ec) {
        AE_LOG_WARN("RHI", "cannot replace pipeline cache '{}': {}", paths::to_utf8(file), ec.message());
        std::filesystem::remove(tmp, ec);
    }
}

} // namespace aether::rhi::vk
