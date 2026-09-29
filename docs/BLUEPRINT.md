# Aether Engine — Master Blueprint

> A from-scratch, C++20 + Vulkan real-time 3D engine intended to rival Unreal Engine
> across rendering fidelity, editor/tooling, physics/animation, and scripting/gameplay.
>
> **Status:** Foundation phase (multi-session project). **Author:** Engine architect (orchestrator).
> **Doc version:** 1.18 (ADR-0018 applied). Update this header on every material revision.

---

## 0. How to read this document

This is the single source of truth for the engine's architecture, conventions, and roadmap.
Every subsystem owner (human or subagent) must conform to the **Coding Standards (§12)**, the
**Module Dependency Rules (§4)**, and the **Public API/ABI contracts** their module exposes.
When a decision here conflicts with an implementation, the blueprint wins until formally amended
(amendments are appended to §16 Change Log with rationale).

---

## 1. Vision, scope & non-goals

### 1.1 Vision
Build a modern, data-oriented, GPU-driven engine with a production-grade editor. Match Unreal's
*capability surface* over time — physically based rendering, dynamic global illumination,
virtualized geometry, a full scene editor, a robust physics/animation stack, and a gameplay +
scripting framework — while being leaner, more hackable, and Vulkan-first.

**Delivery (owner requirement, ADR-0012):** Aether ships as a **downloadable desktop application that
runs entirely locally** — the user downloads an installer/archive, installs it, and launches the editor
like any other program: no source build, no compiler or SDK, no account, no cloud service, works
offline. Games made with it are exported as standalone local executables (`aether-player --package`,
ADR-0006).

### 1.2 What "rival Unreal" concretely means (capability targets)
| Pillar | Unreal reference feature | Aether target (phased) |
|---|---|---|
| Rendering | Lumen (dynamic GI) | SSGI → voxel/surfel GI → (stretch) hardware-RT GI |
| Rendering | Nanite (virtualized geometry) | Meshlet/mesh-shader GPU-driven pipeline → cluster LOD streaming |
| Rendering | PBR + IBL + post stack | Clustered forward+ PBR, IBL, TAA, bloom, tonemap, SSAO (M1) |
| Editor | Unreal Editor | ImGui docking editor: viewport, hierarchy, inspector, asset browser, gizmos, PIE |
| Physics | Chaos | Jolt Physics integration (rigid bodies, characters, constraints) |
| Animation | Anim Blueprint / Control Rig | Skeleton, clips, blend trees, state machines, 2-bone IK, root motion |
| Gameplay | Actor/Component + Blueprints | ECS + gameplay framework + Lua scripting (visual scripting later) |
| Assets | UAsset + cook | GUID asset DB, glTF/stb/KTX2 import, offline cooker, hot-reload |
| Platforms | Win/Linux/Mac/consoles | Windows-first; Linux via SDL/Vulkan parity kept in the abstraction |
| Distribution | Unreal Editor installed from a download | Standalone installer (Windows first): editor + player + shaders + starter content, runs offline |

### 1.3 Non-goals (explicitly out of scope, at least for the first several milestones)
- Console platform ports (NDA toolchains), mobile/GLES/Metal backends.
- A bespoke shading language/compiler (we use GLSL/HLSL → SPIR-V via glslang/DXC).
- A marketplace, launcher, or cloud services. (A plain downloadable installer of the editor is IN scope —
  see "Delivery" above; only an Epic-style launcher/store is out.)
- Backwards ABI compatibility guarantees during pre-1.0 (we refactor freely).
- Photoreal film-quality path tracing (a debug reference path tracer is a *stretch* goal only).

### 1.4 Guiding principles
1. **Data-oriented by default.** Hot paths operate over contiguous arrays (SoA), not object graphs.
2. **GPU-driven where it pays.** Culling, LOD selection, and draw submission move to the GPU.
3. **RHI abstraction, Vulkan backend.** No Vulkan types leak above the RHI boundary.
4. **Explicit over implicit.** Explicit lifetimes, explicit allocators, explicit frame boundaries.
5. **Tooling is a first-class citizen.** If it isn't inspectable in the editor, it isn't done.
6. **Compile fast, iterate fast.** Unity/jumbo builds optional; strict module boundaries; PCH.
7. **Deterministic simulation.** Fixed-step physics/gameplay; frame-rate independent.

---

## 2. Technology stack & rationale

| Concern | Choice | Rationale |
|---|---|---|
| Language | C++20 (concepts, ranges, `<span>`, `<bit>`, designated init, modules-off) | Industry standard for AAA; we avoid C++20 *modules* for tooling stability, prefer headers + PCH. |
| Graphics API | Vulkan 1.3 core (1.4 opportunistic) | Explicit, cross-vendor, dynamic rendering, timeline semaphores, descriptor indexing, sync2. |
| GPU loader | **volk** | Dynamically loads `vulkan-1.dll`; no Vulkan SDK link dependency; per-device function tables. |
| Headers | **Vulkan-Headers** (Khronos) | Header-only; pinned version; no full SDK install required. |
| Allocator | **VulkanMemoryAllocator (VMA)** | Battle-tested GPU allocator; sub-allocation, defrag, budgets. |
| Windowing/input | **GLFW** (M1) with a thin `Window`/`Input` abstraction | Cross-platform, minimal; abstraction lets us swap to SDL3 or raw Win32 later. |
| Math | Custom SIMD math (`aether::math`), `glm` as a reference/fallback during bring-up | Full control over layout, alignment, and codegen for hot paths. |
| Shader compile | **glslang** (GLSL) + optional **DXC** (HLSL) → SPIR-V; **SPIRV-Reflect** for reflection | Offline + runtime; reflection drives descriptor layout automation. |
| ECS | **EnTT** | Best-in-class C++ sparse-set ECS; header-only; proven. |
| Physics | **Jolt Physics** | Modern, multicore, deterministic; used in AAA (Horizon). |
| Scripting | **Lua 5.4 + sol2** (M1); design for C#/visual scripting later | Tiny, embeddable, hot-reloadable; sol2 gives clean bindings. |
| Editor UI | **Dear ImGui** (docking) + **ImGuizmo** + **ImPlot** | De-facto standard for engine tooling; immediate-mode fits editor iteration. |
| Model import | **cgltf** (glTF 2.0), **stb_image**, **KTX-Software** (KTX2/Basis) | Lean, permissive, wide coverage. |
| Logging | Custom (`aether::log`) with optional **spdlog** backend | Structured, async, category-based. |
| Serialization | Custom binary (cooked) + JSON (source assets) via a reflection layer | Reflection-driven so new components serialize for free. |
| Job system | Custom work-stealing scheduler | Fibers optional later; first pass is task-graph over a thread pool. |
| Build | **CMake ≥ 3.28** + **Ninja**, MSVC 14.4x (VS2022 BuildTools) | Present on the build machine; FetchContent for all deps. |
| Test | **doctest** (unit), custom golden-image harness (render) | Header-only, fast; render tests compare against reference PNGs. |
| Profiling | **Tracy** | Frame + GPU timeline profiling; opt-in via `AE_TRACY`. |

**License posture:** every third-party is permissive (MIT/Apache-2.0/BSD/zlib/public-domain).
No GPL/LGPL. See §13 for the pinned list and licenses.

### 2.1 Build machine reality (verified this session)
- MSVC **14.44.35207** (VS 2022 Build Tools), CMake **3.31.6**, Ninja **1.12.1**, Windows SDK **10.0.26100** — all bundled with VS Build Tools, activated via `vcvars64.bat`.
- **No Vulkan SDK installed** → we deliberately depend only on `Vulkan-Headers` + `volk` (runtime loader). `vulkan-1.dll v1.4.313.0` is present in System32 (Intel Arc driver), so built binaries can run.
- GPU: **Intel Arc** (Vulkan 1.4, mesh shaders, bindless, RT capable). GitHub reachable → FetchContent works.

---

## 3. Repository layout

```
3D Engine/
├─ CMakeLists.txt                 # root; options, toolchain guards, add_subdirectory of modules
├─ CMakePresets.json              # msvc-x64-{debug,release,reldbg}; points at bundled Ninja
├─ .clang-format / .clang-tidy    # enforced style + lint
├─ .editorconfig / .gitignore / .gitattributes
├─ README.md
├─ docs/
│  ├─ BLUEPRINT.md                # this document
│  ├─ ARCHITECTURE.md             # deep dives per subsystem (grows over time)
│  ├─ CODING_STANDARDS.md         # extracted §12 for quick reference
│  └─ adr/                        # Architecture Decision Records (one file per decision)
├─ cmake/
│  ├─ AetherModule.cmake          # aether_add_module() helper (warnings, PCH, folders)
│  ├─ Dependencies.cmake          # all FetchContent_Declare + MakeAvailable
│  ├─ CompilerWarnings.cmake      # /W4 /WX (opt), sanitizer toggles
│  └─ ShaderCompile.cmake         # aether_compile_shaders() -> SPIR-V
├─ third_party/                   # vendored small headers (single-file libs) + FetchContent cache
├─ engine/
│  ├─ core/        (aether.core)      # platform, memory, math, containers, log, jobs, reflection, time
│  ├─ rhi/         (aether.rhi)       # RHI interface + vulkan/ backend
│  ├─ renderer/    (aether.renderer)  # render graph, passes, materials, lighting, post
│  ├─ scene/       (aether.scene)     # ECS wrapper, transforms, world, components
│  ├─ assets/      (aether.assets)    # asset DB, importers, cooker, streaming
│  ├─ physics/     (aether.physics)   # Jolt bridge
│  ├─ animation/   (aether.animation) # skeleton, clips, blend, IK, skinning
│  ├─ scripting/   (aether.scripting) # Lua VM, bindings, script components
│  └─ gameplay/    (aether.gameplay)  # framework, input mapping, camera, systems glue
├─ editor/         (aether.editor)    # ImGui editor application
├─ runtime/        (aether.runtime)   # standalone game/player launcher
├─ shaders/                            # .glsl/.hlsl sources + generated SPIR-V
├─ tools/                              # shaderc CLI wrapper, asset cooker CLI, codegen
├─ samples/                            # sample scenes & mini-apps
└─ tests/                              # unit + golden-image tests
```

---

## 4. Module architecture & dependency rules

### 4.1 Layered dependency graph (a module may depend only on layers **below** it)

```
        ┌───────────────────────────── editor / runtime ─────────────────────────────┐
Layer 5 │  aether.editor            aether.runtime                                     │
        └──────────────────────────────────────────────────────────────────────────────┘
        ┌──────────────── high-level systems (may depend on scene + below) ────────────┐
Layer 4 │  aether.gameplay   aether.scripting   aether.physics   aether.animation       │
        └──────────────────────────────────────────────────────────────────────────────┘
        ┌──────────────── mid-level (depend on rhi/core, not each other) ──────────────┐
Layer 3 │  aether.renderer            aether.assets            aether.scene              │
        └──────────────────────────────────────────────────────────────────────────────┘
Layer 2 │  aether.rhi   (interface + vulkan backend)                                    │
        └──────────────────────────────────────────────────────────────────────────────┘
Layer 1 │  aether.core  (no engine deps; only std + tiny 3rd-party)                     │
        └──────────────────────────────────────────────────────────────────────────────┘
```

**Hard rules**
- No upward or sideways-in-same-layer dependencies. `renderer` must **not** include `scene`;
  the *gameplay/editor* layer wires scene data into the renderer via explicit "render scene" DTOs.
- Cross-module communication is through **public headers only** (`engine/<m>/include/aether/<m>/…`).
  Private headers/impl live in `engine/<m>/src/…` and are never included across modules.
- Every module is a **static library** target named `aether.<name>` with an alias `aether::<name>`.
- Only `core` may expose third-party types in its public headers **and only** for math/log; everything
  else keeps third-party types private (e.g., Vulkan, Jolt, Lua, EnTT are *implementation details*).

### 4.2 Module ownership boundaries (which subagent owns what)
| Module | Owner (subagent) | Public surface others rely on |
|---|---|---|
| core | Core team | allocators, math types, containers, `Log`, `JobSystem`, `TypeInfo`, `Window`, `Input` |
| rhi | RHI team | `rhi::Device`, `Swapchain`, `CommandList`, `Pipeline`, `Buffer`, `Texture`, `DescriptorSet`, enums |
| renderer | Renderer team | `Renderer`, `RenderScene` (input DTO), `MaterialInstance`, `RenderView`, render-graph nodes |
| scene | Scene team | `World`, `Entity`, transform/hierarchy components, `SceneSerializer` |
| assets | Assets team | `AssetId`, `AssetManager`, `AssetHandle<T>`, importer/cooker CLIs, `Mesh`/`Texture`/`Material` assets |
| physics | Physics team | `PhysicsWorld`, `RigidBodyComponent`, `Collider`, queries, `CharacterController` |
| animation | Anim team | `Skeleton`, `AnimationClip`, `AnimGraph`, `Pose`, `SkinnedMeshComponent` |
| scripting | Scripting team | `ScriptVM`, `ScriptComponent`, binding registration API |
| gameplay | Gameplay team | `Application`, `GameLoop`, `CameraController`, system registration, input maps |
| editor | Editor team | the editor executable; depends on all |

---

## 5. Engine runtime architecture

### 5.1 Application & frame loop
`aether.gameplay::Application` owns the lifecycle. Subsystems implement a common interface:

```cpp
struct ISubsystem {
    virtual ~ISubsystem() = default;
    virtual void on_startup(EngineContext&) = 0;
    virtual void on_shutdown() = 0;
    virtual void on_begin_frame(FrameContext&) {}
    virtual void on_fixed_update(FixedContext&) {}   // deterministic, may run 0..N times/frame
    virtual void on_update(FrameContext&) {}         // variable-rate gameplay/animation
    virtual void on_render(RenderContext&) {}        // build render scene / submit
    virtual void on_end_frame(FrameContext&) {}
};
```

