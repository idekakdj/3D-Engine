// aether/rhi/imgui.h — Dear ImGui integration living inside the RHI.
//
// FROZEN CONTRACT (ADR-0001). The ImGui GLFW+Vulkan backend needs raw Vk handles, so
// it is confined here; the editor calls only these functions and never sees Vulkan.
#pragma once

#include "aether/core/types.h"
#include "aether/rhi/device.h"
#include "aether/rhi/resources.h"

namespace aether {
class Window;
}

namespace aether::rhi {

// Initialize ImGui with GLFW + the Vulkan backend for this device/window.
// Creates the ImGui context, installs input callbacks, uploads fonts.
void imgui_init(Device& device, Window& window);
void imgui_shutdown(Device& device);

// Call at the start of the frame (before building UI). After building UI, call
// imgui_render(cmd) INSIDE a begin_rendering() scope whose single color attachment is
// the swapchain image (format = FrameInfo::swapchain_format, LoadOp::Load to overlay).
// Handles ImGui multi-viewport platform windows if enabled.
void imgui_new_frame();
void imgui_render(CommandList& cmd);

// Register a TextureHandle for display inside ImGui (returns an ImTextureID as u64).
// Used by the editor viewport to show the rendered scene as an image.
// The texture must be in ResourceState::ShaderRead when ImGui samples it.
// `sampler` is currently IGNORED: the ImGui Vulkan backend (1.92) binds its own linear-clamp
// sampler for every user texture. It is kept for API stability / other backends.
u64  imgui_add_texture(Device& device, TextureHandle texture, SamplerHandle sampler);
void imgui_remove_texture(Device& device, u64 imgui_texture_id);

} // namespace aether::rhi
