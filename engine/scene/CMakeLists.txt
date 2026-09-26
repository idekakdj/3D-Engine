# aether.scene - Layer 3: EnTT-backed World, transform hierarchy, uuids, visibility and the
# .aescene JSON serializer. EnTT is part of the public API (ADR-0002 #10); nlohmann_json is private.
aether_add_module(scene
    SOURCES
        src/world.cpp
        src/hierarchy.cpp
        src/id.cpp
        src/transform.cpp
        src/visibility.cpp
        src/clone.cpp
        src/scene_serializer.cpp
    PUBLIC_DEPS
        aether::core
        EnTT::EnTT
    PRIVATE_DEPS
        nlohmann_json::nlohmann_json)

aether_add_test(scene
    tests/test_world.cpp
    tests/test_transform.cpp
    tests/test_serializer.cpp)
