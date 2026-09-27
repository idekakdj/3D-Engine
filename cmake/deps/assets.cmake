# cmake/deps/assets.cmake - cgltf + stb (header-only; engine TUs define the implementations),
# MikkTSpace (tangent space), bc7enc_rdo (BC7 encoder/decoder + rgbcx BC1-5).
# OWNER: the assets module agent may edit this file to fix integration of its own
# dependencies. Included by cmake/Dependencies.cmake only when the module is enabled.
#
# Licenses (verified against the fetched sources, ADR-0009 M2 assets):
#   cgltf       MIT
#   stb         MIT / public domain (dual)
#   MikkTSpace  zlib (license header in mikktspace.h / mikktspace.c)
#   bc7enc_rdo  MIT / Unlicense (dual; LICENSE + header of bc7enc.cpp, bc7decomp.cpp, rgbcx.h)

FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        v1.15
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _none_
    SYSTEM)
FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR  _none_
    SYSTEM)
# github.com/mmikk/MikkTSpace master (2020-03-26), the reference implementation used by
# Blender, Substance, xNormal and the glTF sample assets.
FetchContent_Declare(mikktspace
    URL https://github.com/mmikk/MikkTSpace/archive/3e895b49d05ea07e4c2133156cfa94369e19e409.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR  _none_
    SYSTEM)
# github.com/richgel999/bc7enc_rdo master: bc7enc.cpp (BC7 modes 1/5/6/7 encoder),
# bc7decomp.cpp (BC7 decoder), rgbcx.h (BC1-5 encoder/decoder).
FetchContent_Declare(bc7enc_rdo
    URL https://github.com/richgel999/bc7enc_rdo/archive/b9438627eef73a1157e84201b6fa6eb2ffd6d9f0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR  _none_
    SYSTEM)
FetchContent_MakeAvailable(cgltf stb mikktspace bc7enc_rdo)

add_library(aether_cgltf INTERFACE)
target_include_directories(aether_cgltf SYSTEM INTERFACE "${cgltf_SOURCE_DIR}")
add_library(aether_stb INTERFACE)
target_include_directories(aether_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")

# Third-party code is compiled with warnings off (ADR-0001: our /W4 applies to our targets only).
function(_aether_quiet_third_party target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W0)
    else()
        target_compile_options(${target} PRIVATE -w)
    endif()
    set_target_properties(${target} PROPERTIES FOLDER "third_party" POSITION_INDEPENDENT_CODE ON)
endfunction()

# MikkTSpace: one C file.
add_library(aether_mikktspace STATIC "${mikktspace_SOURCE_DIR}/mikktspace.c")
target_include_directories(aether_mikktspace SYSTEM PUBLIC "${mikktspace_SOURCE_DIR}")
_aether_quiet_third_party(aether_mikktspace)

# bc7enc_rdo: only the MIT/Unlicense encoder + decoder files (not bc7e.ispc, lodepng, the RDO
# tool or the prebuilt ispc.exe, none of which are compiled or run).
add_library(aether_bc7enc STATIC
    "${bc7enc_rdo_SOURCE_DIR}/bc7enc.cpp"
    "${bc7enc_rdo_SOURCE_DIR}/bc7decomp.cpp"
    "${bc7enc_rdo_SOURCE_DIR}/rgbcx.cpp")
target_include_directories(aether_bc7enc SYSTEM PUBLIC "${bc7enc_rdo_SOURCE_DIR}")
_aether_quiet_third_party(aether_bc7enc)
