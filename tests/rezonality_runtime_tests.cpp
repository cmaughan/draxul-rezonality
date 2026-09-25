#include <catch2/catch_test_macros.hpp>

#include "runtime_controller.h"
#include "animation_clock.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

TEST_CASE("Rezonality animation excludes idle and rendered paused intervals",
    "[rezonality][runtime][clock]")
{
    rezonality::AnimationClock clock;
    CHECK(clock.advance(10.0) == 0.0);
    CHECK(clock.advance(11.0) == 1.0);
    clock.set_paused(true);
    CHECK(clock.advance(100.0) == 1.0);
    CHECK(clock.advance(200.0) == 1.0);
    clock.set_paused(false);
    CHECK(clock.advance(500.0) == 1.0);
    CHECK(clock.advance(500.5) == 1.5);
    clock.set_paused(true);
    clock.set_paused(false);
    CHECK(clock.advance(900.0) == 1.5);
    CHECK(clock.advance(901.0) == 2.5);
    clock.elapsed_seconds = 12.5;
    clock.last_seconds = -1.0;
    CHECK(clock.advance(1200.0) == 12.5);
}

namespace
{

struct FakeBackend final : rezonality::RuntimeBackend
{
    bool active_compatible() const override
    {
        return compatible;
    }

    rezonality::BackendPreparation prepare(
        const rezonality::ShaderBuild& build) override
    {
        events.push_back("prepare g" + std::to_string(build.generation));
        prepared_generation = build.generation;
        return { prepare_succeeds,
            prepare_succeeds ? std::string{} : failure };
    }

    void activate_prepared() override
    {
        events.push_back("activate g"
            + std::to_string(prepared_generation));
        activated_generations.push_back(prepared_generation);
    }

    void retire_completed_slot(uint32_t frame_index) override
    {
        events.push_back("retire slot " + std::to_string(frame_index));
        retired_slots.push_back(frame_index);
    }

    bool compatible = false;
    bool prepare_succeeds = true;
    uint64_t prepared_generation = 0;
    std::string failure = "injected backend preparation failure";
    std::vector<std::string> events;
    std::vector<uint64_t> activated_generations;
    std::vector<uint32_t> retired_slots;
};

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

TEST_CASE("Rezonality runtime drives fake backend preparation and recreation",
    "[rezonality][runtime][backend]")
{
    rezonality::RuntimeController runtime;
    FakeBackend backend;
    auto first = candidate(1, 2, 3);
    runtime.accept(first);
    CHECK(runtime.attempted_generation() == 1);

    const auto activated = runtime.prepare_frame(backend, 2);
    CHECK(activated.disposition
        == rezonality::RuntimePrepareDisposition::Activated);
    CHECK(activated.transition.active_generation == 1);
    REQUIRE(backend.events.size() == 3);
    CHECK(backend.events[0] == "retire slot 2");
    CHECK(backend.events[1] == "prepare g1");
    CHECK(backend.events[2] == "activate g1");

    backend.compatible = true;
    const auto unchanged = runtime.prepare_frame(backend, 0);
    CHECK(unchanged.disposition
        == rezonality::RuntimePrepareDisposition::Unchanged);
    REQUIRE(backend.events.size() == 4);
    CHECK(backend.events.back() == "retire slot 0");

    backend.compatible = false;
    const auto recreated = runtime.prepare_frame(backend, 1);
    CHECK(recreated.disposition
        == rezonality::RuntimePrepareDisposition::Activated);
    REQUIRE(backend.activated_generations.size() == 2);
    CHECK(backend.activated_generations[1] == 1);
    REQUIRE(backend.events.size() == 7);
    CHECK(backend.events[4] == "retire slot 1");
    CHECK(backend.events[5] == "prepare g1");
    CHECK(backend.events[6] == "activate g1");
}

TEST_CASE("Rezonality runtime rejects fake backend failure and retries",
    "[rezonality][runtime][backend]")
{
    rezonality::RuntimeController runtime;
    FakeBackend backend;
    auto first = candidate(4);
    runtime.accept(first);
    runtime.prepare_frame(backend, 0);

    auto replacement = candidate(5);
    runtime.accept(replacement);
    CHECK(runtime.attempted_generation() == 5);
    backend.prepare_succeeds = false;
    const auto rejected = runtime.prepare_frame(backend, 1);
    CHECK(rejected.disposition
        == rezonality::RuntimePrepareDisposition::Rejected);
    CHECK(rejected.error == backend.failure);
    CHECK(rejected.transition.active_generation == 4);
    CHECK(rejected.transition.status
        == "BUILD FAILED g5 | rendering last good g4 | " + backend.failure);
    REQUIRE(runtime.active_build());
    CHECK(runtime.active_build()->generation == 4);
    REQUIRE(backend.activated_generations.size() == 1);
    CHECK(backend.activated_generations.front() == 4);

    auto retry = candidate(6);
    runtime.accept(retry);
    backend.prepare_succeeds = true;
    const auto repaired = runtime.prepare_frame(backend, 2);
    CHECK(repaired.disposition
        == rezonality::RuntimePrepareDisposition::Activated);
    CHECK(repaired.transition.active_generation == 6);
    REQUIRE(backend.activated_generations.size() == 2);
    CHECK(backend.activated_generations.back() == 6);
}

TEST_CASE("Rezonality runtime identifies activation older than latest attempt",
    "[rezonality][runtime][backend]")
{
    rezonality::RuntimeController runtime;
    FakeBackend backend;

    auto first = candidate(1);
    runtime.accept(first);

    rezonality::BuildResult broken;
    broken.generation = 2;
    broken.error = "compile failed";
    runtime.accept(broken);

    const auto late_activation = runtime.prepare_frame(backend, 0);
    REQUIRE(late_activation.disposition
        == rezonality::RuntimePrepareDisposition::Activated);
    CHECK(late_activation.transition.active_generation == 1);
    CHECK(late_activation.transition.attempted_generation == 2);
    CHECK_FALSE(late_activation.activated_latest_attempt());

    auto repaired = candidate(3);
    runtime.accept(repaired);
    const auto current_activation = runtime.prepare_frame(backend, 1);
    REQUIRE(current_activation.disposition
        == rezonality::RuntimePrepareDisposition::Activated);
    CHECK(current_activation.transition.active_generation == 3);
    CHECK(current_activation.transition.attempted_generation == 3);
    CHECK(current_activation.activated_latest_attempt());
}

TEST_CASE("Rezonality runtime owns hidden and quiesced policy",
    "[rezonality][runtime][lifecycle]")
{
    rezonality::RuntimeController runtime;
    CHECK(runtime.visible());
    CHECK_FALSE(runtime.quiesced());
    CHECK(runtime.should_render());

    runtime.set_visible(false);
    CHECK_FALSE(runtime.should_render());
    runtime.set_visible(true);
    CHECK(runtime.should_render());

    runtime.set_quiesced(true);
    CHECK(runtime.quiesced());
    CHECK_FALSE(runtime.should_render());
    runtime.set_quiesced(false);
    CHECK(runtime.should_render());
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
