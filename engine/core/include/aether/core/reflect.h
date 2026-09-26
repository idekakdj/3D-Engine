// aether/core/reflect.h — reflection macros (compiling no-op for M0).
//
// FROZEN CONTRACT (ADR-0001). The SYNTAX below is final so modules can annotate
// components/assets now; the macros expand to nothing until the reflection backend
// lands (blueprint §5.4). When the backend arrives, these macros will emit TypeInfo
// registration WITHOUT any change to call sites.
//
// Usage:
//     struct Transform {
//         Vec3 position;
//         Quat rotation;
//         Vec3 scale;
//         AE_REFLECT(Transform,
//             AE_FIELD(position),
//             AE_FIELD(rotation),
//             AE_FIELD(scale))
//     };
#pragma once

namespace aether {
// Placeholder tag so translation units including this header are never empty.
struct ReflectionTag {};
} // namespace aether

// No-op today; real expansion later. Accepts a trailing field list (ignored now).
#define AE_REFLECT(Type, ...)      using ae_reflected_type = Type;
#define AE_FIELD(name)             /* field(name) */
#define AE_ATTR(...)               /* attribute(...) */
#define AE_REFLECT_ENUM(Type, ...) /* enum reflection */
