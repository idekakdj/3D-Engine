# Sample applications. Each sample is added only when the modules it needs are
# part of the configured module set (AE_MODULES).
if(TARGET aether.rhi)
    add_subdirectory(sandbox)
endif()

# M1 vertical slice (owned by the gameplay agent; hook added by the orchestrator).
if(TARGET aether.gameplay AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vertical_slice/CMakeLists.txt")
    add_subdirectory(vertical_slice)
endif()
