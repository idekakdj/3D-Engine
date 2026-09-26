# samples/vertical_slice — the M1 artifact (blueprint §14): a lit scene with a physics stack, a
# glTF model, an animated skinned character and Lua-driven props, built on gameplay::Application.
if(NOT (TARGET aether.physics AND TARGET aether.scripting AND TARGET aether.animation))
    message(STATUS "Aether: vertical_slice skipped (needs physics, scripting and animation)")
    return()
endif()

add_executable(vertical_slice main.cpp)
target_link_libraries(vertical_slice PRIVATE
    aether::gameplay aether::physics aether::scripting aether::animation imgui)
ae_set_target_warnings(vertical_slice)
if(MSVC)
    target_compile_options(vertical_slice PRIVATE /permissive- /Zc:preprocessor /Zc:__cplusplus)
endif()
set_target_properties(vertical_slice PROPERTIES FOLDER "samples")
