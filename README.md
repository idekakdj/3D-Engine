# Aether Engine

A C++20 + Vulkan 1.3 real-time 3D engine: clustered-forward PBR renderer with IBL, Jolt physics,
skeletal animation (blend trees, state machines, IK, root motion), Lua 5.4 scripting, a glTF asset
pipeline, and an ImGui docking editor with play-in-editor.

- Architecture, conventions and roadmap: [`docs/BLUEPRINT.md`](docs/BLUEPRINT.md) (read the ADRs at
  the end: they amend the earlier sections).
- Rules for contributors / implementation agents: [`docs/AGENT_GUIDE.md`](docs/AGENT_GUIDE.md).
- Lua scripting API: [`content/scripts/README.md`](content/scripts/README.md).

## Layout

| Path | What |
|---|---|
| `engine/core` | math, logging, jobs, window/input, paths, errors |
| `engine/rhi` | render hardware interface + Vulkan backend, ImGui backend |
| `engine/renderer` | render graph, clustered forward+, shadows, SSAO, TAA, bloom, IBL |
| `engine/scene` | EnTT world, hierarchy, transforms, scene serializer |
| `engine/assets` | glTF / image importers, asset database, cooker, async AssetManager |
| `engine/physics` | Jolt integration (bodies, characters, constraints, queries) |
| `engine/animation` | skeletons, clips, anim graphs, IK, root motion |
| `engine/scripting` | sandboxed Lua VM, script components, hot reload |
| `engine/gameplay` | `Application`, render bridge, input maps, cameras, scene instantiation |
| `editor/` | `aether-editor` |
| `samples/` | `sandbox` (RHI bring-up), `vertical_slice` (M1 demo) |
| `content/` | sample models, scripts and scenes |

## Building

Windows (the primary platform; MSVC 2022 Build Tools, see `scripts/build.ps1`):

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Preset msvc-x64-debug
```

Linux (GCC 13 / Clang; also the headless verification path):

```bash
sudo apt install libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev \
                 mesa-vulkan-drivers vulkan-validationlayers xvfb
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DGLFW_BUILD_WAYLAND=OFF
cmake --build build
```

Dependencies are fetched by CMake (FetchContent, pinned versions). `AE_MODULES` limits which modules are
configured, e.g. `-DAE_MODULES="core;scene;scripting"`.

## Running

```bash
build/bin/aether-editor --scene scenes/showcase.aescene   # the editor
build/bin/vertical_slice                                  # the M1 demo
build/bin/test.<module>                                   # unit tests (doctest), per module
```

Headless (no GPU, no display): start `Xvfb :99`, then run with `DISPLAY=:99`. Useful checks:

```bash
build/bin/aether-editor --self-test          # scripted editor workflow, exit code = result
build/bin/vertical_slice --frames 300 --check
build/bin/sandbox --frames 60
```

Editor controls: hold RMB + WASD/QE to fly, mouse wheel to dolly, LMB to select, W/E/R for the
move/rotate/scale gizmo, F to focus, Ctrl+Z/Y undo/redo, Ctrl+D duplicate, Del delete, Ctrl+S save,
Ctrl+P play/stop.
