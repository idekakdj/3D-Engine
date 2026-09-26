// aether/core/error.h — error handling: Error, Result<T,E>, assertions.
//
// FROZEN CONTRACT (ADR-0001). Convention: engine code never throws; recoverable
// failures are returned as Result<T,E>. Exceptions remain ENABLED at the compiler
// level through M1 (MSVC STL / sol2 / doctest require it) but engine code avoids
// them. Assertions abort in Debug, log-and-continue in Release (configurable).
#pragma once

#include "aether/core/types.h"

#include <string>
#include <utility>
#include <variant>

namespace aether {

// ---------------------------------------------------------------------------
// Error
// ---------------------------------------------------------------------------
enum class ErrorCode : u32 {
    Unknown = 0,
    InvalidArgument,
    OutOfMemory,
    NotFound,
    AlreadyExists,
    NotInitialized,
    Unsupported,
    IoError,
    DeviceLost,        // GPU device removed/reset
    OutOfDate,         // e.g. swapchain out of date -> recreate (not fatal)
    CompilationFailed, // shader/script compile failure
    Internal,
};

struct Error {
    ErrorCode   code = ErrorCode::Unknown;
    std::string message;

    Error() = default;
    Error(ErrorCode c, std::string msg = {}) : code(c), message(std::move(msg)) {}

    [[nodiscard]] bool is(ErrorCode c) const noexcept { return code == c; }
};

// ---------------------------------------------------------------------------
// Result<T, E> — an expected-like value-or-error type (no exceptions required).
// Use Result<void> for operations that either succeed or return an Error.
// ---------------------------------------------------------------------------
template <typename T, typename E = Error>
class [[nodiscard]] Result {
public:
    Result(T value) : storage_(std::move(value)) {}     // NOLINT(google-explicit-constructor)
    Result(E error) : storage_(std::move(error)) {}     // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T&       value()       { return std::get<0>(storage_); }
    const T& value() const { return std::get<0>(storage_); }
    const E& error() const { return std::get<1>(storage_); }

    T value_or(T fallback) const {
        return has_value() ? std::get<0>(storage_) : std::move(fallback);
    }

    T*       operator->()       { return &std::get<0>(storage_); }
    const T* operator->() const { return &std::get<0>(storage_); }
    T&       operator*()        { return std::get<0>(storage_); }
    const T& operator*() const  { return std::get<0>(storage_); }

private:
    std::variant<T, E> storage_;
};

// Result<void> specialization.
template <typename E>
class [[nodiscard]] Result<void, E> {
public:
    Result() : has_(true) {}
    Result(E error) : has_(false), error_(std::move(error)) {} // NOLINT

    [[nodiscard]] bool has_value() const noexcept { return has_; }
    explicit operator bool() const noexcept { return has_; }
    const E& error() const { return error_; }

private:
    bool has_ = true;
    E    error_{};
};

// Convenience factory for the error path.
template <typename T = void>
inline Result<T> make_error(ErrorCode code, std::string msg = {}) {
    return Result<T>(Error{code, std::move(msg)});
}

// ---------------------------------------------------------------------------
// Assertions. AE_ASSERT: Debug-only. AE_VERIFY/AE_CHECK: always-on (returns cond).
// Implemented in core/src/assert.cpp; declared here to avoid heavy includes.
// ---------------------------------------------------------------------------
namespace detail {
[[noreturn]] void assert_fail(const char* expr, const char* file, int line, const char* msg);
void            assert_report(const char* expr, const char* file, int line, const char* msg);
} // namespace detail

} // namespace aether

#define AE_ASSERT_MSG(cond, msg)                                                                   \
    do {                                                                                           \
        if (!(cond)) [[unlikely]] {                                                                 \
            ::aether::detail::assert_fail(#cond, __FILE__, __LINE__, (msg));                        \
        }                                                                                          \
    } while (0)

#define AE_VERIFY_MSG(cond, msg)                                                                   \
    ([&]() -> bool {                                                                               \
        const bool _ae_ok = static_cast<bool>(cond);                                               \
        if (!_ae_ok) [[unlikely]] {                                                                 \
            ::aether::detail::assert_report(#cond, __FILE__, __LINE__, (msg));                      \
        }                                                                                          \
        return _ae_ok;                                                                             \
    }())

#if defined(AE_DEBUG) || !defined(NDEBUG)
#    define AE_ASSERT(cond) AE_ASSERT_MSG((cond), nullptr)
#else
#    define AE_ASSERT(cond) ((void)0)
#endif

#define AE_VERIFY(cond) AE_VERIFY_MSG((cond), nullptr)
#define AE_CHECK(cond)  AE_VERIFY_MSG((cond), nullptr)

// AE_UNREACHABLE — mark logically-impossible paths.
#define AE_UNREACHABLE()                                                                           \
    ::aether::detail::assert_fail("unreachable", __FILE__, __LINE__, nullptr)
