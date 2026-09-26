# sandbox - the M0 artifact: window + Vulkan device + ImGui overlay + a triangle whose
# GLSL (shaders/sandbox) is compiled at runtime. `--frames N` enables unattended runs.
add_executable(sandbox main.cpp)
target_link_libraries(sandbox PRIVATE aether::rhi aether::core imgui glfw)
ae_set_target_warnings(sandbox)
if(MSVC)
    target_compile_options(sandbox PRIVATE /permissive- /Zc:preprocessor /Zc:__cplusplus)
endif()
set_target_properties(sandbox PROPERTIES FOLDER "samples")
