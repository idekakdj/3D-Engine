# Offline shader compilation helper.
#
# ADR-0001: GLSL is compiled at RUNTIME via the glslang C++ API (wrapped in aether.rhi),
# which gives hot-reload for free. Shipping builds precompile the renderer's shaders with
# aether-shaderc (ADR-0014, engine/renderer/CMakeLists.txt: target aether_shader_cache).
#
# aether_add_shader_dir(<target> <dir>) - reserved; currently records the shader
# directory as a target property for tooling to discover.
function(aether_add_shader_dir TARGET DIR)
    set_property(TARGET ${TARGET} APPEND PROPERTY AE_SHADER_DIRS "${DIR}")
endfunction()