**Frame timeline (fixed-step accumulator):**
```
poll input → begin_frame
while (accumulator >= fixed_dt):  on_fixed_update  (physics, deterministic gameplay)
    accumulator -= fixed_dt
on_update(dt)               (animation sampling, camera, variable gameplay, scripts)
on_render                   (cull → build RenderScene → renderer.render(view))
present
end_frame → job-system drain → swap frame allocators
```
- `fixed_dt` default 1/60 s; render is uncapped/vsync-selectable.
- **Triple-buffered frame resources**: `FRAMES_IN_FLIGHT = 2` (configurable to 3). Per-frame linear
  allocators, per-frame descriptor pools, per-frame command pools, timeline-semaphore fences.

### 5.2 Threading model
- **Main thread**: input, window events, frame orchestration, editor UI, submit.
- **Job system**: work-stealing pool sized to `hardware_concurrency()-1`. All parallelizable work
  (culling, animation sampling, particle sim, asset decode) is expressed as jobs/task-graphs.
- **Render submission**: command lists recorded in parallel across worker threads, one primary per
  thread, merged at submit. Vulkan queues: 1 graphics (+present), 1 async-compute (optional), 1
  transfer (uploads/streaming). Ownership transfers via queue-family barriers.
- **Rules**: no blocking on the main thread except at explicit sync points; no mutex in the render
  hot path — use lock-free queues / per-thread scratch; GPU/CPU sync only via timeline semaphores.

### 5.3 Memory model
- **Allocator hierarchy**: a global `SystemAllocator` (malloc/VirtualAlloc) underpins typed allocators:
  - `ArenaAllocator` (linear bump, `reset()` per scope/frame) — transient per-frame data.
  - `PoolAllocator<T>` / `FreeListAllocator` — fixed-size object churn (components, handles).
  - `StackAllocator` — scoped scratch with markers.
  - `TrackingAllocator` (debug) — tags, leak detection, high-water marks, category budgets.
- **Frame allocators**: each in-flight frame owns an arena reset at frame start; nothing from a frame
  arena outlives the frame. GPU staging uses ring buffers.
- **Handles over pointers**: resources are referenced by 32/64-bit handles (`{index, generation}`),
  not raw pointers, to survive relocation and detect use-after-free.
- **No hidden allocation in hot paths**; containers take explicit allocators; STL allowed in tooling
  but discouraged in engine hot paths (custom containers preferred for control).

### 5.4 Reflection & serialization
- Lightweight compile-time + registration-based reflection (`AE_REFLECT` macros → `TypeInfo` with
  fields, types, offsets, attributes). Drives: editor inspector, JSON (source) and binary (cooked)
  serialization, script binding, and network replication later.
- Source assets serialize to JSON (diff-friendly, human-editable); runtime cooks to a packed binary
  (`.aeasset`) with a versioned header + hash for cache invalidation.

---

## 6. RHI — Render Hardware Interface (Layer 2)

### 6.1 Goals
A thin, explicit, modern abstraction (~Vulkan 1.3 shaped) that other Khronos-style backends could
implement later, but designed for Vulkan first. **No leaking of `Vk*` types** through public headers.

### 6.2 Core objects (public interface, backend-agnostic)
```
rhi::Instance         create/destroy, layers, debug messenger
rhi::PhysicalDevice   enumerate, feature/limit query
rhi::Device           logical device, queues, resource factory, frame sync
rhi::Swapchain        surface, images, acquire/present, resize
rhi::CommandList      begin/end, dynamic rendering, binds, draws/dispatches, barriers
rhi::Buffer           usage flags, VMA-backed, mapped/staged; typed views
rhi::Texture          1D/2D/3D/cube/array, mips, usage; ImageView cache
rhi::Sampler          filter/wrap/anisotropy/compare
rhi::Pipeline         graphics/compute/(mesh); PSO cache keyed by hash
rhi::PipelineLayout   push constants + descriptor set layouts
rhi::DescriptorSet    + bindless global set (descriptor indexing)
rhi::Shader           SPIR-V module + reflected interface
rhi::QueryPool        timestamps / occlusion / pipeline stats
rhi::Fence/Semaphore  timeline-first synchronization
```

### 6.3 Key design decisions (RHI)
- **Dynamic rendering** (`VK_KHR_dynamic_rendering`, core 1.3) — no `VkRenderPass`/`VkFramebuffer`
  objects; simplifies the render graph enormously.
- **Bindless** via `VK_EXT_descriptor_indexing` (core 1.2): one large global descriptor set for
  textures/buffers/samplers; materials index into it. Push constants carry indices.
- **Timeline semaphores** (core 1.2) for all CPU↔GPU and queue↔queue sync.
- **`synchronization2`** (core 1.3) for barriers.
- **PSO hashing + on-disk pipeline cache** to amortize compile cost.
- **Descriptor/layout automation** from SPIRV-Reflect: shaders declare bindings; RHI builds layouts.
- **Validation**: `VK_LAYER_KHRONOS_validation` enabled in Debug via loader; debug-utils names on all
  objects for RenderDoc/validation clarity. Gracefully absent if layer not installed.
- **Feature tiers**: query and gate optional features (mesh shaders, RT pipeline, descriptor buffer)
  behind `DeviceFeatures` flags; renderer paths branch on tiers.

### 6.4 Bring-up milestone (RHI M0)
Instance+debug → device+queues → VMA → swapchain (dynamic rendering) → per-frame command recording →
clear to color → hardcoded triangle (embedded SPIR-V) → present, resize-safe, validation-clean.

---

## 7. Renderer (Layer 3)

### 7.1 Architecture: a render graph over the RHI
- **Frame graph**: passes declare read/writes on virtual resources; the graph computes barriers,
  transient resource aliasing, and execution order. Transient textures/buffers are pool-allocated
  and aliased by lifetime. This is the backbone that makes adding GI/shadows/post modular.

### 7.2 Shading path: Clustered Forward+ (M1), with a deferred/visibility path planned (M3)
- **Clustered forward+**: cluster the view frustum (e.g., 16×9×24 froxels), assign lights per cluster
  via a compute pass, shade in the forward pass. Scales to thousands of dynamic lights without a
  fat G-buffer, plays well with MSAA/transparency, and is a solid base for correct PBR.
- **Material model**: metallic-roughness PBR (Cook-Torrance GGX, multiscatter energy comp), normal
  mapping, emissive, clearcoat/anisotropy as extensions. IBL via prefiltered env + irradiance +
  BRDF LUT. Material = shader permutation (feature bits) + parameter block (bindless texture indices).

### 7.3 Feature roadmap (renderer, phased)
| Phase | Features |
|---|---|
| M1 | Clustered forward+ PBR, directional + point/spot lights, cascaded shadow maps, HDR, ACES tonemap, bloom, TAA, SSAO, skybox/IBL, debug line/wire draw |
| M2 | GPU-driven culling (compute frustum + Hi-Z occlusion), indirect draw, mesh-shader meshlet path (Nanite-lite), material batching, transparency (OIT or sorted) |
| M3 | Visibility-buffer deferred, dynamic GI (SSGI → surfel/voxel), reflections (SSR → RT), volumetric fog, decals, virtual shadow maps |
| M4 (stretch) | Hardware ray tracing (RT reflections/GI/AO), path-traced reference mode, DLSS/FSR-style upscaling hook |

### 7.4 Shader system
- Author in GLSL (M1) with `#include` support; compile to SPIR-V via glslang at build time
  (`aether_compile_shaders`) and optionally at runtime for hot-reload. HLSL via DXC is supported as an
  alternate front-end. SPIRV-Reflect generates binding metadata. Permutations via `#define` feature
  flags managed by a shader-variant system (hash of defines → cached SPIR-V).

### 7.5 The renderer/scene boundary
The renderer consumes a **`RenderScene`** snapshot (immutable per frame): arrays of draw items
(mesh handle, transform, material, flags), lights, camera/view, environment. Scene/gameplay layers
*produce* this DTO; the renderer never reaches into ECS. This keeps Layer 3 modules decoupled and
enables render-thread parallelism and future record-ahead.

---

## 8. Scene, ECS & gameplay (Layers 3–4)

### 8.1 ECS (EnTT-backed, wrapped)
- `World` wraps an `entt::registry`. Public API exposes `Entity` (a strong handle), component
  add/get/remove, views/queries, and system scheduling — **EnTT types stay private** so we can swap
  implementations. Systems are ordered, grouped (fixed vs variable), and parallelizable.
- **Core components**: `Transform` (local TRS), `WorldTransform` (cached matrix), `Hierarchy`
  (parent/children, dirty flags), `Name`, `Tag`, `Visibility`, `MeshRenderer` (mesh+material handles),
  `LightComponent`, `CameraComponent`. Physics/anim/script add their own.
- **Transform system**: dirty-flag propagation, topological update of world matrices each frame,
  SoA-friendly for parallel update.

### 8.2 Gameplay framework
- `Application` + `GameLoop` orchestrate subsystems (§5.1). A thin **Actor** convenience layer sits
  atop ECS for authoring ergonomics (an Actor = entity + helper API), but ECS remains the substrate.
- **Input**: device polling in core (`Input`), mapped to **actions/axes** via an input-map asset;
  gameplay/scripts subscribe to actions. **Camera**: `CameraController` (fly/orbit) for editor + samples.
- **Serialization**: `SceneSerializer` reads/writes worlds as JSON (source `.aescene`) and cooked binary,
  reflection-driven so new components are automatically covered.

---

## 9. Assets & content pipeline (Layer 3)

### 9.1 Model
- **Source assets** (DCC-authored: `.gltf/.glb`, `.png/.jpg/.hdr/.ktx2`) live under a project's
  `content/`. **Cooked assets** (`.aeasset`) are engine-native, memory-mappable, versioned binaries.
- **Asset DB**: maps a stable **GUID** (128-bit) → source path + import settings + cooked output +
  content hash. `AssetHandle<T>` is a typed, refcounted, generational handle; loading is async.
- **Importers**: glTF (cgltf) → meshes/materials/scenes/skeletons/animations; images (stb/KTX2) →
  GPU textures (with mip/compression); produce cooked outputs deterministically.
- **Cooker CLI** (`tools/cooker`): batch-imports `content/` → `cooked/`, parallel via job system,
  incremental via content hashing. The editor triggers cooks on change (hot-reload watches files).
- **Streaming**: mesh/texture data streams on the transfer queue; residency managed by budgets and
  distance/importance; mips streamed progressively.

### 9.2 Runtime asset types (M1)
`Mesh` (vertex/index buffers + submesh + bounds + optional meshlets), `Texture`, `Material`
(shader + params + texture handles), `Shader`, `Skeleton`, `AnimationClip`, `Scene`, `Prefab`.

---

## 10. Physics & animation (Layer 4)

### 10.1 Physics — Jolt integration
- `PhysicsWorld` wraps Jolt's `PhysicsSystem`; fixed-step (`on_fixed_update`) with interpolation for
  rendering. Components: `RigidBodyComponent` (static/kinematic/dynamic), `Collider` (box/sphere/
  capsule/convex/mesh/heightfield), `CharacterController`, joints/constraints. Bridges: ECS transform
  ↔ Jolt body sync; collision/trigger events → gameplay callbacks. Queries: ray/shape cast, overlap.
  Debug draw via renderer's line API. Jolt types stay **private** to the module.

### 10.2 Animation
- `Skeleton` (bind pose, bone hierarchy, inverse-bind matrices), `AnimationClip` (sampled tracks with
  interpolation), `Pose` (local/global transforms). **AnimGraph**: blend trees (lerp/additive) +
  state machine with transitions; parameters driven by gameplay/scripts. **2-bone IK** (foot/hand),
  **root motion** extraction, **skinning** on the GPU (compute or vertex, dual-quat optional).
  `SkinnedMeshComponent` ties skeleton + clips + mesh; palette uploaded per frame to bindless buffer.

---

## 11. Scripting & the editor (Layers 4–5)

### 11.1 Scripting — Lua 5.4 + sol2 (M1)
- `ScriptVM` per world; `ScriptComponent` binds a Lua module with lifecycle callbacks
  (`on_start/on_update/on_fixed/on_destroy`). Bindings expose: entity/component access, math, input
  actions, physics queries, spawn/destroy, logging, timers, events. **Hot-reload** scripts on file
  change. Sandboxed (no `os`/`io` in shipping). Roadmap: C# (via a managed host) and a node-based
  **visual scripting** graph that compiles to the same command interface.

### 11.2 Editor — Dear ImGui docking application
- **Panels**: Viewport (renders the world to an offscreen target, gizmos via ImGuizmo), Scene
  Hierarchy (tree, drag-reparent), Inspector (reflection-driven property editing + undo/redo command
  stack), Asset Browser (thumbnails, import, drag-to-scene), Console/Log, Profiler (Tracy/ImPlot
  frame graph), Content cook status. **Play-In-Editor**: enter/exit play, world snapshot/restore.
- **Gizmos**: translate/rotate/scale, snapping, local/world. **Selection**: GPU id-buffer picking.
- Editor is a separate executable linking every module; the runtime player is a slimmer executable
  (no ImGui, loads a cooked project).

---

## 12. Coding standards (enforced)

- **Language**: C++20. No RTTI in engine hot code (use our reflection). Exceptions **off** in engine
  runtime (`/EHsc-` where feasible; error handling via `Result<T>`/status codes + asserts); exceptions
  permitted in tooling/importers only. No `new/delete` in hot paths — use allocators.
- **Naming**: `namespace aether` (alias `ae`), one sub-namespace per module (`aether::rhi`). Types
  `PascalCase`; functions/methods/variables `snake_case`; constants/enum values `PascalCase`; macros
  `AE_UPPER_SNAKE`; member fields `snake_case_` (trailing underscore) for private, plain for public
  PODs. Files `snake_case.{h,cpp}`. Include guards via `#pragma once`.
- **Headers**: public API in `include/aether/<module>/`. Minimize includes; forward-declare; PIMPL for
  heavy/private deps. No third-party headers in public headers except math/log in `core`.
