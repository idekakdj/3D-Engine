# aether.assets - Layer 3: importers (glTF/stb), cooked .aeasset format, asset database,
# runtime AssetManager (async loads + hot reload), and the aether-cook CLI.
aether_add_module(assets
    SOURCES
        src/asset_traits.cpp
        src/file_util.cpp
        src/format.cpp
        src/mesh_processing.cpp
        src/texture_processing.cpp
        src/image_importer.cpp
        src/gltf_importer.cpp
        src/import.cpp
        src/asset_database.cpp
        src/asset_manager.cpp
    PUBLIC_DEPS
        aether::core
    PRIVATE_DEPS
        aether_cgltf
        aether_stb
        aether_mikktspace
        aether_bc7enc
        nlohmann_json::nlohmann_json)

# ---------------------------------------------------------------------------
# aether-cook <content_dir> <cooked_dir> [--force] [--compress|--no-compress] [--no-mips] [-j N]
# ---------------------------------------------------------------------------
add_executable(aether-cook tools/cook/main.cpp)
target_link_libraries(aether-cook PRIVATE aether::assets)
ae_set_target_warnings(aether-cook)
if(MSVC)
    target_compile_options(aether-cook PRIVATE /permissive- /Zc:preprocessor /Zc:__cplusplus)
endif()
set_target_properties(aether-cook PROPERTIES FOLDER "tools")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
aether_add_test(assets
    tests/test_helpers.cpp
    tests/mesh_processing_tests.cpp
    tests/texture_tests.cpp
    tests/import_tests.cpp
    tests/format_tests.cpp
    tests/database_tests.cpp
    tests/manager_tests.cpp)
if(TARGET test.assets)
    # stb_image_write for synthesising PNGs; white-box helpers from src/.
    target_link_libraries(test.assets PRIVATE aether_stb)
    target_include_directories(test.assets PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    # The repo path contains a space - the escaped quotes keep it one token.
    target_compile_definitions(test.assets PRIVATE "AE_TEST_CONTENT_DIR=\"${PROJECT_SOURCE_DIR}/content\"")

    # End-to-end: cook the sample content with the CLI.
    add_test(NAME assets.cook_samples
             COMMAND aether-cook "${PROJECT_SOURCE_DIR}/content" "${CMAKE_CURRENT_BINARY_DIR}/cook_samples_out" --force)
endif()
