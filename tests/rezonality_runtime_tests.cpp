#include <catch2/catch_test_macros.hpp>

#include "runtime_controller.h"

namespace
{

rezonality::BuildResult candidate(uint64_t generation,
    size_t pass_count = 1, size_t surface_count = 1)
{
    rezonality::ShaderBuild build;
    build.generation = generation;
    build.passes.resize(pass_count);
    build.surfaces.resize(surface_count);
    return {
        .generation = generation,
        .build = std::move(build),
    };
}

} // namespace

TEST_CASE("Rezonality runtime activates candidates without invalid aliases",
    "[rezonality][runtime]")
{
    rezonality::RuntimeController runtime;
    auto first = candidate(1, 2, 3);
    CHECK(runtime.accept(first).status == "ready g1");
    REQUIRE(runtime.desired(false));
    CHECK(runtime.desired(false)->generation == 1);

    const auto activated = runtime.activate_prepared();
    CHECK(activated.active_generation == 1);
    CHECK(activated.pass_count == 2);
    CHECK(activated.surface_count == 3);
    CHECK(activated.status == "live g1 | 2 passes | 3 surfaces");
    REQUIRE(runtime.active_build());
    CHECK(runtime.active_build()->generation == 1);
    CHECK(runtime.desired(true) == nullptr);

    // Backend-incompatible resize/target recreation selects the immutable
    // active build and can commit it without invalidating its own alias.
    REQUIRE(runtime.desired(false));
    CHECK(runtime.desired(false)->generation == 1);
    CHECK(runtime.activate_prepared().active_generation == 1);
}

TEST_CASE("Rezonality runtime preserves last good state after failures",
    "[rezonality][runtime]")
{
    rezonality::RuntimeController runtime;
    auto first = candidate(4);
    runtime.accept(first);
    runtime.activate_prepared();

    rezonality::BuildResult broken;
    broken.generation = 5;
    broken.error = "compile failed";
    const auto build_failure = runtime.accept(broken);
    CHECK(build_failure.active_generation == 4);
    CHECK(build_failure.status
        == "BUILD FAILED g5 | rendering last good g4 | compile failed");
    REQUIRE(runtime.active_build());
    CHECK(runtime.active_build()->generation == 4);

    auto retry = candidate(6);
    runtime.accept(retry);
    const auto prepare_failure = runtime.reject_prepared("GPU allocation failed");
    CHECK(prepare_failure.active_generation == 4);
    CHECK(prepare_failure.status
        == "BUILD FAILED g6 | rendering last good g4 | GPU allocation failed");
    CHECK(runtime.desired(true) == nullptr);
    REQUIRE(runtime.active_build());
    CHECK(runtime.active_build()->generation == 4);

    auto repaired = candidate(7, 2, 3);
    CHECK(runtime.accept(repaired).status == "ready g7");
    const auto activated_retry = runtime.activate_prepared();
    CHECK(activated_retry.active_generation == 7);
    CHECK(activated_retry.pass_count == 2);
    CHECK(activated_retry.surface_count == 3);
    REQUIRE(runtime.active_build());
    CHECK(runtime.active_build()->generation == 7);
}