- **Errors**: `AE_ASSERT`/`AE_VERIFY`/`AE_CHECK` (configurable: abort in Debug, log in Release);
  `Result<T, Error>` for recoverable failures. All Vulkan calls checked via `VK_CHECK`.
- **Style**: `.clang-format` (LLVM-based, 4-space, 100-col) is authoritative; `.clang-tidy` for lint.
  `/W4` warnings, `/WX` in CI. `const`-correct, `[[nodiscard]]` on factories/getters, `enum class`.
- **Ownership**: prefer values/handles; `unique_ptr` for exclusive heap ownership; `shared_ptr` only
  where sharing is real (assets). Rule of zero; `= delete` copies on resource wrappers; explicit moves.
- **Concurrency**: document thread-affinity of every public API (`// thread-safe` / `// main-thread`).
  No global mutable state without synchronization; prefer passing context objects.
- **Tests**: every non-trivial algorithm gets a doctest; renderer features get a golden-image test.
- **Commits**: conventional-commits style (`feat(rhi): …`), small and focused, build must pass.
- **Docs**: every public header has a file-level comment; non-obvious functions documented. Each ADR
  captures a significant decision in `docs/adr/NNNN-title.md`.

---

## 13. Third-party dependencies (pinned)

| Library | Purpose | License | Integration |
|---|---|---|---|
| Vulkan-Headers | Vulkan API headers | Apache-2.0 | FetchContent (pinned tag) |
| volk | Vulkan meta-loader | MIT | FetchContent |
| VulkanMemoryAllocator | GPU allocator | MIT | FetchContent (header) |
| glfw | window/input | zlib | FetchContent |
| glm | math (bring-up/reference) | MIT | FetchContent |
| glslang | GLSL→SPIR-V | BSD/Apache-2.0 | FetchContent (SPIRV-Tools dep) |
| SPIRV-Reflect | shader reflection | Apache-2.0 | FetchContent |
| EnTT | ECS | MIT | FetchContent |
| Jolt Physics | physics | MIT | FetchContent |
| Lua + sol2 | scripting | MIT | FetchContent |
| Dear ImGui (docking) | editor UI | MIT | FetchContent |
| ImGuizmo | transform gizmos | MIT | FetchContent |
| ImPlot | plots/profiler UI | MIT | FetchContent |
| cgltf | glTF import | MIT | vendored single-header |
| stb_image / stb_image_write | image import/export | MIT/public-domain | vendored |
| KTX-Software | KTX2/Basis textures | Apache-2.0 | FetchContent (M2) |
| doctest | unit tests | MIT | FetchContent |
| Tracy | profiler | BSD-3 | FetchContent (opt-in) |
| spdlog | logging backend (opt) | MIT | FetchContent (opt) |

All versions pinned by git tag/commit in `cmake/Dependencies.cmake`; no floating `master`.

---

## 14. Milestones & roadmap

> Each milestone ends with a **runnable artifact** and green build/tests. This session targets **M0 → most of M1**.

- **M0 — Foundation (this session):** repo + build system green; `aether.core` (math, memory,
  containers, log, jobs, window/input); RHI + Vulkan bring-up (clear + triangle, validation-clean);
  ECS/scene skeleton; asset stubs; empty editor window with ImGui + docking. **Artifact:** a window
  that opens, clears, draws a triangle, with an ImGui overlay showing FPS.
- **M1 — Vertical slice:** load a glTF scene; clustered forward+ PBR with IBL, shadows, TAA, bloom,
  tonemap; fly camera; editor viewport + hierarchy + inspector + asset browser + gizmos; Jolt drop-a-box;
  skeletal mesh playing a clip; a Lua script moving an entity. **Artifact:** editor loads Sponza-like
  scene, lit, navigable, with a physics box and an animated character, script-driven.
- **M2 — GPU-driven + tooling depth:** indirect draws, GPU culling + Hi-Z, meshlet/mesh-shader path,
  streaming, KTX2, undo/redo, PIE, prefabs, blend trees + state machine, character controller.
- **M3 — Fidelity:** visibility-buffer deferred, dynamic GI (SSGI→surfel), SSR, volumetrics, virtual
  shadow maps, decals; profiler + memory tooling; hot-reload everywhere.
- **M4 — Stretch:** hardware ray tracing, reference path tracer, upscaling, C#/visual scripting,
  Linux backend parity, networking/replication foundation.

---

## 15. Parallel work breakdown (orchestrator → Fable 5 subagents)

**Wave 0 (orchestrator, serial):** §3 repo skeleton, root CMake + presets, `cmake/*.cmake`,
`Dependencies.cmake`, `.clang-format`, `.gitignore`, and a **stable public-header contract stub** for
`core` and `rhi` so downstream modules can compile against interfaces immediately. Verify configure step.

**Wave 1 (parallel, depend only on Wave 0 contracts):**
- A: `aether.core` implementation (math, memory, containers, log, jobs, window/input, reflection, time).
- B: `aether.rhi` interface finalize + Vulkan backend to triangle bring-up.
- C: `aether.scene` (ECS wrapper, transforms, components, serializer scaffolding).
- D: `aether.assets` (asset DB, handles, glTF/stb importers, cooker CLI skeleton).

**Wave 2 (parallel, depend on Wave 1):**
- E: `aether.renderer` (render graph, clustered forward+, PBR, shadows, post) — needs core+rhi.
- F: `aether.physics` (Jolt bridge) — needs core+scene.
- G: `aether.animation` (skeleton/clips/blend/skinning) — needs core+scene(+assets).
- H: `aether.scripting` (Lua/sol2 bindings) — needs core+scene.

**Wave 3 (parallel/serial as deps allow):**
- I: `aether.gameplay` (Application/loop/input-map/camera) — needs core+scene+renderer.
- J: `aether.editor` (ImGui panels, viewport, inspector, gizmos) — needs all.
- K: `samples/runtime` vertical-slice app + golden-image tests.

**Orchestrator duties:** own all shared files (root CMake, `Dependencies.cmake`, cross-module headers),
integrate each wave, run the build after every merge, resolve interface drift, and re-task/fix any
subagent output that fails to compile, violates §4/§12, or misses its contract. No wave is "done"
until it builds and its acceptance check passes.

### 15.1 Per-module acceptance checklist (Definition of Done)
1. Configures + compiles under MSVC `/W4` with no new warnings.
2. Conforms to §4 dependency rules (no illegal includes) and §12 standards.
3. Public headers self-contained (include-what-you-use); no third-party leakage beyond the allowed set.
4. Has at least a smoke test (doctest) and, where visual, a golden-image or manual-verify note.
5. Exposes exactly the public surface listed in §4.2; documents thread-affinity.
6. Integrated into the root build and the sample/editor as applicable.

---

## 16. Risks & mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Scope (rivaling UE) | Never "done" | Milestone gating; each M ships a runnable artifact; features are additive behind the render graph. |
| No Vulkan SDK on build box | Can't compile Vulkan | volk + Vulkan-Headers (no SDK); runtime `vulkan-1.dll` present; validation optional. |
| Parallel subagents editing shared files | Merge conflicts/build breaks | Orchestrator owns shared files; strict module dir ownership; interface stubs first; build after every merge. |
| Interface drift between modules | Integration failures | Public-header contracts frozen in Wave 0; changes go through orchestrator + ADR. |
| Vulkan validation/driver quirks (Intel Arc) | Rendering bugs | Validation layers in Debug, RenderDoc captures, feature-tier gating, conservative extension use. |
| Build time bloat | Slow iteration | PCH, strict includes, static libs, unity-build option, ccache-like caching later. |
| Third-party breakage on pinned bump | Build breaks | Pin exact tags/commits; bump deliberately with a build check. |
| Determinism (physics/gameplay) | Networking/replay later | Fixed-step, fixed-order systems, avoid FP nondeterminism in sim, seedable RNG. |

---

## 17. Change log
- **v1.0** — Initial master blueprint authored (pre-Fable-5-review).
- **v1.1** — Applied Fable 5 review (ADR-0001 below). All items below are **authoritative amendments**; where they conflict with §1–§16, the amendment wins.
- **v1.2** — ADR-0002: Opus 5.5 orchestration, isolated per-agent build trees, expanded frozen contracts, UNORM swapchain.
- **v1.3** — ADR-0003: gameplay becomes the engine assembly layer (4b); `Application` contract frozen for Wave B.
- **v1.4** — ADR-0004: Wave A closed (scripting, animation tests), gameplay implemented, M1 vertical slice, Linux/llvmpipe headless verification path.
- **v1.5** — ADR-0005: the editor (Wave B complete).
- **v1.6** — ADR-0006: the runtime player, project manifests and packaging; `AppDesc` cooked-asset fields.
- **v1.7** — ADR-0007: golden-image render tests; `rhi::read_texture_rgba8` readback.
- **v1.8** — ADR-0008: M1 baseline verified on Windows/MSVC/Intel Arc (build, tests, validation-clean runs, first hardware shadows, Arc golden references, Windows packaging).
- **v1.9** — ADR-0009: M2 scope, additive contracts, three-agent wave.
- **v1.10** — ADR-0010: M2 wave verified on Linux; meshlet mesh-shader path (ADR-0009 graphics stretch 1).
- **v1.11** — ADR-0011: asset-browser thumbnails (ADR-0009 editor stretch); toolbar / layout fixes.
- **v1.12** — ADR-0012: spot-light shadows (ADR-0009 graphics stretch 2); delivery as a downloadable local app made explicit.
- **v1.13** — ADR-0013: installable build — install rules, CPack ZIP + Inno Setup installer, Documents workspace.
- **v1.14** — ADR-0014: application identity (icon, version info, splash), File > New / Open Project, precompiled shaders.
- **v1.15** — ADR-0015: point-light shadows (six-view cube shadows sharing the spot-shadow path).
- **v1.16** — ADR-0016: dynamic diffuse global illumination (irradiance probe volume, GI Volume component).
- **v1.17** — ADR-0017: reflection capture probes (box-projected prefiltered cubemaps, Reflection Probe component).
- **v1.18** — ADR-0018: visual scripting (Blueprint-style node graphs compiled to Lua, node editor).

---

## ADR-0001 — Fable 5 review resolution (authoritative amendments)

**Build environment**
- **Out-of-OneDrive build:** `CMakePresets.json` sets `binaryDir` = `C:/Users/paulc/.aether/build/${presetName}` and `FETCHCONTENT_BASE_DIR` = `C:/Users/paulc/.aether/deps`. Source stays in the OneDrive repo; nothing generated is written there. `.gitignore` excludes `build/`, `desktop.ini`, `*.user`.
- **MSVC activation:** all builds go through `scripts/build.ps1` (imports `vcvars64.bat` env, then runs the bundled CMake+Ninja). Agents never assume `cl.exe` is on PATH. Orchestrator is the build authority; breadth agents author to contract and do **not** run global builds (avoids concurrent-Ninja thrash on the shared `binaryDir`).
- **Python:** confirmed present (3.14.2) → glslang FetchContent configure works. glslang fetched with `ENABLE_OPT=OFF`, `ENABLE_HLSL=OFF`, tests/examples off (drops SPIRV-Tools, cuts build time). shaderc/DXC deferred.

**Language / build policy**
- **Exceptions ON** (`/EHsc`) globally through M1 (MSVC STL + sol2 + doctest require it). Convention — engine code never throws; recoverable errors via `Result<T,E>`. Selective `/EHs-c-` revisited post-M1.
- **Warnings** applied **only** via `aether_add_module()` (`target_compile_options … PRIVATE /W4`). `/WX` is opt-in via `-DAE_WERROR=ON` (OFF during active dev). All FetchContent deps declared with `SYSTEM`. No global `CMAKE_CXX_FLAGS` mutation (hard orchestrator rule).
- **Math (M0–M1):** `core/math.h` provides **glm-backed aliases** (`Vec2/3/4`, `Mat3/4`, `Quat`, `AABB`). Custom SIMD math deferred past M1.
- **Containers (through M1):** `std::` types allowed in public APIs (behind `aether::` aliases where cheap). Custom containers deferred.
- **Float policy:** sim/deterministic code compiles `/fp:precise` (never `/fp:fast`).

**Layering corrections (supersede §4.2/§5.1/§8.1/§9–§11 where noted)**
- **Shared vocabulary moves to `aether.core`:** `Handle<T>{index,generation}`, `AssetId` (128-bit GUID), `Result<T,E>`, `Error`, the assert/`VK_CHECK` macros, `Log`, `AE_REFLECT` (compiling no-op with final syntax), `ISubsystem` + `EngineContext`/`FrameContext`/`FixedContext`/`RenderContext`, `Window`, `Input`, and time. These are frozen in Wave 0.
- Scene components store **plain typed IDs** (`AssetId` / renderer-owned handles), never `AssetHandle<T>` (removes scene→assets sideways dep).
- **`assets` owns data types** (`MeshData`, `SkeletonData`, `AnimationClipData`); **`animation` owns runtime types** (`Skeleton`, `Pose`, `AnimGraph`). `animation` may depend on `assets`.
- **RenderScene uses renderer-owned handles**, not asset handles. The renderer exposes `register_mesh/register_material/register_texture` → opaque handles; the asset→render bridge lives in `gameplay`. This keeps `renderer` decoupled from `assets` (both Layer 3).
- **Anim & physics never touch the GPU directly.** Animation writes CPU skinning palettes into `RenderScene`; physics emits debug lines into `RenderScene.debug_lines`; the **renderer** owns all GPU uploads. (Supersedes §10.1/§10.2 GPU wording and Wave 2 F/G dep notes.)
- **ImGui/Vulkan escape hatch:** the ImGui platform+Vulkan backend lives **inside `rhi`** behind `rhi::imgui_init/new_frame/render`. No `Vk*` type ever reaches the editor. Same hatch serves viewport-texture-as-ImGui-image.

