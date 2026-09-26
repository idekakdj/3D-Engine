# Aether Engine — Agent Guide (read before writing any code)

Every implementation agent works under these rules. The orchestrator enforces them at
integration; work that violates them is sent back.

## 1. Read first
1. `docs/BLUEPRINT.md` — especially §4 (layering), §12 (coding standards), **ADR-0001**, **ADR-0002**, §18.
2. The **frozen contract headers** for your module and everything you depend on (list in §3).

## 2. Ownership — write ONLY inside your area

| Owner | May create/modify |
|---|---|
| Orchestrator | root `CMakeLists.txt`, `CMakePresets.json`, `cmake/*.cmake`, `scripts/build.ps1`, `scripts/run.ps1`, `docs/BLUEPRINT.md`, `docs/AGENT_GUIDE.md`, `shaders/common/**` (frozen) |
| core-rhi agent | `engine/core/**`, `engine/rhi/**`, `samples/CMakeLists.txt`, `samples/sandbox/**`, `shaders/sandbox/**`, `cmake/deps/rhi.cmake` |
| scene agent | `engine/scene/**`, `cmake/deps/scene.cmake`, `cmake/deps/json.cmake` |
| assets agent | `engine/assets/**`, `cmake/deps/assets.cmake`, `content/samples/**` |
| renderer agent | `engine/renderer/**`, `shaders/renderer/**` |
| physics agent | `engine/physics/**`, `cmake/deps/physics.cmake` |
| animation agent | `engine/animation/**` |
| scripting agent | `engine/scripting/**`, `cmake/deps/scripting.cmake`, `content/scripts/**` |
| gameplay agent | `engine/gameplay/**` (except the frozen `application.h`), `samples/vertical_slice/**` |
| editor agent | `editor/**`, `cmake/deps/editor.cmake` |
| validation agent | `scripts/provision_validation.ps1`, `C:/Users/paulc/.aether/tools/vvl/**` |

Never edit another owner's files. If you need something from another module, code against
its frozen header; if the header is missing something, **work around it inside your module**
and list the request under "Contract change requests" in your report.

## 3. Frozen contracts
Implement exactly; do not change signatures. If a frozen header has a genuine defect (cannot
compile, or impossible to implement correctly) you may make the **minimal** fix and must list
it under "Contract changes made" in your report. Additive helpers go in **new** headers.

- core: `types.h error.h log.h handle.h math.h geometry.h reflect.h input.h window.h time.h paths.h job_system.h subsystem.h`
- rhi: `enums.h resources.h command_list.h device.h imgui.h shader_compiler.h`
- shaders: `shaders/common/bindless.glsl` (GLSL mirror of the universal binding model)
- renderer: `render_scene.h renderer.h`
- scene: `entity.h components.h world.h`
- assets: `asset_types.h`
- gameplay: `application.h` (ADR-0003: gameplay = assembly layer 4b; may depend on physics/animation/scripting)

Namespaces: core vocabulary + scene's World/Entity/components live in `aether`; everything else
in `aether::<module>` (`aether::rhi`, `aether::renderer`, `aether::assets`, `aether::physics`,
`aether::animation`, `aether::scripting`, `aether::gameplay`, `aether::editor`).

