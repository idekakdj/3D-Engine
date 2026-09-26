// script_test_utils.h — a World + ScriptVM over a private temporary script root.
#pragma once

#include "aether/scene/world.h"
#include "aether/scripting/components.h"
#include "aether/scripting/script_vm.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

namespace aether::scripting::test {

inline std::filesystem::path make_temp_root() {
    static std::atomic<u32> counter{ 0 };
    const auto              base = std::filesystem::temp_directory_path() / "aether_script_tests";
    for (;;) {
        const auto dir = base / std::to_string(static_cast<u64>(std::chrono::steady_clock::now()
                                                                    .time_since_epoch()
                                                                    .count()) +
                                               counter.fetch_add(1));
        std::error_code ec;
        if (std::filesystem::create_directories(dir, ec)) {
            return dir;
        }
    }
}

struct ScriptFixture {
    std::filesystem::path     root = make_temp_root();
    World                     world; // declared before the VM: the VM dies first
    std::unique_ptr<ScriptVM> vm;

    ScriptFixture() = default;
    ~ScriptFixture() {
        vm.reset();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    ScriptFixture(const ScriptFixture&)            = delete;
    ScriptFixture& operator=(const ScriptFixture&) = delete;

    void write(std::string_view rel, std::string_view text) const {
        const auto path = root / std::filesystem::path(rel);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }

    ScriptVM& start(ScriptVMConfig config = {}) {
        config.script_root = root;
        config.hot_reload  = false; // tests drive reload_changed() explicitly
        vm                 = std::make_unique<ScriptVM>(world, config);
        return *vm;
    }

    // Creates an entity with a ScriptComponent (instances appear at the next update).
    Entity spawn(std::string_view script, std::string name = "Scripted") {
        const Entity    e = world.create(std::move(name));
        ScriptComponent c;
        c.script = std::string(script);
        world.add<ScriptComponent>(e, std::move(c));
        return e;
    }

    // Evaluates a console expression expected to succeed.
    ScriptValue eval(std::string_view expr) const {
        auto r = vm->evaluate(expr);
        if (!r) {
            FAIL_CHECK("evaluate(" << std::string(expr) << ") failed: " << r.error().message);
            return {};
        }
        return *r;
    }

    void run(std::string_view code) const {
        auto r = vm->run_string(code);
        if (!r) {
            FAIL_CHECK("run_string failed: " << r.error().message);
        }
    }

    template <typename T>
    T eval_as(std::string_view expr) const {
        const ScriptValue v = eval(expr);
        if (const T* p = std::get_if<T>(&v)) {
            return *p;
        }
        FAIL_CHECK("evaluate(" << std::string(expr) << ") returned another type (index " << v.index() << ")");
        return T{};
    }
};

} // namespace aether::scripting::test
