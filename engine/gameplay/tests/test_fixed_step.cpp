// test_fixed_step.cpp — FixedStepAccumulator (header-only).
#include "aether/gameplay/fixed_step.h"

#include <doctest/doctest.h>

using namespace aether;
using namespace aether::gameplay;

TEST_CASE("fixed step: whole steps, remainder and alpha") {
    FixedStepAccumulator acc(0.01f, 8);
    CHECK(acc.advance(0.025f) == 2);
    CHECK(acc.remainder() == doctest::Approx(0.005));
    CHECK(acc.alpha() == doctest::Approx(0.5f));
    CHECK(acc.advance(0.005f) == 1);
    CHECK(acc.remainder() == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(acc.total_steps() == 3);
}

TEST_CASE("fixed step: frame delta equal to the step never jitters") {
    FixedStepAccumulator acc(1.0f / 60.0f, 8);
    for (int i = 0; i < 600; ++i) {
        CHECK(acc.advance(1.0f / 60.0f) == 1);
    }
    CHECK(acc.total_steps() == 600);
}

TEST_CASE("fixed step: spiral-of-death clamp drops excess time") {
    FixedStepAccumulator acc(0.01f, 4);
    CHECK(acc.advance(1.0f) == 4);
    CHECK(acc.remainder() < 0.01);
    CHECK(acc.dropped_steps() == doctest::Approx(96.0));
    CHECK(acc.advance(0.0f) == 0);
}

TEST_CASE("fixed step: negative deltas, reset and reconfigure") {
    FixedStepAccumulator acc(0.02f, 8);
    CHECK(acc.advance(-1.0f) == 0);
    CHECK(acc.advance(0.015f) == 0);
    acc.reset();
    CHECK(acc.remainder() == 0.0);
    acc.configure(0.0f, 2); // invalid step size falls back to 1/60
    CHECK(acc.fixed_delta() == doctest::Approx(1.0f / 60.0f));
    CHECK(acc.max_steps() == 2);
}
