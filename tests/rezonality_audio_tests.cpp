#include <catch2/catch_test_macros.hpp>

#include "audio_analysis.h"

#include <algorithm>

TEST_CASE("Rezonality synthetic and silent audio are deterministic",
    "[rezonality][audio]")
{
    rezonality::AudioOptions options;
    options.source = rezonality::AudioOptions::Source::Synthetic;
    rezonality::AudioAnalyzer analyzer(options);

    const auto first = analyzer.frame();
    const auto second = analyzer.frame();
    REQUIRE(first.rgba.size()
        == rezonality::AudioTextureFrame::width
            * rezonality::AudioTextureFrame::height * 4);
    CHECK(first.generation == 1);
    CHECK(first.status == "audio synthetic fixture");
    CHECK(first.rgba == second.rgba);
    CHECK(std::any_of(first.rgba.begin(), first.rgba.end(),
        [](float value) { return value > 0.1f && value < 0.99f; }));

    analyzer.set_visible(false);
    CHECK(analyzer.frame().rgba == first.rgba);
    analyzer.set_visible(true);
    CHECK(analyzer.frame().rgba == first.rgba);

    options.source = rezonality::AudioOptions::Source::Silent;
    rezonality::AudioAnalyzer silent(options);
    const auto fallback = silent.frame();
    CHECK(fallback.generation == 1);
    CHECK(fallback.status.find("audio unavailable") != std::string::npos);
    CHECK(fallback.rgba.size() == first.rgba.size());
    CHECK(std::all_of(fallback.rgba.begin(), fallback.rgba.end(),
        [](float value) { return value == 0.0f || value == 1.0f; }));
}
