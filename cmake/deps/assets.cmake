# cmake/deps/assets.cmake - cgltf + stb (header-only; engine TUs define the implementations)
# OWNER: the assets module agent may edit this file to fix integration of its own
# dependencies. Included by cmake/Dependencies.cmake only when the module is enabled.

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
FetchContent_MakeAvailable(cgltf stb)

add_library(aether_cgltf INTERFACE)
target_include_directories(aether_cgltf SYSTEM INTERFACE "${cgltf_SOURCE_DIR}")
add_library(aether_stb INTERFACE)
target_include_directories(aether_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")