**RHI / Vulkan correctness**
- **WSI sync:** binary semaphores for `vkAcquireNextImageKHR`/`vkQueuePresentKHR`; timeline semaphores for all *internal* CPU↔GPU/queue↔queue sync. (Supersedes §5.2/§6.3 "timeline-only".)
- **`VK_CHECK`** aborts only on must-succeed calls. `OUT_OF_DATE_KHR`/`SUBOPTIMAL_KHR` from acquire/present return a `Result` handled as resize (recreate swapchain), not an assert.
- **volk:** `VK_NO_PROTOTYPES` on every target seeing Vulkan headers; `volkInitialize`→`volkLoadInstance`→`volkLoadDevice` (global table for M0–M2). VMA configured `VMA_STATIC_VULKAN_FUNCTIONS=0`, `VMA_DYNAMIC_VULKAN_FUNCTIONS=1` with a filled `VmaVulkanFunctions`. `glfwInitVulkanLoader(vkGetInstanceProcAddr)` before window creation. ImGui backend uses `ImGui_ImplVulkan_LoadFunctions`.
- **Surface ownership:** `core::Window` exposes `void* native_handle()` (HWND); `rhi` creates the surface via `vkCreateWin32SurfaceKHR` (`VK_USE_PLATFORM_WIN32_KHR`). Wave 0 ships a **working** GLFW window, not a stub.
- **Swapchain:** sRGB format (`VK_FORMAT_B8G8R8A8_SRGB`) with ImGui gamma-aware path; honor `minImageCount`; handle zero-size/minimized (skip render). `FRAMES_IN_FLIGHT` (CPU/GPU overlap, =2) is independent of swapchain image count.
- **Intel Arc reality:** do **not** assume a dedicated transfer/compute queue family; degrade to a single graphics+compute+transfer family (no queue-ownership barriers on this box — flag untested paths). `wideLines` unsupported → 1px or geometry-expanded debug lines; check `fillModeNonSolid` before wireframe. 1.3-core (dynamic rendering, sync2, timeline, descriptor-indexing update-after-bind) all supported.
- **Validation:** SDK layers absent on this machine. Validation is **best-effort**: enabled if `VK_LAYER_KHRONOS_validation` is discoverable (optionally provisioned via prebuilt binaries + `VK_LAYER_PATH`), else skipped with a warning. M0 acceptance no longer claims "validation-clean" — it claims "runs clean; validation when provisioned."
- **Pipeline cache** validates vendor/device/driver UUID before reuse (Arc driver churn).

**Assets / paths**
- **exe-relative root discovery** for `shaders/`, `assets/`, `content/` from the first frame (no CWD-relative paths). A `core::paths` helper provides `engine_root()`, `shader_dir()`, `asset_dir()`.
- **JSON:** `nlohmann/json` added to §13 when the serializer lands (deferred past M0).
- **Lua:** use a CMake-wrapped mirror (walterschell/Lua) or vendor the C files; sol2 confined to a single `scripting` TU.

**Testing / CI**
- Golden-image references are **machine-local**, fuzzy-compared (per-channel tolerance + max-bad-pixel budget) until GPU-less CI exists (CI = compile-only, deferred).
- `hardware_concurrency()` clamped `max(1, n-1)`.

---

## 18. Execution model for this session (two-tier orchestration)

Reconciles the reviewer's "guarantee a runnable artifact" with the mandate for broad parallel Fable-5 delegation.

**Tier 1 — Critical path (orchestrator-owned, guarantees the artifact):**
Wave 0 (repo + build system + **frozen contract headers** + working GLFW window) → `aether.core` minimal impl → `aether.rhi` Vulkan bring-up → `samples/sandbox` (window, clear, triangle via glslang-compiled SPIR-V, ImGui FPS overlay, resize/minimize-safe, clean exit). The orchestrator writes/integrates/builds this personally.

**Tier 2 — Breadth agents (parallel Fable 5, author standalone static libs against frozen contracts):**
`scene` (EnTT wrapper + transforms + components), `assets` (DB + handles + glTF/stb import skeleton + cooker CLI), `renderer` (render-graph + clustered-forward+ + PBR building blocks consuming `RenderScene`), `physics` (Jolt bridge), `animation` (skeleton/clip/blend/skinning), `scripting` (Lua/sol2). Each **must compile as `aether.<mod>` against the frozen `core`/`rhi` headers** with a smoke test; wiring into `sandbox`/editor is best-effort and integrated by the orchestrator as pieces land. Nothing here may block Tier 1.

**Orchestrator loop:** freeze contracts → fan out Tier 2 in parallel → build Tier 1 to the artifact → as each Tier-2 lib lands, integrate + build + fix/re-task on failure. A module is "done" only when it satisfies the §15.1 checklist and builds in the tree.

*(Amendments beyond ADR-0001 are appended below with rationale.)*

---

## ADR-0002 — Orchestration model, build isolation, contract expansion (2026-09-25)

**Context.** Orchestration moved to Opus 5.5 by user directive; all subagents are now Opus 5.5.
The user's mandate is that *all* implementation is delegated, with the orchestrator coordinating,
integrating, verifying, and stepping in on failure. This supersedes §18's "orchestrator writes the
critical path personally".

**Decisions**
1. **Delegation.** Every module, including the critical path (core + rhi + sandbox), is implemented by
   an Opus 5.5 subagent. The orchestrator owns: frozen contracts, shared build files, `docs/`,
   integration builds (`msvc-x64-*` presets), cross-module fixes, and re-tasking failed work.
   Ownership map and rules: `docs/AGENT_GUIDE.md`.
2. **Build isolation (fixes an ADR-0001 defect).** A shared `FETCHCONTENT_BASE_DIR` is unsafe:
   FetchContent places each dependency's *build* tree there too, so two presets would corrupt each
   other. Dependencies now live in each preset's own `<binaryDir>/_deps` (still outside OneDrive).
   Each agent gets a private `wip-<module>` preset + build tree, so every agent can compile-check and
   unit-test its own module concurrently.
3. **Module filter + dependency optimisation.** `AE_MODULES` limits which modules a preset configures
   (a sibling's half-written module can't break your configure); `CMAKE_OPTIMIZE_DEPENDENCIES=ON` lets
   `aether.<mod>` compile without building its (possibly in-flux) static-lib dependencies.
4. **Per-module dependency files.** Optional third-party deps live in `cmake/deps/<module>.cmake`, each
   owned by that module's agent; fetched only when the module is enabled.
5. **RAM budget.** 22 logical cores but 15.7 GB RAM: WIP build presets cap Ninja at 4 jobs
   (core-rhi 8, gameplay 6).