## 4. Building — your own isolated build tree
All builds go through the wrapper (it imports the MSVC environment; `cl.exe` is not on PATH):

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Preset wip-<module> -Reconfigure   # after any CMakeLists change
powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Preset wip-<module> -Target aether.<module>
powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Preset wip-<module> -Target test.<module>
C:/Users/paulc/.aether/build/wip-<module>/bin/test.<module>.exe
```

- Use **only your preset** (`wip-core-rhi`, `wip-scene`, `wip-assets`, `wip-renderer`, `wip-physics`,
  `wip-animation`, `wip-scripting`, `wip-gameplay`, `wip-editor`). Never touch `msvc-x64-*` (orchestrator's) or another agent's preset.
- The machine has **16 GB RAM** shared by ~8 concurrent builds: never raise the preset's job count. If MSVC
  fails with an out-of-memory error (C1060, C1076, C3859, "heap space"), it is contention, not your code:
  retry with `-Jobs 2`.
- Long builds: tool calls time out at 10 minutes. Run long builds with `run_in_background` and poll, or
  simply re-run - Ninja resumes incrementally.
- Other agents write sibling modules concurrently. `CMAKE_OPTIMIZE_DEPENDENCIES` is ON, so building
  `aether.<yours>` compiles only your module. A test that links an in-flux sibling may fail for
  reasons outside your control — report it, don't "fix" their code.
- **Never list a source file in CMakeLists.txt that does not exist on disk** — other agents
  configure your module too. Create files (stubs are fine) before referencing them.
- Module CMakeLists uses the helpers:
  ```cmake
  aether_add_module(<name> SOURCES src/a.cpp ... PUBLIC_DEPS aether::core ... PRIVATE_DEPS <third-party>)
  aether_add_test(<name> tests/<name>_tests.cpp ...)   # doctest; include <doctest/doctest.h>, main is generated
  ```
- Third-party targets available (see `cmake/Dependencies.cmake` + `cmake/deps/*`): `Vulkan::Headers volk
  aether_vma aether_glslang imgui glfw glm::glm doctest::doctest EnTT::EnTT nlohmann_json::nlohmann_json
  aether_cgltf aether_stb Jolt aether_lua aether_sol2`. Keep third-party types out of your public headers
  (allowed exceptions: glm via core math, EnTT via scene).
- Need a new third-party library? Only inside your own `cmake/deps/<module>.cmake`, pinned to a
  verified tag/commit, permissive license (MIT/BSD/Apache/zlib), and listed in your report.

## 5. Code standards (summary of BLUEPRINT §12 + ADRs)
- C++20, MSVC 14.44. Your code must compile **warning-free at /W4** with `/permissive-`.
- Exceptions are enabled at the compiler level, but engine code **never throws**; recoverable
  failures return `aether::Result<T>`; programmer errors use `AE_ASSERT` / `AE_VERIFY`.
- Logging: `AE_LOG_INFO("Category", "fmt {}", x)` (std::format syntax).
- Naming: types `PascalCase`; functions/variables `snake_case`; private members `snake_case_`;
  macros `AE_UPPER`; files `snake_case.h/.cpp`; `#pragma once`.
- Public headers in `include/aether/<module>/`, private in `src/`. Every public header starts with a
  file comment; document thread-affinity on public APIs (`// Thread-safe.` / `// Main thread only.`).
- Handles over raw pointers for pooled resources (`aether::Handle<T>`); no `new`/`delete` in hot paths.
- Paths: never CWD-relative — use `aether::paths::*`.
- Tests: doctest for every non-trivial algorithm (math, hierarchy, parsing, blending, ...).

## 6. Hard don'ts
- No `git` commands that change state (no commit/checkout/reset/stash). The orchestrator handles git.
- Do not edit root `CMakeLists.txt`, `CMakePresets.json`, `cmake/*.cmake` (except your own deps file), `scripts/build.ps1`, `docs/BLUEPRINT.md`.
- Do not download anything outside FetchContent in your own deps file (validation agent excepted).
- Do not leave the build broken at the end: if a feature doesn't compile, stub it cleanly and report it.

## 7. Final report (your last message — the orchestrator reads only this)
```
## <module> report
Status: COMPLETE | PARTIAL | BLOCKED
Files created/modified: <list>
Public API summary: <key types/functions>
Build: <exact commands run> -> <result>. Warnings in our code: <count>
Tests: <N> test cases, <N> assertions -> <pass/fail>
Implemented: <bullets>
Not implemented / TODO (next session): <bullets>
Contract changes made: <file: change + reason> | none
Contract change requests: <request + reason> | none
Known issues / risks: <bullets>
```
