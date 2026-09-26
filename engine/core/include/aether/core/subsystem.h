// aether/core/subsystem.h — the engine subsystem interface + frame contexts.
//
// FROZEN CONTRACT (ADR-0001). Lives in core (Layer 1) so any layer can implement it
// (physics/animation/scripting are Layer 4, editor Layer 5). The Application (gameplay)
// owns the ordered list of subsystems and drives the frame timeline (blueprint §5.1).
#pragma once

#include "aether/core/time.h"
#include "aether/core/types.h"

namespace aether {

class Window;
class World;                              // fwd (aether::World, defined in aether.scene)
namespace renderer { struct RenderScene; } // fwd (defined in aether.renderer)

// Shared services handed to subsystems at startup. Extended as systems come online.
struct EngineContext {
    Window* window = nullptr;
    World*  world  = nullptr; // active world (may be null until a scene is loaded)
    void*   renderer = nullptr; // aether::renderer::Renderer* (opaque here to keep layering)
};

// Variable-rate update context.
struct FrameContext {
    EngineContext* engine = nullptr;
    FrameTime      time{};
};

// Deterministic fixed-step context (physics, deterministic gameplay).
struct FixedContext {
    EngineContext* engine = nullptr;
    f32            fixed_delta = 1.0f / 60.0f;
    u64            step_index  = 0;
};

// Render context: subsystems contribute to the frame's RenderScene (CPU-side DTO).
// Only the renderer touches the GPU (ADR-0001): anim writes skinning palettes here,
// physics appends debug lines here, etc.
struct RenderContext {
    EngineContext*        engine = nullptr;
    renderer::RenderScene* scene = nullptr; // the frame's render scene being assembled
    FrameTime             time{};
};

// Base interface for an engine subsystem. Override only what you need.
class ISubsystem {
public:
    virtual ~ISubsystem() = default;

    [[nodiscard]] virtual const char* name() const = 0;

    virtual void on_startup(EngineContext&) {}
    virtual void on_shutdown() {}

    virtual void on_begin_frame(FrameContext&) {}
    virtual void on_fixed_update(FixedContext&) {} // may run 0..N times per frame
    virtual void on_update(FrameContext&) {}       // variable-rate
    virtual void on_render(RenderContext&) {}       // assemble RenderScene / submit
    virtual void on_end_frame(FrameContext&) {}
};

} // namespace aether
