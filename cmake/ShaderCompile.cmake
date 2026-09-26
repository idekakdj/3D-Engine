# Offline shader compilation helper.
#
# ADR-0001: the primary path for M0/M1 is RUNTIME compilation via the glslang C++
# API (wrapped in aether.rhi), which gives hot-reload for free. This module is a
# thin placeholder for a future offline/build-time SPIR-V bake step so shaders can
# be precompiled for shipping builds. It is intentionally a no-op stub until the
# renderer's shader-variant system lands (blueprint §7.4).
#
# aether_add_shader_dir(<target> <dir>) - reserved; currently records the shader
# directory as a target property for tooling to discover.
function(aether_add_shader_dir TARGET DIR)
    set_property(TARGET ${TARGET} APPEND PROPERTY AE_SHADER_DIRS "${DIR}")
endfunction()
