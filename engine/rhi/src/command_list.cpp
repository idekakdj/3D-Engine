// command_list.cpp — default implementations of the additive CommandList entry points
// (ADR-0010). Backends override them; test doubles inherit the logging no-ops.
#include "aether/rhi/command_list.h"

#include "aether/core/log.h"

namespace aether::rhi {

void CommandList::draw_mesh_tasks(u32, u32, u32) {
    AE_LOG_ERROR("RHI", "draw_mesh_tasks: not supported by this command list");
}

void CommandList::draw_mesh_tasks_indirect_count(BufferHandle, u64, BufferHandle, u64, u32, u32) {
    AE_LOG_ERROR("RHI", "draw_mesh_tasks_indirect_count: not supported by this command list");
}

} // namespace aether::rhi
