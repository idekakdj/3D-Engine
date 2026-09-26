# cmake/deps/json.cmake - nlohmann_json (shared by scene + assets)
# OWNER: the json module agent may edit this file to fix integration of its own
# dependencies. Included by cmake/Dependencies.cmake only when the module is enabled.

FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
FetchContent_MakeAvailable(json)
