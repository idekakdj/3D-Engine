# cmake/deps/scene.cmake - EnTT
# OWNER: the scene module agent may edit this file to fix integration of its own
# dependencies. Included by cmake/Dependencies.cmake only when the module is enabled.

FetchContent_Declare(entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG        v3.14.0
    GIT_SHALLOW    TRUE
    SYSTEM)
FetchContent_MakeAvailable(entt)