6. **Swapchain = `B8G8R8A8_UNORM`, display encoding in-shader** (supersedes ADR-0001's sRGB swapchain).
   Rationale: Dear ImGui's colours are sRGB-authored and must be written unmodified; the editor
   viewport image (sampled by ImGui) must hold display-encoded values. The renderer's final pass applies
   the sRGB OETF when its `RenderTarget` format is UNORM, and never when it is `*_SRGB`.
7. **Universal binding model.** One pipeline layout for all pipelines: set 0 = bindless sampled textures
   (binding 0) + bindless storage images (binding 1); 128 B push constants on all stages; buffers via
   buffer device address (`GL_EXT_buffer_reference`). No per-pipeline descriptor layouts.
8. **Swapchain state contract.** `begin_frame` hands out the backbuffer in `Undefined`; the frame must
   leave it in `ColorAttachment`; `end_frame` transitions to `Present`.
9. **New frozen contracts:** `core/geometry.h` (48-byte `Vertex`, 24-byte `SkinVertex`, `Submesh` —
   shared by assets and renderer with zero conversion), `assets/asset_types.h` (Mesh/Texture/Material/
   Skeleton/AnimationClip/Scene data), `renderer/renderer.h` (resource registration, `render()`,
   `RenderTarget`, settings, hot reload). The RHI gained BDA, storage-image bindless, per-mip/layer
   attachments, depth bias/clamp, indirect-count draws, `fill_buffer`, `immediate_submit`, shader compile
   options/includes, and `frames_in_flight()`.
10. **EnTT in scene's public headers** (like glm in core), so the ECS API stays ergonomic.
11. **Real Vulkan validation.** Reversing the review's advice not to build validation layers from source:
    with 22 cores the one-off build is affordable, and correctness of a hand-written Vulkan backend is
    unverifiable without it. `scripts/provision_validation.ps1` builds `VK_LAYER_KHRONOS_validation`
    (tag `vulkan-sdk-1.4.328.0`) into `C:/Users/paulc/.aether/tools/vvl`; `scripts/run.ps1` enables it
    automatically. M0 acceptance is again "validation-clean" once provisioned.
12. **Dependency smoke test.** Before fan-out, every dependency block is compiled and executed once in an
    isolated project (volk init, VMA impl, glslang compile, ImGui context, GLFW init, EnTT+json, cgltf+stb,
    Jolt init, Lua via sol2) so integration failures surface once, not in eight agents at the same time.

**Wave plan (supersedes §15/§18 sequencing)**
- **Wave A (parallel now):** core-rhi (core impl, Vulkan RHI, ImGui, `samples/sandbox`), scene, assets,
  renderer, physics, animation, scripting, validation-layer provisioning.
- **Wave B (after Wave A integration):** gameplay (Application loop, input map, camera, asset→render bridge,
  `samples/vertical_slice`), editor (ImGui docking editor, viewport, hierarchy, inspector, gizmos, PIE).
- **Integration:** orchestrator builds `msvc-x64-debug` with all modules after each wave, runs every test,
  runs the samples under validation, and bounces failures back to the owning agent (or fixes directly).

---

## ADR-0003 — gameplay is the engine assembly layer (Layer 4b)

**Context.** The blueprint placed gameplay in Layer 4 beside physics/animation/scripting, yet gameplay
must assemble the default engine: register those subsystems, copy physics debug lines and animation
skinning palettes into the `RenderScene`, and bind physics queries into Lua. That is a sideways
dependency the layering rules forbid; pushing the glue into every Layer-5 executable (editor, runtime,
samples) would triplicate it.

**Decision.** Split Layer 4:
- **4a** — physics, animation, scripting: depend on core/scene/assets only (never on each other).
- **4b** — gameplay: may depend on everything in Layers 1–4a. Owns `Application` (services + frame
  loop + subsystem registry + simulation control), the asset→render bridge (AssetManager → Renderer
  registration, AssetId → renderer-handle cache), the ECS→RenderScene builder (meshes, lights, camera,
  interpolated physics transforms, skinning palettes, physics debug lines), input action mapping, camera
  controllers, and the Layer-4a script bindings (via scripting's opt-in `lua_integration.h`).
- **5** — editor, runtime, samples derive from `gameplay::Application`.
This mirrors Unreal's `Engine` module sitting above its feature modules.

**Frozen:** `engine/gameplay/include/aether/gameplay/application.h` — so the gameplay and editor agents
can work in parallel in Wave B. Play-in-editor = snapshot the world (scene serializer) → enable
simulation → on stop, `reload_world()` from the snapshot (it shuts down and restarts the Simulation
subsystems around the reload, so per-world state such as physics bodies and script instances is rebuilt).

---

## ADR-0004 — Wave A close-out, gameplay, M1 vertical slice, headless verification (2026-09-26)

**State at session start.** Wave A modules existed except that `scripting` had no CMakeLists, bindings,
subsystem, codec or tests, and `animation/CMakeLists.txt` listed four missing test files (any tree
with tests enabled failed to configure). `gameplay` was headers plus one-line stubs; no editor.

**Done**
- **scripting** completed: `aether.scripting` + `aether::scripting_lua` (opt-in sol2 target),
  engine bindings (math, Entity/Light/Camera, `world`, `input`, `time`, `timer`), "Script" codec,
  `ScriptingSubsystem`, 34 tests (clean under ASan/UBSan), `content/scripts/README.md` API reference.
- **animation** test suite completed (37 cases).
- **gameplay** implemented against the frozen `application.h`. New ADDITIVE public headers (the
  editor needs them): `render_bridge.h`, `scene_instantiation.h`, `component_codecs.h`,
  `components.h` (`MaterialOverridesComponent`), `procedural_mesh.h` (built-in asset ids),
  `animation_bridge.h`, `asset_hot_reload.h`, `environment.h` (procedural HDR sky), `debug_ui.h`.
  Physics / animation / scripting types stay out of gameplay's public headers (hooks instead).
- **samples/vertical_slice** — the M1 artifact; `--check` gates the exit code on physics, scripts,
  animation and rendering actually working.

**Decisions**
1. **Linux is a verification platform now.** The RHI creates its surface through GLFW on non-Windows
   (Win32 path unchanged). On Linux with Xvfb + Mesa llvmpipe (Vulkan 1.4) + `vulkan-validationlayers`
   every test suite runs, and `sandbox` / `vertical_slice --check` run validation-clean. This is the
   GPU-less CI path ADR-0001 deferred. Recipe: `apt install libxrandr-dev libxinerama-dev
   libxcursor-dev libxi-dev libgl-dev mesa-vulkan-drivers vulkan-validationlayers xvfb`, configure with
   `-DGLFW_BUILD_WAYLAND=OFF`, run under `Xvfb :99` with `DISPLAY=:99`.
2. **llvmpipe shadow workaround.** Mesa llvmpipe 25.x crashes in its JIT when fragment shaders sample
   the cascaded shadow map (null sample-function table; every sampling form tried fails). `Application`
   disables shadows only when the adapter is a software rasterizer, with a warning. Shadows still need a
   real-GPU check on the Arc box — the renderer's shadow path has never been exercised on hardware.
3. **Jolt is built with RTTI** (`CPP_RTTI_ENABLED`): GCC/Clang need Jolt typeinfo because
   `aether.physics` derives from Jolt classes. No behavioural change on MSVC.
4. **Cooked asset cache** (`<engine root>/assets/`) is git-ignored.

**Not verified this session:** MSVC build (all new code was compiled with GCC 13 at -Wall -Wextra
-Wpedantic -Wshadow -Wconversion, warning-free) and real-GPU rendering.

**Next (Wave B remainder):** `editor/` (ImGui docking editor: viewport via `set_render_extent` +
`on_render_frame` override, hierarchy, inspector, gizmos, play-in-editor via `reload_world`), the
`runtime/` player, golden-image tests using the headless path, and a real-GPU shadow check.

---

## ADR-0005 — The editor (2026-09-26)

**Shape.** `editor/` builds `aether.editor` (a static library holding `EditorApp` plus the GPU-free
pieces that are unit-tested: `EditHistory`, picking, the log console) and the `aether-editor`
executable. `EditorApp` derives from `gameplay::Application` and overrides `on_render_frame` to render
the world into an offscreen RGBA8 viewport texture (`RenderTarget` final state `ShaderRead`) that a
docked ImGui window displays; `set_render_extent` keeps the renderer at the panel size. Viewport
targets are retired `frames_in_flight + 1` frames after a resize before their ImGui descriptor and
texture are freed.

**Decisions**
1. **Undo = whole-scene snapshots** (`scene::save_scene_to_string`). Exact for every component that
   serializes (module codecs included) and trivially correct for hierarchy / add / remove; cheap at
   editor scene sizes. Selection is kept by uuid because undo reloads the world. Drags and text fields
   collapse into one step (continuous edits); toggles and combos are discrete steps.
2. **Play-in-editor** = snapshot -> `set_simulation_enabled(true)`; Stop = `reload_world` from the
   snapshot (ADR-0003 contract). Edit mode keeps the simulation disabled, so physics bodies and script
   instances only exist while playing; edits made during play are not recorded and vanish on Stop.
3. **Picking is CPU ray vs world AABB** (mesh bounds from the render cache) — no GPU id buffer yet.
4. **ImGuizmo** (MIT, pinned commit in `cmake/deps/editor.cmake`) for translate/rotate/scale gizmos; it
   is fed an OpenGL-style (Y-up NDC) projection, separate from the engine's Vulkan projection.
5. **Built-in materials** (`gameplay::BuiltinMaterial`) join the built-in meshes, so scenes authored in
   the editor render without asset files; **RenderBridgeSubsystem registers a procedural sky** as the
   default environment (skybox + IBL) unless an app sets its own.
6. `aether-editor --self-test` drives a full workflow (create, edit, undo/redo, duplicate/delete,
   physics, reparent, pick, play/stop, save/open, model instancing, scripts) with the UI live; it passes
   headless on llvmpipe with Vulkan validation clean. `content/scenes/showcase.aescene` is a sample scene.

**Not done yet:** GPU id-buffer picking and multi-select, prefab assets, a material editor, asset
thumbnails, drag-from-asset-browser into the viewport, an animation graph editor, the `runtime/`
player (done: ADR-0006), and golden-image tests.

---

## ADR-0006 — The runtime player

**Status:** accepted. **Scope:** `runtime/` (`aether.runtime` + `aether-player`), an additive
`AppDesc` amendment, `game.aeproject`.

**Contract change (additive, `gameplay/application.h`).** `AppDesc` gains two trailing fields with
defaults: `cooked_assets_only` (the Application initialises its AssetManager in
`AssetLoadMode::Runtime`: cooked data only, no source import, no hot reload) and `cooked_root`
(empty => `paths::asset_dir()`). Existing code and aggregate initialisers are unaffected.

**Project manifest (`.aeproject`, JSON, `format: "aether.project"`, version 1).** Name, startup scene
(content-relative), content/cooked roots (relative to the manifest), `cooked_assets`, an optional
InputMap JSON, window (title, size, resizable, vsync, fullscreen), simulation (fixed delta, max steps),
rendering (exposure, sky) and `camera_controller`. Every key but `format` is optional; unknown keys warn
and are ignored so newer manifests still load. The repository root's `game.aeproject` runs the
showcase scene from source during development.

**Player (`aether-player`).** A slim `gameplay::Application`: no ImGui, validation only in debug builds,
the project's input map (else the default camera bindings) plus `Player.Quit` (Esc) and
`Player.ToggleFullscreen` (F11, borderless on the primary monitor via GLFW), and an optional
`CameraControllerSubsystem` (Simulation kind) so `FlyCamera`/`OrbitCamera` scene cameras work. Without
a project argument it runs the `*.aeproject` next to the executable, else the one in the engine root.
`--cooked` / `--source-assets` override the manifest's asset mode.

**Packaging (`aether-player --package <dir>`).** Cooks every importable source into `<dir>/assets`
(the AssetDatabase scan, incremental), copies the runtime-read content (`.aescene`, `.aeprefab`,
`.lua`, `.json`), the shader tree and the player itself, and writes `<dir>/game.aeproject` with
`cooked_assets: true` and roots relative to it. `paths::engine_root()` resolves to `<dir>` (it holds
`shaders/`), so the package is relocatable. Source models and dev-only files are not shipped.

**Verification.** `aether-player --frames N --check` asserts the scene loaded, the scene camera
rendered it, no mesh is pending or failed, every enabled script is running with no errors and
physics created its bodies. Verified on llvmpipe (validation clean): the dev tree from source, and a
package moved to another directory running cooked-only; deleting cooked files makes the check fail.
`test.runtime` covers manifest parsing/round-trip, discovery, `apply_project` and packaging followed
by a Runtime-mode load of the packaged data.

**Not done yet:** archive/pak files (the package is a loose directory), shader precompilation into
the package (shaders still compile at startup), a Windows packaging run, a launcher/splash screen and
save-game support.

---

## ADR-0007 — Golden-image render tests

**Status:** accepted. **Scope:** `tests/golden/` (`aether.golden`, `aether-golden`, `test.golden`),
an additive RHI function, root `CMakeLists.txt` (the `golden` entry of `AE_MODULES`).

**Contract change (additive, `rhi/device_ext.h`).** `read_texture_rgba8(Device&, TextureHandle,
ResourceState)` copies mip 0 of an RGBA8/BGRA8 texture (needs `TextureUsage::TransferSrc`) to the CPU
through `immediate_submit` and a GpuToCpu buffer (invalidated for non-coherent memory), returning the
texture to its state. It is the screenshot / readback primitive; `CommandList` stays frozen.

**Harness.** One process per case (`aether-golden --case <name>`), so a case's image never depends on
which cases ran before it (TAA history, caches). The case builds its scene from built-in meshes and
runtime materials, renders offscreen at 320x240 for 40 frames and compares the final display-encoded
target with `tests/golden/reference/<device class>/<case>.png`. Nothing in the scenes depends on
wall-clock time, so runs are deterministic: on llvmpipe repeated runs are bit-identical. Match rule:
<= 0.2% of pixels beyond a per-channel tolerance of 4 and mean absolute error <= 0.75 (0..255).
A mismatch writes the actual image and a diff (grey = amplified error, red = beyond tolerance).

**References are per device class** (`llvmpipe`, `swiftshader`, else the sanitised adapter name):
drivers legitimately differ, so a device without references reports ctest *skipped* (exit 77) instead
of failing, and `--update` records them. The `shadows` case needs cascaded shadows and skips where the
llvmpipe workaround disables them (ADR-0004), so its first reference must come from a real GPU.

**Cases (M1 renderer features):** PBR metal/roughness sweep with sky IBL and the full post chain; the
same without SSAO/bloom/TAA; punctual point + spot lights with flat ambient; emissive + bloom;
alpha-blended translucency; the Normals / Albedo / Roughness debug views; shadows. ctest entries
`golden.<case>` (labels `gpu;golden`) are registered with `-DAE_GOLDEN_TESTS=ON`.

**Verification.** All eight llvmpipe references were reviewed visually before being committed; a
deliberate 15% roughness change in `forward.frag` failed the four roughness-sensitive cases (the
Normals/Albedo views correctly passed) with diffs localised to the specular lobes; validation clean.

**Not done yet:** references for a real GPU (the Intel Arc dev box), CI wiring, and cases for
skinning, debug lines, cluster light counts at scale and the other debug views.

---

## ADR-0008 — Windows / MSVC / real-GPU verification of the M1 baseline (2026-09-26)

**Context.** ADR-0004..0007 were built and verified on Linux (GCC 13, Mesa llvmpipe). They listed as
unverified: the MSVC build, real-GPU rendering, the cascaded-shadow path (llvmpipe crashes sampling it),
real-GPU golden references and a Windows packaging run. This session closed all of them on the dev box
(Windows 11, MSVC 14.44, Intel Arc integrated, driver 101.8991, Vulkan 1.4.356, source-built
VK_LAYER_KHRONOS_validation 1.4.328).

**Results**
| Check | Result |
|---|---|
| `msvc-x64-debug`, all 13 modules (core..scripting, gameplay, editor, runtime, samples, golden) | green, **0 warnings** in engine code |
| ctest (13 suites incl. `assets.cook_samples`) | **13/13 pass** (18 s) |
| `sandbox --frames 300 --test-resize` | exit 0, validation 0 errors / 0 warnings, 0 leaks |
| `vertical_slice --frames 600 --check` | PASSED, 0 errors / 0 warnings / 0 VUIDs; shadows ON (first hardware run) |
| `aether-editor --self-test` | PASSED, validation clean (2 WARN = intended self-parent rejection) |
| `aether-player --frames 600 --check` | PASSED, validation clean |
| golden cases on Arc (9, incl. `shadows`) | recorded after visual review to `tests/golden/reference/intel_r_arc_tm_graphics/`; compare re-run **bit-identical** (max diff 0) |
| Arc vs llvmpipe | debug views, emissive_bloom, punctual_lights identical (MAE <= 0.01); PBR / translucency differ only by the shadows llvmpipe disables |
| `aether-player --package` on Windows | 3 sources -> 15 cooked assets, 42 files; package run from `C:\` cooked-only: self-check PASSED, validation clean, engine root = package dir |

**Fixes made during verification**
- `engine/animation/tests/test_graph.cpp`: include `<ostream>` — MSVC's `operator<<(ostream&, string_view)`
  needs the complete `basic_ostream` when doctest stringifies a `string_view` (libstdc++ provides it
  transitively). The only MSVC break in ~16k lines of GCC-verified code.
- `scripts/run.ps1`: documented invocation drops the `--` separator (Windows PowerShell 5.1 rejects it).

**Status.** M0 and M1 are complete and verified on both platforms. The cascaded-shadow path is verified on
hardware but not on llvmpipe (workaround stays). Next: M2 (ADR-0009).

---

## ADR-0009 — M2: GPU-driven rendering and tooling depth (2026-09-26)

**Context.** M0/M1 are complete and verified on Linux/llvmpipe and Windows/Intel Arc (ADR-0008). Already
done from the original M2 list: undo/redo, play-in-editor, blend trees + state machines, character
controller. Remaining M2 scope plus the highest-value "not done yet" items from ADR-0004..0007 and the
module reports, split into three agents with disjoint ownership (docs/AGENT_GUIDE.md).

**Additive contract changes (orchestrator, compiled green across all 13 modules before fan-out)**
- `render_scene.h`: `RenderMeshInstance::user_id` (0 = not pickable; gameplay writes entity index + 1).
- `renderer.h`: `TextureUpload::mip_levels` (pre-built chains; required for BCn); `RendererSettings::
  gpu_culling`, `occlusion_culling`; GPU culling stats; `Renderer::request_pick(UVec2)` /
  `poll_pick()` (async, non-pure defaults so existing fakes/mocks keep compiling).
- `asset_types.h`: `TextureFormat::BC7_SRGB`, `BC7_UNORM`, `BC5_UNORM` (cooker output, all mips present).
- `rhi/enums.h`: `Format::BC7Unorm`.
- gameplay glue (orchestrator): the render bridge fills `user_id`, maps the BC formats and passes cooked
  mip chains through (`generate_mips` only when a single uncompressed mip is supplied).
- Build: `cmake/deps/renderer.cmake` hook (OPTIONAL include); presets `m2-graphics`, `m2-assets`, `m2-editor`.

**Agents and scope**
1. **Graphics** (rhi + renderer + renderer shaders):
   GPU-driven opaque path (GPU instance/draw buffers, compute frustum culling, `drawIndexedIndirectCount`,
   material/pipeline batching); two-phase Hi-Z occlusion culling; picking id buffer + async readback
   (`request_pick`/`poll_pick`); `TextureUpload::mip_levels` + BC1/3/5/7 uploads (`BC7Unorm` mapping);
   `VertexAttribute::binding` (requested by renderer and RHI); JobSystem header docs. Stretch, in order:
   mesh-shader meshlet path gated on `DeviceFeatures::mesh_shaders` (meshoptimizer via
   `cmake/deps/renderer.cmake`), spot-light shadows.
2. **Assets**: MikkTSpace tangents; cooker mip generation (sRGB-correct, box/Kaiser); BC7 (color / linear) +
   BC5 (normals) compression in the cooker; animation of non-joint nodes; texture de-duplication (glTF image
   also present as a standalone file); cooker mtime fast path.
3. **Editor**: GPU picking via `request_pick` (CPU AABB fallback when unsupported); multi-select + group
   transform; prefab assets (`.aeprefab`: create from selection, instantiate, drag in); asset-browser
   drag-and-drop into viewport/hierarchy; material editor (`MaterialOverridesComponent` / built-ins); grid +
   snapping; read-only animation state-machine view. Stretch: asset thumbnails.

**Acceptance gates (orchestrator re-verifies on the Arc before accepting)**
- `msvc-x64-debug` all modules green, 0 warnings; ctest all pass.
- `vertical_slice --check`, `aether-editor --self-test`, `aether-player --check` pass validation-clean on the Arc.
- Arc golden cases pass unchanged — the GPU-driven path must reproduce the CPU path's images. Any intended
  visual change is re-recorded only after the images are reviewed, and reported.
- New features carry tests (CPU where possible; golden cases for visual features).

---

## ADR-0010 — M2 wave verified on Linux; the meshlet mesh-shader path (2026-09-27)

**Context.** The ADR-0009 wave (commit 851c037) landed without a close-out. Re-verified here on Linux
(GCC 13, Mesa llvmpipe 25.2 / Vulkan 1.4, which exposes VK_EXT_mesh_shader): all 13 modules build,
ctest 21/21 pass + `golden.shadows` skipped (llvmpipe), `vertical_slice --check`, `aether-editor
--self-test` and `aether-player --check` pass validation-clean, and the GPU-driven path (GPU frustum +
two-phase Hi-Z + indirect draws) is active, not falling back. All ADR-0009 core scope is present with
tests (GPU culling / Hi-Z / picking, BC + mips + MikkTSpace + node animation + texture de-dup + mtime
fast path, editor picking / multi-select / prefabs / drag-drop / material editor / snapping / anim
view). Not done from ADR-0009: graphics stretch 2 (spot-light shadows — cannot be verified on
llvmpipe, whose shadow sampling crashes; needs the Arc) and editor stretch (thumbnails).
Environment note: this sandbox's proxy rejects GitHub archive tarballs, so MikkTSpace / bc7enc_rdo were
provided via `FETCHCONTENT_SOURCE_DIR_*` clones at the pinned commits (no build-file change).

**Decision: graphics stretch 1, the meshlet path.** Static (non-skinned) opaque/masked candidates of
the GPU-driven path are drawn as meshlets through task + mesh shaders when
`DeviceFeatures::mesh_shaders` and the new `RendererSettings::mesh_shading` (default on) allow it;
everything else is unchanged.
- **RHI (additive):** `GraphicsPipelineDesc::task/mesh` (mesh pipelines have no vertex input);
  `CommandList::draw_mesh_tasks[_indirect_count]` as non-pure virtuals (logging defaults keep test
  doubles compiling), implemented with `vkCmdDrawMeshTasks[IndirectCount]EXT`. Pre-existing GCC
  `-Wextra` warning in `vk_resources.cpp` fixed.
- **Build:** `cmake/deps/renderer.cmake` (the ADR-0009 hook) fetches meshoptimizer v0.25 (MIT, pinned
  commit, library sources only) as `aether_meshoptimizer`, private to the renderer.
- **Meshlets:** built per submesh at `register_mesh()` (<= 64 vertices / 124 triangles, cone weight
  0.25, `meshopt_optimizeMeshlet`, sphere + normal-cone bounds) into one GPU buffer per mesh:
  `GpuMeshlet[64 B] x n | u32 words` (mesh-local vertex indices, then triangles packed 8:8:8).
  Winding is preserved. Released with the mesh.
- **Culling:** instance culling is unchanged (`gpu_cull.comp`: frustum + two-phase Hi-Z). A new
  batch bit (`kBatchMeshlet`, 16 -> 32 batches) routes static candidates to meshlet batches, for which
  the cull shader writes `{ceil(n/32), 1, 1, candidate}` into the same 20-byte command slot.
  `meshlet.task` (32 threads) tests each meshlet's world bounding sphere against the main-view planes
  and meshoptimizer's apex cone (skipped for double-sided batches and mirrored / non-uniformly scaled
  instances), compacts survivors in thread order (deterministic, no subgroup ops) and launches one
  `meshlet.mesh` workgroup per survivor. The mesh shader repeats `mesh.vert`'s math expression for
  expression, so prepass / forward / pick (Equal depth tests) stay consistent.
- **Passes:** depth prepass (both phases), forward and pick use the 8 meshlet pipelines
  (depth: masked x double-sided; forward, pick: double-sided); shadows, translucency and skinned
  meshes keep the vertex path. `RendererStats::meshlet_instances` counts meshlet-drawn candidates.

**Verification (llvmpipe).** Every golden case matches the existing (vertex-path) references with the
meshlet path active: max channel diff <= 2, PSNR >= 78 dB (floating-point contraction differences).
New ctest entries `golden.<case>.no_mesh_shading` check the vertex path against the same references;
`aether-golden --no-mesh-shading` switches it off for A/B checks. Proof that the path is live: emitting
no mesh tasks fails the goldens (spheres vanish); inverting the cone test culls every front-facing
meshlet (spheres vanish; the non-uniformly scaled floor is correctly exempt). Unit tests: meshlet
building (every triangle exactly once with its winding, limits, bounding spheres, cone convention ==
the task shader's test), pipeline table (8 task/mesh permutations compile with glslang; absent without
mesh shaders), mock-device path (mesh-task draws with stride 20, picking, fallbacks, buffer release).
Validation clean in all app checks; the slice renders meshlet statics next to a vertex-path skinned
character.

**Verified on the Intel Arc (2026-09-27, driver 101.8991, Vulkan 1.4.356, validation ON).** All 9
golden cases, `shadows` included, run with the meshlet path active ("static meshes as meshlets" in the
log) and match the Arc references **bit-identically** (max diff 0, PSNR inf). Every run shut down with
no leaked resources and 0 VMA allocations. The ADR-0009 acceptance gate ("Arc golden cases pass
unchanged") therefore holds with mesh shading on.

**Remaining:** spot-light shadows (ADR-0009 stretch 2) and editor thumbnails.

---

## ADR-0011 — Asset-browser thumbnails (2026-09-27)

**Status:** accepted; ADR-0009 editor stretch goal. **Scope:** `editor/**` only (no contract changes).

**Decision.** The asset browser shows a 128x128 thumbnail per asset, generated on demand and cached
(`editor/src/thumbnail_cache.*`; GPU-free helpers in `editor/include/aether/editor/thumbnail.h`):
- **Images** (`.png/.jpg/.tga/.bmp/.hdr`): decoded on the CPU with stb and area-downscaled with an
  exact, alpha-weighted box filter (letterboxed to a square; nearest-neighbour when upscaling; HDR
  tonemapped with a log-average exposure + Reinhard). Exact source colours, no renderer involved.
- **Models, prefabs, scenes**: instantiated into a private scratch `World` (never the edited one) and
  resolved through a private `RenderResourceCache`; once every mesh, material and texture is
  resident, the camera is framed on the visible bounds (fixed 3/4 view, bounding-sphere fit) and a
  **dedicated thumbnail Renderer** (created lazily, 128x128, TAA / bloom / GPU culling off so one
  frame is final, shadows mirror the main renderer) draws it into the thumbnail texture. Models and
  prefabs get a key + fill light when they carry none. At most one 3D thumbnail per frame, so the
  second renderer keeps the once-per-device-frame contract (ADR-0002) and never touches the viewport
  renderer's TAA / Hi-Z history. A job that is still waiting after 600 frames renders with what is
  resident.
- **Lifetime:** entries are keyed by content path, regenerated when the file's modification time
  changes (checked once a second), and textures + ImGui descriptors are retired with a
  frames-in-flight delay.
- **UI:** grid view (default; tile size slider) or list view (small icons, large preview in the
  tooltip). Double-click, drag-and-drop into the viewport / hierarchy and a right-click menu work in
  both. `aether-editor --browse <folder>` opens the browser in a content folder.

**Fixes made alongside.** The toolbar's play controls were drawn over the grid / snap controls
(`GetCursorPosX()` is already on the next line after `SameLine` chains); they now start after the
last item or wrap to a second, centred row, and the default layout reserves two toolbar rows.
New *View > Reset Layout* rebuilds the default docking (a stale `imgui.ini` could leave newer
panels floating).

**Verification (Linux / llvmpipe).** `test.editor` +7 cases (kinds, exact area averaging, no colour
bleed from transparent texels, aspect-fit letterboxing, nearest upscaling, HDR tonemap, every
bounds corner on screen after framing). `aether-editor --self-test` step 21-22: thumbnails of
`props.gltf` (BC7 textures + normal maps), `cube.gltf`, a PNG, `showcase.aescene` and the test's
prefab all become ready, are read back from the GPU and contain real images (full coverage, luminance
stddev 27-58); touching the prefab regenerates its thumbnail. Images reviewed by eye; the editor
screenshot shows the grid and the fixed toolbar. Validation clean; ctest 24/24 + shadows skipped.
Not yet checked on the Arc.

**Not done:** material thumbnails (materials live inside glTF files; the editor's material
instances are not rendered by the thumbnail renderer and show the default material), animated
previews, and an on-disk thumbnail cache (thumbnails are regenerated per session).

---

## ADR-0012 — Spot-light shadows; delivery as a downloadable local application (2026-09-27)

**Status:** accepted; verified on llvmpipe and the Intel Arc. Closes ADR-0009 graphics
stretch 2, the last open item of the M2 plan.

**Owner requirement recorded.** The project owner clarified the product goal: Unreal-class capabilities
(§1.2, unchanged) **delivered as a downloadable app that installs and runs locally** (§1.1 "Delivery",
new §1.2 row, clarified non-goal). Today the editor is a native local program but must be built from
source; producing an installable build (editor + player + shaders + starter content, no toolchain
needed) is the next distribution milestone.

**Decision: spot-light shadows.**
- `RendererSettings` (additive): `spot_shadows` (default on), `spot_shadow_map_size` (1024),
  `max_spot_shadows` (4, capped at `kMaxSpotShadows` = 8); `RendererStats::spot_shadow_maps`.
- Spot lights with `cast_shadows` are candidates; those whose light sphere reaches the view are sorted by
  distance and the nearest `max_spot_shadows` get a layer of a D32F `Tex2DArray` ("SpotShadowMaps").
  Each uses a reverse-Z perspective from the light: half-angle = outer cone + 2 deg (PCF margin), capped
  at 80 deg; near = range/100 (clamped 0.02..0.5 m); far = range.
- Casters are culled per light on the CPU (frustum vs world AABB) into shadow draw lists; the
  `SpotShadows` graph pass renders each layer with the existing shadow pipelines
  (`view_index = 1 + kMaxCascades + slot`, read by `mesh.vert` / `meshlet.mesh`), depth bias
  (-1, slope -1.5).
- Shading: `GpuLight::shadow` of a spot = its `GpuSpotShadow` index (view-proj, texel scale, layer; new
  per-frame `SpotShadowBuffer`, `GpuFrame` grows 1016 -> 1032 bytes). `ae_spot_shadow` does 3x3
  tent-weighted bilinear PCF with **textureGather + in-shader reverse-Z comparison** on a point sampler
  and a normal offset of ~1.5 texels at the receiver's distance. Not using a compare sampler avoids the
  Mesa llvmpipe depth-compare crash (ADR-0004), so spot shadows are independent of the cascaded
  `shadows` switch and work on software rasterisers too.
- Editor: *Create > Spot Light* casts shadows by default; Engine panel gets a "Spot shadows" toggle
  and the spot-shadow-map count. The inspector already exposes cast shadows and the cone angles.

**Verification (Linux / llvmpipe).** New golden case `spot_shadows` (two coloured shadow-casting spots
over a pillar, box and sphere; no sun): shadow directions checked against both lights, contact-attached,
no acne; llvmpipe reference recorded after review. Mock-device test: budget (default 4, setting 2,
clamp to 8), out-of-view and non-casting spots skipped, the pass disappears when disabled or without
casters, independence from `shadows`, clean command streams. All previous goldens unchanged
(max diff <= 2); ctest 25/25 + sun `shadows` skipped; slice / editor self-test / player validation clean.

**Verified on the Intel Arc (2026-09-27).** The 9 existing Arc goldens still match bit-identically
(max diff 0, shadows included); the Arc `spot_shadows` image was reviewed (same shadows as llvmpipe,
attached, no acne) and recorded as the Arc reference (commit 527fc43). In the editor the owner
confirmed spot lights with shadows, the inspector controls (cast shadows, cones, range) and the Engine
panel "Spot shadows" toggle. The startup-scan fix below was confirmed on the Arc ("content scan: 6
sources, 6 imported, 0 failed"; the textured cube and skinned model render).

**Not done:** point-light (cube) shadows, shadow caching for static lights, and the sun + spot shadow
interaction in the ShadowCascades debug view.

**Fix found during the Arc walkthrough: models missing from scenes on a fresh checkout.** Scenes
reference assets by AssetId only; in source (Editor) mode a source was imported only when something
asked for it by *path*, so on a machine whose `assets/asset_db.json` did not already list the glTF
sources, `showcase.aescene`'s textured cube and skinned model failed with "not in the asset
database" (reported on the Arc; reproduced on Linux after deleting `assets/`; earlier Linux runs had
passed on a database left by previous sessions). `Application` now enables
`AssetManagerConfig::scan_on_startup` in source mode: an incremental scan (size + mtime fast path)
imports new / changed sources before the startup scene loads. Fresh database: 6 sources imported,
`aether-player --check` passes; second run: 6 up to date. Cooked (shipping) mode is unchanged.

---

## ADR-0013 — The downloadable application: packaging and the user workspace (2026-09-27)

**Status:** accepted; verified on Linux and on Windows (installer + smoke test). Implements
the "Delivery" requirement of §1.1 (ADR-0012).

**Decisions**
1. **Install layout** (`cmake/Packaging.cmake`, component `Aether`): `bin/aether-editor`,
   `bin/aether-player` (+ the MSVC runtime DLLs via `InstallRequiredSystemLibraries`, app-local; the
   UCRT ships with Windows 10/11), `shaders/`, `content/` (starter content, authoring scripts
   excluded), `licenses/<component>/` (the actual license texts of the 18 bundled libraries, taken
   from the fetched sources), `THIRD_PARTY_NOTICES.md`, `README.md` and the install marker
   `aether-install.json`. The engine root resolves as the executable's ancestor holding `shaders/`
   (ADR-0001 paths), so an installed copy never uses the developer's source tree. Third-party install
   rules (their headers / static libs) are in other components and never packaged.
2. **Packages** (CPack): a portable ZIP (with a top-level `Aether-<version>-<os>/` folder) always;
   on Windows with Inno Setup 6 (CMake >= 3.27 generator) also a setup `.exe`: per-user install
   without admin rights (all-users optional in a dialog), Start menu + desktop shortcuts to the editor,
   uninstaller, "launch Aether Editor" on the last page, a stable AppId for upgrades. No license page:
   the project has no LICENSE of its own yet (owner's choice). `scripts/package.ps1` builds Release,
   packages into `dist/` and, with `-SmokeTest`, tests the unzipped package.
3. **User workspace** (`runtime/workspace.h`): an installed engine (marker present, or read-only
   `content/`) must not write into its folder, so without `--project` the editor and the player open
   `Documents/Aether Projects/Starter Project/Starter Project.aeproject`, created on first run by
   copying the installed starter content (never overwritten afterwards, so user edits survive
   upgrades). Documents = Windows' Documents known folder (follows OneDrive redirection), else
   `$XDG_DOCUMENTS_DIR` / `~/Documents`; `AETHER_DOCUMENTS_DIR` overrides it (tests, portable use).
   The editor gained `--project <file.aeproject>` (content + cooked roots, startup scene, window title),
   linking `aether.runtime`, so `runtime` now configures before `editor`. Development checkouts
   (no marker, writable content) behave exactly as before.
4. **Release defaults:** the editor, like the player, requests Vulkan validation only in debug builds
   (end users have no SDK layers; a missing layer was already only a warning).

**Verification (Linux).** Release build -> `cpack` -> 10.5 MB archive containing only the Aether
component. Unpacked to a fresh folder with an empty temporary Documents: engine root = the package
folder ("executable ancestor"), starter project created, validation off, the installed
`aether-editor --self-test` PASSED (6 sources imported into the project), the installed
`aether-player --check` PASSED (6 sources up to date), SHA-1 of every file in the package folder
unchanged afterwards, and the player still passes with the repository's `shaders/` hidden (no
source-tree dependency). `test.runtime` +4 cases (marker, writability probe, Documents override,
starter project created once and never overwritten, dev vs installed). Debug tree: ctest 26/26.

**Verified on Windows (owner's Intel Arc PC, 2026-09-27).** `scripts/package.ps1 -SmokeTest` built the
Release packages (ZIP + Inno Setup installer) and the packaged-build smoke test passed on all four
checks: installed editor self-test PASSED, installed player check PASSED, starter project created in
Documents, install folder unchanged. The setup `.exe` installed, the editor launched from the
installer, the Start menu and desktop shortcuts, and the uninstall were exercised by the owner.

**Not done yet:** offline shader precompilation (shaders still compile at startup; the pipeline cache
makes later starts fast), an application icon and version resource, code signing (unsigned installers
trigger SmartScreen warnings), File > New / Open Project in the editor UI, and a macOS / Linux
installer.

## ADR-0014 — Application identity, projects in the editor UI, precompiled shaders (2026-09-28)

**Status:** accepted; verified on Linux and on Windows. Closes three "not done yet" items of ADR-0013.

**Decisions**
1. **Identity.** `resources/` holds `aether.ico` (16-256 px), `aether.png` and `splash.png`,
   generated by `tools/make_icon.py` (Pillow; procedural logo, no third-party art) and installed as
   `resources/`. On Windows each app embeds the icon plus a `VERSIONINFO` resource
   (`cmake/packaging/aether.rc.in`, version = `PROJECT_VERSION`); the Inno Setup installer and the
   uninstall entry use the same icon. `AppDesc` gained trailing fields (additive, like ADR-0006)
   `window_icon`, `splash_image`, `splash_title`: the window icon is set through GLFW on every
   platform, and while the engine initialises (shaders, assets, scene) a splash frame with a status
   line is presented on the swapchain (ImGui; no second window) and shows `AE_VERSION_STRING`.
2. **Projects in the editor.** `runtime/workspace.h` gained `create_project` (templates *Starter* =
   copy of the installed starter content, *Empty* = one scene with camera, sun and floor),
   `valid_project_name`, `find_projects`, `RecentProjects` (`<cache_dir>/recent_projects.json`, 10
   entries) and `launch_detached`. File > New Project... / Open Project... / Recent Projects open a
   Projects window (also `--projects`); switching asks about unsaved changes, then relaunches the
   editor with `--project` (a clean process per project, no in-place teardown of every subsystem).
   The self-test drives it with a recording launcher.
3. **Precompiled shaders (SPIR-V cache).** `PipelineLibrary` looks a module up, in order, in
   `<shader root>/spirv/` (read-only, baked at build time and shipped) and in the user cache
   `<cache_dir>/shaders/`, and only then compiles GLSL (writing the result to the user cache through
   a temp file + rename). The file name is an FNV-1a 64 hash of: a format version, the **tree hash**
   (sorted relative paths + contents of every file under the shader root except `spirv/`), the
   shader file, stage, all defines (shared + permutation) and the debug-info flag. Any shader edit,
   includes included, therefore invalidates every entry: correctness over reuse, and hot reload
   (which recomputes the tree hash) is unaffected. Entries are checked for the SPIR-V magic and size;
   a corrupt entry is recompiled. `AE_SHADER_CACHE=0` turns the cache off.
   `aether-shaderc <shader root> <out dir>` (engine/renderer/tools/shaderc) compiles every unique
   shader of `build_pipeline_specs()` with mesh shaders enabled (a superset for every device) using
   the same `shader_compile_options()` as the runtime; the target `aether_shader_cache` re-runs it
   when a file under `shaders/` changes, the apps depend on it, and the output is installed as
   `shaders/spirv/`. The build configuration fixes the debug flag, so a Release package ships
   Release SPIR-V. The Vulkan pipeline cache (ADR-0001) is unchanged and complements this.

**Verification (Linux).** `test.renderer` +2 cases (user cache fill then 100 % hits, no temp
leftovers; an edit of an include recompiles everything; a `spirv/` folder is used first and not
hashed; corrupt entries are rewritten; the off switch; the cache name depends on defines, tree
and debug flag but not on the absolute include path). Debug ctest 26/26 (golden references still
bit-identical when the shaders come from the cache). Release: 29 modules baked in 0.4 s; the
installed player and editor self-test load all 29 as precompiled (0 compiled) and the install
folder stays unchanged; on llvmpipe the pipeline build went from 563 ms to 383 ms (what remains is
pipeline creation). `scripts/package.ps1 -SmokeTest` gained a fifth check (baked shaders shipped).

**Verified on Windows (owner's Intel Arc PC, 2026-09-28).** `scripts/package.ps1 -SmokeTest` passed
all five checks. The installed shortcuts show the Aether icon; both executables report their version
resource (FileDescription "Aether Editor" / "Aether Player", FileVersion 0.1.0, ProductName Aether).
File > New Project (*Empty* template) relaunched the editor into a scene with only the floor, sun and
camera; Open Project and Recent Projects list every project and switch between them. The installed
editor logged `pipelines built in 14 ms (shaders: 29 precompiled, 0 cached, 0 compiled)`.

**Not done yet:** code signing, a per-shader dependency hash (so one edit only recompiles its dependants).

## ADR-0015 — Point-light shadows (2026-09-28)

**Status:** accepted; verified on Linux (llvmpipe). First of the owner's "Unreal-style" feature
list (point shadows, then global illumination).

**Decisions**
1. **Six perspective views, not a cube map.** A shadow-casting point light gets six reverse-Z
   perspective views (+X, -X, +Y, -Y, +Z, -Z; `render_math.h point_shadow_face`) rendered into
   layers of a second D32F array, `PointShadowMaps` (`point_shadow_map_size` per face, default 512;
   6 x `max_point_shadows` layers). They are ordinary entries of the ADR-0012 `GpuSpotShadow`
   table (spots first, then 6 consecutive faces per point light; `GpuLight::shadow` = the first
   face), so the mesh / meshlet shaders, `view_index`, CPU culling and the shadow pass code are
   shared, and sampling is the same textureGather + in-shader compare (works on llvmpipe, ADR-0004).
   No cube-map sampling, no linear-distance depth format, no geometry-shader or multiview need.
2. **Seamless faces.** The shader picks the face by the major axis of light -> receiver
   (`ae_point_shadow_face`, mirrored by `point_shadow_face_for` on the CPU). Each face's field of view
   is slightly wider than 90 degrees (`kPointShadowMarginTexels` = 5 texels on each side), so the
   PCF footprint (normal offset + 3x3 taps + bilinear quad) of any receiver stays inside the face it
   selected; there is no seam and no light leak at face edges (unit-tested over 2000 directions and
   the eight cube corners).
3. **Data (additive).** `GpuSpotShadow::pad0` became `map` (the bindless index of the array that
   holds the entry: spot or point map); `GpuFrame` is unchanged (1032 B). `kMaxPointShadows` = 4,
   `kMaxLocalShadowViews` = 8 + 4 x 6 = 32 GpuSpotShadow entries per frame. `RendererSettings` gained
   `point_shadows` (on), `point_shadow_map_size` (512), `max_point_shadows` (2);
   `RendererStats::point_shadow_maps`. Selection mirrors spots: point lights with `cast_shadows`
   whose range sphere reaches the view, nearest first, within the budget. The Engine panel has a
   "Point shadows" switch and shows the count. `LightComponent::cast_shadows` stays off by default;
   the showcase's orange "Lamp" now casts shadows.
4. **Cost.** Six depth passes per shadowed light (each CPU-culled against its face frustum); at the
   default budget 2 x 6 x 512^2 D32F = 12 MB.

**Verification (Linux).** `test.renderer` +2 cases (six faces per light, budget + clamp, the pass
runs, spots and points together with separate maps, the switch, a size change; face selection +
footprint containment). New golden case `point_shadows` (a lamp among six pillars, a ball below it
and a back wall: radial shadows across every horizontal face and the -Y face, no seams); ctest 27/27
(cascaded `shadows` skipped on llvmpipe as before). Editor self-test, player check and vertical
slice pass validation-clean with the showcase lamp shadowed.

**Verified on Windows (owner's Intel Arc PC, 2026-09-28).** `aether-golden --case point_shadows
--update` rendered the same image as llvmpipe (radial shadows, no seams at the cube-face edges).

**Not done yet:** per-face culling of views that see no
receiver in the camera frustum; shadow caching for static lights (re-render only when something in
range moves).

## ADR-0016 — Dynamic diffuse global illumination: the irradiance probe volume (2026-09-28)

**Status:** accepted; verified on Linux (llvmpipe). Second of the owner's "Unreal-style" features.

**Context.** Indirect light so far came from the environment only (IBL or a flat ambient): rooms
were lit by the sky through their walls and shadows were black indoors. Hardware ray tracing
(Lumen / DDGI) is unavailable on the software rasteriser we verify on, and a baked lightmap
pipeline needs UV2 unwrapping and a slow build step. The owner wants GI that "just works" in the
editor while editing.

**Decisions**
1. **Rasterised probe captures, updated continuously.** `RenderScene::gi` (`GiVolume`: an
   axis-aligned box + probes per axis, additive) defines a regular probe grid. Every frame the
   renderer re-captures `gi_probes_per_frame` probes round-robin (default 8, <= 64): six 90-degree
   faces of `gi_capture_size`^2 texels (default 16) each, rendered with the new `gi_capture.frag`
   (Lambert albedo x direct light from every light with its sun / spot / point shadow maps + the
   GI volume itself + emissive), into an RGBA16F array. The capture views are extra entries of the
   ADR-0012 view table (after the shadow views), so the mesh vertex shader and CPU culling are
   shared. Because a capture reads the volume, each refresh adds one light bounce (multi-bounce
   converges over a few refreshes); moving lights or objects update within
   probes / gi_probes_per_frame frames. No ray tracing, no baking, no UV2, works on every device.
2. **L1 spherical harmonics.** `gi_project.comp` (one workgroup per captured probe) integrates the
   six faces (texel solid angles; texels that saw nothing take the environment's radiance; back
   faces of single-sided geometry count as "inside") into L1 SH pre-convolved with the clamped
   cosine and divided by pi: `irradiance/pi(n) = c.x + dot(c.yzw, n)` per channel (`GpuGiProbe`,
   64 B, in a persistent storage buffer; the buffer is cleared when the box or grid changes).
3. **Shading.** `gi.glsl` blends the 8 surrounding probes trilinearly with a DDGI-style smooth
   back-face weight, down-weights probes that saw > 20 % back faces (inside walls), and offsets the
   lookup along the geometric normal (0.25 x the smallest spacing) to limit leaking. Inside the box
   the result replaces the environment's diffuse light; it fades to the environment over one probe
   cell outside the box. The environment reflection is scaled by min(1, GI / environment
   luminance): a cheap specular occlusion indoors. New debug view `GlobalIllumination`.
4. **Data (additive).** `GpuFrame` grew to 1088 B (`gi_probes`, `gi_min`, `gi_intensity`,
   `gi_inv_spacing`, `gi_normal_bias`, `gi_counts`; off = `gi_counts.x == 0`, no 64-bit compare).
   Mesh pipeline table +2 (`MeshPass::GiCapture`, static / skinned, no culling), `PipelineId::GiProject`.
   `RendererSettings::gi` / `gi_probes_per_frame` / `gi_capture_size`, `RendererStats::gi_probes` /
   `gi_probes_updated`. Limits: 64 probes per axis, 32768 per volume (the densest axis shrinks).
5. **Scene side.** `gameplay::GIVolumeComponent` (`probe_spacing`, `intensity`, `enabled`;
   codec "GIVolume"): the entity's position is the box centre and its scale the box size (rotation
   ignored). `gi_volume_from()` derives the grid (size / spacing + 1 per axis, clamped to 2..64); the
   first visible, enabled volume is used. The editor has Create > GI Volume, an inspector section
   (probe count readout), Add Component > GI Volume, a yellow box outline while selected, and in the
   Engine panel a "Global illumination" switch, a probes-per-frame slider and a debug "View" picker
   (all DebugView modes, which had no UI before). The showcase scene has a 16 x 6 x 12 m volume.
6. **Cost.** Per frame 6 x gi_probes_per_frame small depth + colour passes (CPU-culled per face)
   plus one compute dispatch; the probe buffer is 64 B per probe (e.g. 540 probes = 34 KB). Off, or
   with no volume, nothing runs and every other golden image is unchanged.

**Verification (Linux).** `test.renderer` +1 case (captures only with a volume, round-robin count,
per-frame and per-volume clamps, degenerate box, switches, capture-target re-creation);
`test.gameplay` +2 cases (codec round trip; box / probe counts / clamps and extraction incl.
disabled and hidden volumes). Golden cases `gi_room` (a lamp in a red / green / white room: lit
ceiling, filled shadows, red and green bleeding onto the white blocks) and `gi_room_off` (the same
room without GI, for comparison). ctest 29/29 (cascaded `shadows` skipped on llvmpipe as before);
editor self-test (+ Create > GI Volume -> probes captured), player check and vertical slice pass
validation-clean.

**Verified on Windows (owner's Intel Arc PC, 2026-09-28).** `gi_room` and `gi_room_off` rendered on
the Arc match the llvmpipe references visually (lit ceiling, filled shadows, red / green bleeding vs.
black shadows without GI): the capture + SH projection path behaves the same on real hardware. In the
editor (Debug build, validation on) the showcase's GI switch, the GI debug view and the point-shadowed
lamp behave as intended; GPU time 3-5 ms with GI off, 4-6 ms with GI on (8 probes / frame). The
selection outline was drawn light blue instead of yellow (R and B swapped in the packed colour; fixed).

**Not done yet:** probe relocation out of walls and
per-probe visibility (depth moments) to remove the remaining leaks; several volumes at once and
rotated volumes; GI for translucent capture; baking a volume to disk for static scenes.

## ADR-0017 — Reflections: box-projected reflection capture probes (2026-09-28)

**Status:** accepted; verified on Linux (llvmpipe) and on Windows (Intel Arc). Third of the owner's "Unreal-style" features.

**Context.** Specular reflections came only from the sky (IBL prefiltered cube) or the flat ambient:
a polished floor indoors mirrored the sky, and metals looked wrong in any enclosed space. Screen-space
reflections need a normal / roughness buffer the forward renderer does not have, and cannot show
what is off screen; hardware ray tracing is unavailable on the verification device. Unreal's
answer for the same problem is the (box) reflection capture.

**Decisions**
1. **Probes.** `RenderScene::reflection_probes` (additive; `ReflectionProbe` = stable id, capture
   position, axis-aligned box, intensity, blend distance). The renderer keeps up to 8 persistent
   slots keyed by id (`kMaxReflectionProbes`); a slot owns an RGBA16F environment cube (full mips)
   and a GGX-prefiltered cube (`kPrefilteredMips`), both `reflection_probe_size`^2 (default 128).
   Slots of probes that disappear are released (deferred past in-flight frames) before new probes
   are assigned.
2. **Capture, one probe per frame.** A dirty probe (new, moved, resized) is captured first; with
   `reflection_realtime` (default on) one probe per frame is re-captured round-robin, so moving
   objects appear in reflections. The capture renders six 90-degree faces with the GI capture
   pipeline (`gi_capture.frag`: albedo x direct light with shadows + GI + emissive, i.e. the diffuse
   look of the scene, like Unreal's captures), views in the shared view table after the GI views
   (`kReflViewBase`). Then, in one render-graph pass with manual barriers on the probe's own
   textures: `refl_resolve.comp` projects each cube texel's direction (Vulkan cube orientation) into
   the capture face of its major axis and fills sky texels from the skybox (or the ambient colour);
   `cube_downsample.comp` builds the mip chain (2x2 box); the existing IBL `Prefilter` pipeline
   (filtered importance sampling, 64 samples) writes the roughness mips.
3. **Shading.** `reflections.glsl`: probes are uploaded smallest box first (`GpuReflectionProbe`,
   64 B, `GpuFrame` +16 B = 1104 B). For each probe containing the point, the reflection ray is
   intersected with the box and the cube is sampled towards that hit from the capture point (box
   projection / parallax correction), at lod = roughness x (mips - 1); the probe's weight rises over
   `blend_distance` from the box faces and the remaining weight falls through to larger probes and
   finally to the sky (whose GI-based specular occlusion, ADR-0016, still applies). New debug view
   `Reflections`.
4. **llvmpipe.** With no probes the shader returns before the loop: Mesa llvmpipe hoisted the first
   probe load above the loop condition and dereferenced the (then null) buffer address, crashing
   every frame; the buffer address is also always valid frame-arena memory now.
5. **Scene / editor.** `gameplay::ReflectionProbeComponent` (`intensity`, `blend_distance`,
   `enabled`; codec "ReflectionProbe"; box = the entity's position + scale like the GI volume,
   `reflection_probe_from()`; id = entity index + 1). Editor: Create > Reflection Probe (10 x 4 x 10),
   inspector section, Add Component entry, light-blue box outline while selected, Engine panel
   "Reflection probes" + "real-time" switches and probe count, "Reflections" in the View picker.
   The showcase has a 20 x 6 x 20 m probe (the metal balls now mirror the scene). Practical rule:
   make the box a little larger than the room so floors and walls lie inside it.
6. **Cost.** One capture per frame: 6 small passes (CPU-culled per face) + resolve + mips +
   prefilter of a 128^2 cube; forward shading adds a short loop per pixel (<= 8 box tests).

**Verification (Linux).** `test.renderer` +1 case (slots, one capture per frame, dirty-only
without realtime, re-capture on move, id / box validation, the 8-probe limit when the set is
replaced, size change, removal and the switch, deferred releases without device errors);
`test.gameplay` +2 cases (codec round trip; probe box / id stability / disabled and hidden
probes). Golden cases `reflections` (the ADR-0016 room with a polished metal floor and a chrome
ball: the floor mirrors the red / green walls and the blocks, parallax-correct) and
`reflections_off` (same room without the probe: black floor). ctest 31/31 (cascaded `shadows`
skipped on llvmpipe; every earlier golden unchanged); editor self-test (+ Create > Reflection
Probe -> captured every frame), player check and vertical slice validation-clean.

**Verified on Windows (owner's Intel Arc PC, 2026-09-29).** `reflections` / `reflections_off` render
the same as on llvmpipe (mirrored walls and blocks on the floor, the room in the chrome ball vs. a
black floor without the probe). In the editor the showcase's metal balls reflect the scene, the
Reflection probes switch and the Reflections view behave as intended, and the probe outline shows.
GPU time in the editor (Debug + validation): 6-8 ms with probes off, 7-9 ms on (about +1 ms for
one real-time capture per frame).

**Not done yet:** screen-space reflections
for contact detail; captures that include specular (second bounce); rotated boxes and sphere
probes; saving captures with the scene for static lighting.

## ADR-0018 — Visual scripting: node graphs compiled to Lua (2026-09-28)

**Status:** accepted; verified on Linux. Fourth of the owner's "Unreal-style" features (Blueprints).

**Decisions**
1. **Graphs compile to Lua; no second runtime.** A visual script is a `.aegraph` JSON file
   (`"format": "aether.graph"`, version 1) under `content/scripts`. `ScriptVM::read_script`
   compiles it with `compile_graph_to_lua` when it is read. It is then an ordinary script:
   per-entity instances, the sandbox and instruction budget, error isolation, hot reload (a graph
   with errors keeps the previous version running), `declared_properties` for the inspector, and
   packaging (`is_runtime_content_file` includes `.aegraph`). Script paths ending in `.aegraph` are
   no longer suffixed with `.lua`. This mirrors Unreal compiling Blueprints to bytecode, and it
   lets users read the generated Lua (View > Show generated Lua) to learn the scripting API.
2. **Model** (`aether/scripting/visual_script.h`, in the scripting module, so the player runs
   graphs without the editor): `VisualGraph` = nodes (id, type, canvas position, literals of
   unconnected inputs, selected variable), links (output pin -> input pin) and variables (name,
   type, default -> the script's `properties`). The pin types are exec, bool, number, vector,
   string, entity and any. `connect()` enforces the rules: types compatible, one wire per data
   input, one wire per exec output (exec inputs merge), no self links. An unconnected entity
   input is **Self**.
3. **Node library** (61 nodes).
   * Events: On Start / On Update / On Key Pressed / On Key Released / On Event / Every N Seconds.
   * Flow: Branch, Sequence, Delay.
   * Actions: Print, Set (World) Position, Move By, Rotate, Set Scale, Set Visible, Look At, Spawn,
     Destroy, Emit Event, Set Variable.
   * Smooth motion (added after the owner's first test, where Move By + Delay hops looked choppy):
     Move By / Move To / Rotate Over Time. Each is a per-update timer with a smoothstep ease. Its
     `then` output continues immediately and `finished` runs at the end.
   * Data: Get Variable, Self, Find Entity, positions and forward, Delta Time, Time, Is Key Down,
     Key Axis, number maths, comparisons and logic, vector make / break / add / subtract / scale /
   length / normalize / distance, and Join Text.
4. **Compiler.** Pure nodes become inline Lua expressions, recursively from each input's wire.
   Exec chains become statements: Branch becomes `if/else`, Sequence emits its outputs in order,
   and Delay becomes `timer.after(function ... end)`. Nodes where two chains meet are inlined per
   chain. Values produced by exec nodes (Spawn) live in `self.__n<id>_*`. Numbers are emitted as
   float literals. Entity actions are guarded (`if t and t:valid()`).
   * On Start: Start chains, `events.subscribe` for On Event and `timer.every` for Every N Seconds.
   * On Update: Update chains and key events (`input.key_pressed / key_released`).
   * Errors carry a node id: unknown node, missing variable, invalid or duplicate variable names,
     wires to unknown pins or of incompatible types (hand-edited files), execution loops, data
     loops, and nodes that cannot run.
5. **Editor** (`editor/src/graph_editor.{h,cpp}`: a "Visual Script" window built on ImGui draw
   lists with manual hit-testing; no third-party node library).
   * Canvas: grid, pan (right / middle drag) and zoom (wheel, 0.4-1.6, the font scales with it).
   * Nodes have colour-coded headers and help tooltips. Pins are triangles (exec) or circles
     (data), colour-coded by type, and wires are bezier curves.
   * Wiring: drag between pins; grab a connected input to move its wire; Alt+click clears a pin.
     A right-click, or a wire dropped on empty space, opens a searchable node menu filtered to
     compatible nodes, and the new node is auto-wired.
   * Unconnected inputs are edited inline on the node. A variables list edits name, type and
     default, and the palette adds nodes.
   * Undo / redo use whole-graph snapshots. Del, Ctrl+D, Ctrl+S and Revert are available.
   * Compile errors are listed live; clicking one focuses its node, which is outlined red.
   * Entry points: Assets panel (double-click / "Edit" on `.aegraph`, "Attach to selected
     entity"), inspector Script section ("New Visual Script" on an empty script path ->
     `scripts/<name>.aegraph` with a starter graph, "Edit Graph"), View > Visual Script. Dropping a
     graph on an entity or in the viewport attaches it like a Lua file.
   * The showcase has a "Graph Cube" running `graphs/spin_and_hop.aegraph`: it spins at the
     `spin_speed` variable, and Space makes it hop (Move By, Delay 0.4 s, Move By back, Print).

**Verification (Linux).** `test.scripting` +5 cases:
* connection rules (type checks, replacement, exec fan-out, self links, variable-typed pins);
* JSON round trip, including malformed documents;
* each compiler error kind;
* a compiled graph running in a ScriptVM: On Start, On Update, variables as inspector-overridable
  properties, Branch plus logic, Emit Event reaching an On Event graph on another entity;
* key press, Sequence, Delay, Every N Seconds and Spawn, a broken edit keeping the old version,
  and a fixed edit hot reloading.

Also:
* `test.runtime` packages `.aegraph` files.
* Editor self-test: New Visual Script on a cube, editing through the editor (add nodes, wire, set
  a literal, undo / redo), save, then Play. The instance runs, On Start moved the cube and the
  starter graph rotates it. The file is removed afterwards.
* The player check runs the showcase with the Graph Cube's graph. ctest 31/31, and every app
  check is validation-clean.

**Not done yet:**
* Functions / macros (collapsed sub-graphs), local variables, and loops (For Each, While).
* Collision / trigger events from physics.
* Component nodes (lights, physics impulses, audio).
* Comments and reroute nodes, copy / paste between graphs, and live debugging (highlighting the
  wires that executed).

**Owner feedback, round 1 (Intel Arc, 2026-09-29).** Hopping, the console print and editing a
variable all worked. Fixes:
* The demo hop is now eased (Move By Over Time up 0.25 s, then down 0.3 s).
* Dear ImGui's ID-conflict error in the Script inspector: the header and the path field were both
  labelled "Script"; the field is now "Path".
* The editor UI did not scale in fullscreen. View > UI Scale (Auto = window height / 900, in
  5 % steps; or 100-200 %) rebuilds the style from the unscaled one (`ScaleAllSizes`, and
  `FontScaleMain` for the font). The choice is persisted in `<cache_dir>/editor_prefs.json`.
* The viewport grid (ImGuizmo `DrawGrid`, a 2D overlay without depth) was drawn across solid
  objects, a likely source of the reported shimmer. It is now renderer debug lines:
  depth-tested, centred on the camera, split into segments whose alpha fades with distance,
  with a red X axis and a blue Z axis. `debug_line.frag` now uses a relative depth bias (0.2 % of
  the distance), so lines lying on a surface pass against the jittered scene depth.

**Owner feedback, round 2 (Intel Arc, 2026-09-29).**
* **Flicker.** The whole editor window flickered: at random, when hovering the "real-time"
  checkbox (its tooltip crosses the window edge), and with the Visual Script window open. Stray
  "Material" / "Animation" taskbar entries pointed at the cause: Dear ImGui multi-viewports. Any
  panel, popup or tooltip leaving the main window became an OS window with its own swapchain, and
  creating / destroying those windows made the main window flicker. Multi-viewports are now opt-in
  (`AETHER_IMGUI_VIEWPORTS=1`, and then without taskbar icons); panels stay docked inside the
  editor.
* **Hops.** Repeated hops drifted upwards and stayed mid-air: overlapping Move By Over Time tweens
  each interpolated from their own start position. Move By Over Time is now additive (each tick
  applies only its share of the offset), so overlapping moves stack and matching opposite moves
  always land back (test: three overlapping hops return exactly to y = 0). Move To Over Time stays
  absolute.
* Confirmed: fullscreen UI scaling, the depth-tested grid, and a user-made graph (key C hides an
  entity).

