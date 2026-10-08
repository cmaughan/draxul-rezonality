#include <catch2/catch_test_macros.hpp>

#include "live_project.h"
#include "shader_compiler.h"
#include "surface_dimensions.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

namespace fs = std::filesystem;

fs::path plugin_root()
{
    return fs::path(DRAXUL_REZONALITY_TEST_ROOT);
}

// Unique per call so concurrent runs from other checkouts sharing the
// temporary directory cannot remove or populate each other's fixtures.
fs::path unique_temp_path(std::string_view name)
{
    return fs::temp_directory_path()
        / (std::string(name) + "-"
            + std::to_string(std::chrono::steady_clock::now()
                                 .time_since_epoch()
                                 .count()));
}

struct ProjectFixture
{
    explicit ProjectFixture(std::string_view example)
        : path(unique_temp_path(
              "draxul-rezonality-project-" + std::string(example)))
    {
        std::error_code error;
        fs::remove_all(path, error);
        error.clear();
        fs::copy(plugin_root() / "examples" / example, path,
            fs::copy_options::recursive, error);
        REQUIRE_FALSE(error);
    }

    ~ProjectFixture()
    {
        std::error_code error;
        fs::remove_all(path, error);
    }

    fs::path path;
};

struct RestoreRenamedDirectory
{
    ~RestoreRenamedDirectory()
    {
        std::error_code error;
        if (!fs::exists(original, error) && fs::exists(renamed, error))
            fs::rename(renamed, original, error);
    }

    fs::path original;
    fs::path renamed;
};

std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input);
    return { std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>() };
}

void write(const fs::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output);
    output << text;
    REQUIRE(output.good());
}

bool accept_shader(const fs::path&, std::vector<uint32_t>& spirv,
    std::vector<rezonality::DiagnosticEntry>&)
{
    spirv = { 0x07230203u };
    return true;
}

std::optional<rezonality::BuildResult> wait_for_result(
    rezonality::LiveProject& project)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (auto result = project.take_result())
            return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return std::nullopt;
}

// Redirects the process temporary directory for the lifetime of the guard.
// POSIX standard libraries consult TMPDIR; the MSVC library resolves the
// temporary directory through GetTempPath2W, which consults TMP then TEMP.
class TemporaryDirectoryOverride
{
public:
    explicit TemporaryDirectoryOverride(const fs::path& directory)
    {
        for (const char* name : kVariables)
        {
            const char* value = std::getenv(name);
            saved_.push_back(value ? std::optional<std::string>(value)
                                   : std::nullopt);
            set(name, directory.string().c_str());
        }
    }

    ~TemporaryDirectoryOverride()
    {
        for (size_t index = 0; index < std::size(kVariables); ++index)
        {
            if (saved_[index])
                set(kVariables[index], saved_[index]->c_str());
            else
                unset(kVariables[index]);
        }
    }

    TemporaryDirectoryOverride(const TemporaryDirectoryOverride&) = delete;
    TemporaryDirectoryOverride& operator=(
        const TemporaryDirectoryOverride&)
        = delete;

private:
#if defined(_WIN32)
    static constexpr const char* kVariables[] = { "TMP", "TEMP" };
    static void set(const char* name, const char* value)
    {
        _putenv_s(name, value);
    }
    static void unset(const char* name)
    {
        _putenv_s(name, "");
    }
#else
    static constexpr const char* kVariables[] = { "TMPDIR" };
    static void set(const char* name, const char* value)
    {
        setenv(name, value, 1);
    }
    static void unset(const char* name)
    {
        unsetenv(name);
    }
#endif
    std::vector<std::optional<std::string>> saved_;
};

rezonality::ProjectOptions options(const fs::path& path)
{
    rezonality::ProjectOptions result;
    result.project_path = path;
    return result;
}

} // namespace

TEST_CASE("Rezonality checks scaled surface dimensions before GPU conversion",
    "[rezonality][project][surface]")
{
    rezonality::ShaderBuild::Surface surface;
    surface.name = "History";
    rezonality::SurfaceDimensions dimensions;
    std::string error;
    surface.scale_x = 0.5f;
    surface.scale_y = 2.0f;
    REQUIRE(rezonality::checked_surface_dimensions(surface,
        800, 600, 4096, dimensions, error));
    CHECK(dimensions.width == 400);
    CHECK(dimensions.height == 1200);
    surface.scale_x = 1e30f;
    CHECK_FALSE(rezonality::checked_surface_dimensions(surface,
        800, 600, 4096, dimensions, error));
    CHECK(error.find("History") != std::string::npos);
    CHECK(error.find("width") != std::string::npos);
    surface.scale_x = 1.0f;
    surface.scale_y = 1e30f;
    CHECK_FALSE(rezonality::checked_surface_dimensions(surface,
        800, 600, 4096, dimensions, error));
    CHECK(error.find("height") != std::string::npos);
    surface.scale_y = 1.0f;
    surface.image_width = 5000;
    CHECK_FALSE(rezonality::checked_surface_dimensions(surface,
        800, 600, 4096, dimensions, error));
    surface.image_width = 64;
    REQUIRE(rezonality::checked_surface_dimensions(surface,
        800, 600, 4096, dimensions, error));
    CHECK(dimensions.width == 64);
    CHECK(dimensions.height == 600);

    ProjectFixture fixture("simple");
    const fs::path scenegraph = fixture.path / "default.scenegraph";
    write(scenegraph, read(scenegraph)
        + "\nsurface: Huge { scale: (1e30, 1) }\n");
    rezonality::ProjectPipeline pipeline(plugin_root(),
        options(fixture.path), accept_shader);
    const auto candidate = pipeline.build(1);
    REQUIRE(candidate.build);
    const auto huge = std::find_if(candidate.build->surfaces.begin(),
        candidate.build->surfaces.end(), [](const auto& item) {
            return item.name == "Huge";
        });
    REQUIRE(huge != candidate.build->surfaces.end());
    CHECK_FALSE(rezonality::checked_surface_dimensions(*huge,
        800, 600, 4096, dimensions, error));
    CHECK(error.find("Huge") != std::string::npos);
}

TEST_CASE("Rezonality project pipeline contains malformed numeric input",
    "[rezonality][project]")
{
    ProjectFixture fixture("simple");
    const fs::path scenegraph = fixture.path / "default.scenegraph";
    const std::string original = read(scenegraph);
    write(scenegraph, original
        + "\ncamera: Incomplete { field_of_view: - }\n");

    rezonality::ProjectPipeline pipeline(
        plugin_root(), options(fixture.path), accept_shader);
    const auto failed = pipeline.build(7);
    CHECK_FALSE(failed.build);
    CHECK(failed.generation == 7);
    CHECK(failed.diagnostic_path.filename() == "default.scenegraph");
    CHECK(failed.error.find("invalid finite number") != std::string::npos);

    write(scenegraph, original
        + "\nsurface: Incomplete { scale: (1, - }\n");
    const auto incomplete_vector = pipeline.build(8);
    CHECK_FALSE(incomplete_vector.build);
    CHECK(incomplete_vector.error.find("invalid numeric value")
        != std::string::npos);

    write(scenegraph, original
        + "\ncamera: Overflow { field_of_view: 1e999 }\n");
    const auto overflow = pipeline.build(9);
    CHECK_FALSE(overflow.build);
    CHECK(overflow.error.find("invalid finite number")
        != std::string::npos);

    write(scenegraph, original);
    const auto repaired = pipeline.build(10);
    REQUIRE(repaired.build);
    CHECK(repaired.generation == 10);
}

TEST_CASE("Rezonality project pipeline rejects image format mismatches",
    "[rezonality][project]")
{
    ProjectFixture fixture("default");
    const fs::path scenegraph = fixture.path / "default.scenegraph";
    std::ifstream input(scenegraph, std::ios::binary);
    REQUIRE(input);
    std::string scene{ std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>() };
    const std::string original = "surface: Noise { path: noise.png }";
    const auto position = scene.find(original);
    REQUIRE(position != std::string::npos);
    scene.replace(position, original.size(),
        "surface: Noise { path: noise.png format: rgba16f }");
    std::ofstream output(scenegraph, std::ios::binary | std::ios::trunc);
    REQUIRE(output);
    output << scene;
    output.close();

    rezonality::ProjectPipeline pipeline(
        plugin_root(), options(fixture.path), accept_shader);
    const auto failed = pipeline.build(3);
    CHECK_FALSE(failed.build);
    CHECK(failed.error.find("incompatible with decoded pixel storage")
        != std::string::npos);

    const std::string floating
        = "surface: Noise { path: noise.png format: rgba16f }";
    auto updated = scene.find(floating);
    REQUIRE(updated != std::string::npos);
    scene.replace(updated, floating.size(),
        "surface: Noise { path: noise.png format: default_depth }");
    write(scenegraph, scene);
    CHECK_FALSE(pipeline.build(4).build);

    const std::string depth
        = "surface: Noise { path: noise.png format: default_depth }";
    updated = scene.find(depth);
    REQUIRE(updated != std::string::npos);
    scene.replace(updated, depth.size(),
        "surface: Noise { path: noise.png format: rgba8 }");
    write(scenegraph, scene);
    const auto compatible = pipeline.build(5);
    REQUIRE(compatible.build);
    const auto noise = std::ranges::find(
        compatible.build->surfaces, "Noise",
        &rezonality::ShaderBuild::Surface::name);
    REQUIRE(noise != compatible.build->surfaces.end());
    CHECK(noise->format == rezonality::ShaderBuild::SurfaceFormat::Color8);
    CHECK(noise->image_pixels.size()
        == static_cast<size_t>(noise->image_width) * noise->image_height * 4);
}

TEST_CASE("Rezonality project pipeline validates HDR format overrides",
    "[rezonality][project]")
{
    ProjectFixture fixture("simple");
    const fs::path hdr = fixture.path / "sky.hdr";
    std::error_code error;
    fs::copy_file(plugin_root() / "examples" / "pbr_robot" / "textures"
            / "environment" / "farm_field_puresky_1k.hdr",
        hdr, fs::copy_options::overwrite_existing, error);
    REQUIRE_FALSE(error);

    const fs::path scenegraph = fixture.path / "default.scenegraph";
    const std::string original = read(scenegraph);
    rezonality::ProjectPipeline pipeline(
        plugin_root(), options(fixture.path), accept_shader);

    write(scenegraph, original
        + "\nenvironment: Sky { path: sky.hdr format: rgba16f }\n");
    const auto incompatible = pipeline.build(4);
    CHECK_FALSE(incompatible.build);
    CHECK(incompatible.error.find("incompatible with decoded pixel storage")
        != std::string::npos);

    write(scenegraph, original
        + "\nenvironment: Sky { path: sky.hdr format: default_depth }\n");
    CHECK_FALSE(pipeline.build(5).build);

    write(scenegraph, original
        + "\nenvironment: Sky { path: sky.hdr format: rgba32f }\n");
    const auto compatible = pipeline.build(6);
    REQUIRE(compatible.build);
    const auto sky = std::ranges::find(
        compatible.build->surfaces, "Sky",
        &rezonality::ShaderBuild::Surface::name);
    REQUIRE(sky != compatible.build->surfaces.end());
    CHECK(sky->format
        == rezonality::ShaderBuild::SurfaceFormat::Color32Float);
    CHECK(sky->image_float_pixels.size()
        == static_cast<size_t>(sky->image_width) * sky->image_height * 4);
}

TEST_CASE("Rezonality validates decoded image upload storage",
    "[rezonality][project][upload]")
{
    using Format = rezonality::ShaderBuild::SurfaceFormat;
    rezonality::ShaderBuild::Surface surface;
    surface.name = "Image";
    surface.image_width = 2;
    surface.image_height = 3;
    surface.format = Format::Color8;
    surface.image_pixels.resize(24);
    std::string error;
    CHECK(rezonality::validate_surface_upload_storage(surface, error));

    surface.format = Format::Color16Float;
    CHECK_FALSE(rezonality::validate_surface_upload_storage(surface, error));
    surface.format = Format::Depth32;
    CHECK_FALSE(rezonality::validate_surface_upload_storage(surface, error));

    surface.format = Format::Color8;
    surface.image_pixels.resize(23);
    CHECK_FALSE(rezonality::validate_surface_upload_storage(surface, error));

    surface.image_pixels.resize(24);
    surface.image_float_pixels.resize(24);
    surface.format = Format::Color32Float;
    CHECK_FALSE(rezonality::validate_surface_upload_storage(surface, error));
    surface.image_pixels.clear();
    CHECK(rezonality::validate_surface_upload_storage(surface, error));
    surface.format = Format::Color8;
    CHECK_FALSE(rezonality::validate_surface_upload_storage(surface, error));

    surface.image_float_pixels.resize(23);
    surface.format = Format::Color32Float;
    CHECK_FALSE(rezonality::validate_surface_upload_storage(surface, error));
    CHECK(error.find("invalid upload storage") != std::string::npos);
}

TEST_CASE("Rezonality live watch reports filesystem failure and recovers",
    "[rezonality][project][watch]")
{
    ProjectFixture fixture("simple");
    const fs::path unavailable = fixture.path.string() + ".unavailable";
    std::error_code error;
    fs::remove_all(unavailable, error);
    RestoreRenamedDirectory restore{ fixture.path, unavailable };
    auto configured = options(fixture.path);
    configured.compile_debounce_ms = 0;
    rezonality::LiveProject live(plugin_root(), configured, [] {},
        accept_shader);
    live.start();

    auto initial = wait_for_result(live);
    REQUIRE(initial);
    REQUIRE(initial->build);

    fs::rename(fixture.path, unavailable, error);
    REQUIRE_FALSE(error);
    auto failed = wait_for_result(live);
    REQUIRE(failed);
    CHECK_FALSE(failed->build);
    CHECK(failed->error.find("project watch failed") != std::string::npos);
    CHECK(failed->generation > initial->generation);

    fs::rename(unavailable, fixture.path, error);
    REQUIRE_FALSE(error);
    auto recovered = wait_for_result(live);
    REQUIRE(recovered);
    REQUIRE(recovered->build);
    CHECK(recovered->generation > failed->generation);
    live.stop();
}

TEST_CASE("Rezonality live rebuild reports unavailable temporary storage",
    "[rezonality][project][watch][temporary]")
{
    ProjectFixture fixture("simple");
    auto configured = options(fixture.path);
    configured.auto_reload = false;
    configured.compile_debounce_ms = 0;
    rezonality::LiveProject live(plugin_root(), configured, [] {},
        accept_shader);
    live.start();

    const auto initial = wait_for_result(live);
    REQUIRE(initial);
    REQUIRE(initial->build);

    std::optional<rezonality::BuildResult> failed;
    {
        const TemporaryDirectoryOverride missing_temporary_storage(
            fixture.path / "missing-temporary-storage");
        live.force_reload();
        failed = wait_for_result(live);
    }
    REQUIRE(failed);
    CHECK_FALSE(failed->build);
    CHECK(failed->generation > initial->generation);
    CHECK(failed->error.find("Temporary storage") != std::string::npos);

    live.force_reload();
    const auto recovered = wait_for_result(live);
    REQUIRE(recovered);
    REQUIRE(recovered->build);
    CHECK(recovered->generation > failed->generation);
    live.stop();
}

TEST_CASE("Rezonality live watch tracks extensionless nested shader includes",
    "[rezonality][project][watch]")
{
    ProjectFixture fixture("simple");
    const fs::path shader = fixture.path / "screen.frag";
    const fs::path first = fixture.path / "first.inc";
    const fs::path nested = fixture.path / "nested.inc";
    write(first, "#include \"nested.inc\"\n");
    write(nested, "// initial include\n");
    write(shader, read(shader) + "\n#include \"first.inc\"\n");
    auto configured = options(fixture.path);
    configured.compile_debounce_ms = 0;
    rezonality::LiveProject live(plugin_root(), configured, [] {},
        [&nested](const fs::path&, std::vector<uint32_t>& spirv,
            std::vector<rezonality::DiagnosticEntry>& diagnostics) {
            const std::string contents = read(nested);
            if (contents.find("broken") != std::string::npos)
            {
                diagnostics.push_back({
                    .path = nested,
                    .stage = "compile",
                    .severity = "error",
                    .line = 1,
                    .message = "broken nested include",
                });
                return false;
            }
            spirv = { 0x07230203u };
            return true;
        });
    live.start();
    const auto initial = wait_for_result(live);
    REQUIRE(initial);
    REQUIRE(initial->build);
    write(nested, "// changed include\n");
    const auto edited = wait_for_result(live);
    REQUIRE(edited);
    REQUIRE(edited->build);
    CHECK(edited->generation > initial->generation);
    write(nested, "broken\n");
    const auto broken = wait_for_result(live);
    REQUIRE(broken);
    CHECK_FALSE(broken->build);
    CHECK(broken->generation > edited->generation);
    CHECK(broken->diagnostic_path == nested);
    CHECK(broken->error == "broken nested include");
    write(nested, "// repaired include\n");
    const auto repaired = wait_for_result(live);
    REQUIRE(repaired);
    REQUIRE(repaired->build);
    CHECK(repaired->generation > broken->generation);
    live.stop();
}

TEST_CASE("Rezonality project pipeline reports injected compiler failures",
    "[rezonality][project]")
{
    ProjectFixture fixture("simple");
    auto configured = options(fixture.path);

    rezonality::ProjectPipeline empty_compiler(plugin_root(), configured,
        [](const fs::path&, std::vector<uint32_t>&,
            std::vector<rezonality::DiagnosticEntry>&) {
            return true;
        });
    const auto empty = empty_compiler.build(9);
    CHECK_FALSE(empty.build);
    CHECK(empty.error.find("produced no SPIR-V") != std::string::npos);

    rezonality::ProjectPipeline failed_compiler(plugin_root(), configured,
        [](const fs::path& shader, std::vector<uint32_t>&,
            std::vector<rezonality::DiagnosticEntry>& diagnostics) {
            diagnostics.push_back({
                .path = shader,
                .stage = "compile",
                .severity = "error",
                .line = 12,
                .message = "injected compiler diagnostic",
            });
            return false;
        });
    const auto failed = failed_compiler.build(10);
    CHECK_FALSE(failed.build);
    CHECK(failed.diagnostic_line == 12);
    CHECK(failed.error == "injected compiler diagnostic");
}

namespace
{

// Simulates a Windows code page that cannot represent any non-ASCII name.
bool ascii_only(const fs::path& path)
{
    const std::u8string text = path.u8string();
    return std::all_of(text.begin(), text.end(),
        [](char8_t value) { return static_cast<unsigned char>(value) < 0x80; });
}

std::string argument_text(const fs::path& argument)
{
    const std::u8string text = argument.generic_u8string();
    return std::string(text.begin(), text.end());
}

rezonality::ShaderCompileRequest compile_request(const fs::path& project,
    const fs::path& output)
{
    return {
        .compiler = fs::path("compiler") / "glslangValidator",
        .project_path = project,
        .shader = project / "shaders" / "main.frag",
        .output_path = output,
    };
}

// Records the planned process and replays a recorded compiler outcome.
struct RecordedCompiler
{
    rezonality::ProcessResult result;
    std::vector<uint32_t> written_spirv;
    fs::path output_path;
    std::vector<rezonality::ProcessRequest> requests;

    rezonality::CompilerEnvironment environment(
        rezonality::CompilerPathPolicy policy
        = rezonality::CompilerPathPolicy::Absolute)
    {
        return {
            .run = [this](const rezonality::ProcessRequest& request) {
                requests.push_back(request);
                if (!written_spirv.empty())
                {
                    std::ofstream output(output_path, std::ios::binary);
                    output.write(
                        reinterpret_cast<const char*>(written_spirv.data()),
                        static_cast<std::streamsize>(
                            written_spirv.size() * sizeof(uint32_t)));
                }
                return result;
            },
            .policy = policy,
            .encodable = ascii_only,
        };
    }
};

} // namespace

TEST_CASE("Rezonality compiler adapter plans platform argument spellings",
    "[rezonality][project][compiler]")
{
    const fs::path root = fs::temp_directory_path() / "rezonality-plan";
    const fs::path project = root / fs::path(u8"日本語 プロジェクト");
    const fs::path ascii_project = root / "ascii project";
    const fs::path output = root / "out" / "fragment-1-0.spv";
    const auto accept_all = [](const fs::path&) { return true; };

    // Absolute spelling is the unchanged macOS invocation.
    auto absolute = rezonality::plan_compiler_invocation(
        compile_request(ascii_project, output),
        rezonality::CompilerPathPolicy::Absolute, accept_all);
    REQUIRE(absolute.error.empty());
    CHECK(absolute.process.working_directory.empty());
    REQUIRE(absolute.process.arguments.size() >= 10);
    CHECK(absolute.process.arguments[0]
        == fs::path("compiler") / "glslangValidator");
    CHECK(absolute.process.arguments[4]
        == ascii_project / "shaders" / "main.frag");
    CHECK(absolute.process.arguments[6] == output);
    CHECK(argument_text(absolute.process.arguments[9])
        == "-I" + argument_text(ascii_project));

    // The same spelling cannot carry an unrepresentable project name.
    const auto rejected = rezonality::plan_compiler_invocation(
        compile_request(project, output),
        rezonality::CompilerPathPolicy::Absolute, ascii_only);
    CHECK(rejected.error.find("cannot be passed to glslangValidator")
        != std::string::npos);
    CHECK(rejected.error_path == project / "shaders" / "main.frag");

    // Project-relative spelling runs in the project and never names it.
    const auto relative = rezonality::plan_compiler_invocation(
        compile_request(project, output),
        rezonality::CompilerPathPolicy::ProjectRelative, ascii_only);
    REQUIRE(relative.error.empty());
    CHECK(relative.process.working_directory == project);
    CHECK(argument_text(relative.process.arguments[4]) == "shaders/main.frag");
    CHECK(relative.process.arguments[6] == output);
    CHECK(argument_text(relative.process.arguments[9]) == "-I.");
    for (size_t index = 1; index < relative.process.arguments.size(); ++index)
        CHECK(ascii_only(relative.process.arguments[index]));

    // Output under the same international ancestor skips the shared part.
    const auto shared = rezonality::plan_compiler_invocation(
        compile_request(project / "inner", project / "temp" / "out.spv"),
        rezonality::CompilerPathPolicy::ProjectRelative, ascii_only);
    REQUIRE(shared.error.empty());
    CHECK(argument_text(shared.process.arguments[6]) == "../temp/out.spv");

    // Output that can be spelled neither way fails actionably.
    const auto unreachable = rezonality::plan_compiler_invocation(
        compile_request(ascii_project, project / "out.spv"),
        rezonality::CompilerPathPolicy::ProjectRelative, ascii_only);
    CHECK(unreachable.error.find("Shader output directory")
        != std::string::npos);
    CHECK(unreachable.error.find("TMP/TEMP") != std::string::npos);
}

TEST_CASE("Rezonality quotes Windows compiler arguments for the CRT",
    "[rezonality][project][compiler]")
{
    using rezonality::quote_windows_argument;
    CHECK(quote_windows_argument(L"-V") == L"-V");
    CHECK(quote_windows_argument(L"C:\\a\\b.frag") == L"C:\\a\\b.frag");
    CHECK(quote_windows_argument(L"") == L"\"\"");
    CHECK(quote_windows_argument(L"C:\\my project\\a.frag")
        == L"\"C:\\my project\\a.frag\"");
    // Backslashes before the closing quote are doubled.
    CHECK(quote_windows_argument(L"-IC:\\my project\\")
        == L"\"-IC:\\my project\\\\\"");
    // Embedded quotes are escaped along with their preceding backslashes.
    CHECK(quote_windows_argument(L"a\\\"b c")
        == L"\"a\\\\\\\"b c\"");
    CHECK(quote_windows_argument(L"tab\there") == L"\"tab\there\"");
}

TEST_CASE("Rezonality compiler adapter interprets recorded process outcomes",
    "[rezonality][project][compiler]")
{
    const fs::path root = unique_temp_path(
        "draxul-rezonality-recorded-compiler");
    std::error_code ec;
    fs::remove_all(root, ec);
    REQUIRE(fs::create_directories(root / "out"));
    const fs::path project = root / "project";
    const fs::path output = root / "out" / "fragment.spv";
    const auto request = compile_request(project, output);
    const fs::path shader = request.shader;
    RecordedCompiler compiler;
    compiler.output_path = output;
    std::vector<uint32_t> spirv;
    std::vector<rezonality::DiagnosticEntry> diagnostics;

    SECTION("spawn failure")
    {
        compiler.result.status = rezonality::ProcessResult::Status::StartFailed;
        compiler.result.error = "Could not start glslangValidator (error 2)";
        CHECK_FALSE(rezonality::compile_shader(request,
            compiler.environment(), spirv, diagnostics));
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].path == shader);
        CHECK(diagnostics[0].stage == "compile");
        CHECK(diagnostics[0].message == compiler.result.error);
    }

    SECTION("timeout")
    {
        compiler.result.status = rezonality::ProcessResult::Status::TimedOut;
        CHECK_FALSE(rezonality::compile_shader(request,
            compiler.environment(), spirv, diagnostics));
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].message.find("timed out after 10s compiling main.frag")
            != std::string::npos);
        REQUIRE(compiler.requests.size() == 1);
        CHECK(compiler.requests[0].timeout == rezonality::kCompilerTimeout);
    }

    SECTION("relative compiler diagnostics resolve inside the project")
    {
        compiler.result.status = rezonality::ProcessResult::Status::Exited;
        compiler.result.exit_code = 2;
        // Recorded glslangValidator 15 output for a project-relative run.
        compiler.result.output = "shaders/main.frag\n"
                                 "ERROR: shaders/main.frag:5: 'undefined_thing' : undeclared identifier \n"
                                 "WARNING: shaders/common.glsl:2: 'x' : unused\n"
                                 "ERROR: 2 compilation errors.  No code generated.\n\n\n"
                                 "SPIR-V is not generated for failed compile or link\n";
        CHECK_FALSE(rezonality::compile_shader(request,
            compiler.environment(
                rezonality::CompilerPathPolicy::ProjectRelative),
            spirv, diagnostics));
        REQUIRE(compiler.requests.size() == 1);
        CHECK(compiler.requests[0].working_directory == project);
        REQUIRE(diagnostics.size() == 2);
        CHECK(diagnostics[0].path == project / "shaders" / "main.frag");
        CHECK(diagnostics[0].severity == "error");
        CHECK(diagnostics[0].line == 5);
        CHECK(diagnostics[0].message
            == "'undefined_thing' : undeclared identifier");
        CHECK(diagnostics[1].path == project / "shaders" / "common.glsl");
        CHECK(diagnostics[1].severity == "warning");
        CHECK(diagnostics[1].line == 2);
    }

    SECTION("absolute compiler diagnostics keep their path")
    {
        compiler.result.status = rezonality::ProcessResult::Status::Exited;
        compiler.result.exit_code = 2;
        compiler.result.output = "ERROR: " + argument_text(shader)
            + ":12: 'main' : missing\n";
        CHECK_FALSE(rezonality::compile_shader(request,
            compiler.environment(), spirv, diagnostics));
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].path.lexically_normal()
            == shader.lexically_normal());
        CHECK(diagnostics[0].line == 12);
    }

    SECTION("link and usage errors fall back to the shader")
    {
        compiler.result.status = rezonality::ProcessResult::Status::Exited;
        compiler.result.exit_code = 3;
        compiler.result.output = "shaders/main.frag\n"
                                 "ERROR: Linking fragment stage: Missing entry point: Each stage requires one entry point\n\n"
                                 "SPIR-V is not generated for failed compile or link\n";
        CHECK_FALSE(rezonality::compile_shader(request,
            compiler.environment(), spirv, diagnostics));
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].path == shader);
        CHECK(diagnostics[0].message.rfind("ERROR: Linking fragment stage", 0)
            == 0);

        diagnostics.clear();
        compiler.result.exit_code = 1;
        compiler.result.output = "glslangValidator: Error: unable to open input file (use -h for usage)\n";
        CHECK_FALSE(rezonality::compile_shader(request,
            compiler.environment(), spirv, diagnostics));
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].message.find("unable to open input file")
            != std::string::npos);

        diagnostics.clear();
        compiler.result.output.clear();
        CHECK_FALSE(rezonality::compile_shader(request,
            compiler.environment(), spirv, diagnostics));
        REQUIRE(diagnostics.size() == 1);
        CHECK_FALSE(diagnostics[0].message.empty());
    }

    SECTION("diagnostics are bounded and undecodable names do not throw")
    {
        compiler.result.status = rezonality::ProcessResult::Status::Exited;
        compiler.result.exit_code = 2;
        compiler.result.output = "ERROR: caf\xe9.frag:3: code page name\n";
        for (int line = 1; line <= 300; ++line)
            compiler.result.output += "ERROR: shaders/main.frag:"
                + std::to_string(line) + ": repeated\n";
        CHECK_NOTHROW(rezonality::compile_shader(request,
            compiler.environment(), spirv, diagnostics));
        CHECK(diagnostics.size() == rezonality::kMaximumBuildDiagnostics);
        CHECK(diagnostics[0].line == 3);
    }

    SECTION("successful exit reads the requested output")
    {
        compiler.result.status = rezonality::ProcessResult::Status::Exited;
        compiler.result.exit_code = 0;
        compiler.written_spirv = { 0x07230203u, 0x00010500u, 7u };
        REQUIRE(rezonality::compile_shader(request,
            compiler.environment(
                rezonality::CompilerPathPolicy::ProjectRelative),
            spirv, diagnostics));
        CHECK(spirv == compiler.written_spirv);
        CHECK(diagnostics.empty());
    }

    SECTION("successful exit without output keeps the compiler explanation")
    {
        compiler.result.status = rezonality::ProcessResult::Status::Exited;
        compiler.result.exit_code = 0;
        compiler.result.output = "shaders/main.frag\nERROR: Failed to open file: out/fragment.spv\n";
        CHECK_FALSE(rezonality::compile_shader(request,
            compiler.environment(), spirv, diagnostics));
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0].message.find("produced no SPIR-V for main.frag")
            != std::string::npos);
        CHECK(diagnostics[0].message.find("Failed to open file")
            != std::string::npos);
    }

    fs::remove_all(root, ec);
}

TEST_CASE("Rezonality bundled compiler builds international projects",
    "[rezonality][project][compiler][international]")
{
    const fs::path compiler = rezonality::bundled_compiler_path(plugin_root());
    if (!fs::is_regular_file(compiler))
        SKIP("No bundled glslangValidator for this platform");

    const fs::path root = unique_temp_path("draxul-rezonality-compiler");
    const fs::path project = root / fs::path(u8"日本語 プロジェクト-\U0001F3B5");
    REQUIRE(fs::create_directories(project / "shaders"));
    REQUIRE(fs::create_directories(root / "out"));
    write(project / "shaders" / "common.glsl",
        "vec4 tint() { return vec4(1.0); }\n");
    // Includes resolve beside the shader and from the project root.
    write(project / "shaders" / "local.frag",
        "#version 450\n"
        "#extension GL_GOOGLE_include_directive : require\n"
        "#include \"common.glsl\"\n"
        "layout(location = 0) out vec4 color;\n"
        "void main() { color = tint(); }\n");
    write(project / "shaders" / "rooted.frag",
        "#version 450\n"
        "#extension GL_GOOGLE_include_directive : require\n"
        "#include \"shaders/common.glsl\"\n"
        "layout(location = 0) out vec4 color;\n"
        "void main() { color = tint(); }\n");
    write(project / "shaders" / "broken.frag",
        "#version 450\n"
        "layout(location = 0) out vec4 color;\n"
        "void main()\n"
        "{\n"
        "    color = undefined_value;\n"
        "}\n");

    const auto build = [&](const rezonality::CompilerEnvironment& environment,
                           const char* shader_name,
                           std::vector<rezonality::DiagnosticEntry>& diagnostics) {
        std::vector<uint32_t> spirv;
        const fs::path output = root / "out" / "shader.spv";
        const bool compiled = rezonality::compile_shader({
                                                             .compiler = compiler,
                                                             .project_path = project,
                                                             .shader = project / "shaders" / shader_name,
                                                             .output_path = output,
                                                         },
            environment, spirv, diagnostics);
        std::error_code ec;
        fs::remove(output, ec);
        return compiled && !spirv.empty() && spirv.front() == 0x07230203u;
    };

    rezonality::CompilerEnvironment native;
    rezonality::CompilerEnvironment restricted;
    restricted.policy = rezonality::CompilerPathPolicy::ProjectRelative;
    restricted.encodable = ascii_only;
    for (const auto* environment : { &native, &restricted })
    {
        DYNAMIC_SECTION((environment == &native ? "native" : "restricted"))
        {
            std::vector<rezonality::DiagnosticEntry> diagnostics;
            CHECK(build(*environment, "local.frag", diagnostics));
            CHECK(build(*environment, "rooted.frag", diagnostics));
            for (const auto& diagnostic : diagnostics)
                UNSCOPED_INFO(diagnostic.message);
            CHECK(diagnostics.empty());

            CHECK_FALSE(build(*environment, "broken.frag", diagnostics));
            REQUIRE_FALSE(diagnostics.empty());
            CHECK(diagnostics[0].path.lexically_normal()
                == (project / "shaders" / "broken.frag").lexically_normal());
            CHECK(diagnostics[0].line == 5);
            CHECK(diagnostics[0].message.find("undefined_value")
                != std::string::npos);
        }
    }

    std::vector<rezonality::DiagnosticEntry> missing;
    std::vector<uint32_t> spirv;
    CHECK_FALSE(rezonality::compile_shader({
                                               .compiler = root / "missing-compiler",
                                               .project_path = project,
                                               .shader = project / "shaders" / "local.frag",
                                               .output_path = root / "out" / "missing.spv",
                                           },
        {}, spirv, missing));
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].message.find("Could not start glslangValidator")
        != std::string::npos);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("Rezonality parses scene text before resolving assets",
    "[rezonality][project][pipeline]")
{
    rezonality::ProjectOptions configured;
    configured.project_path = fs::path("virtual-project");
    const fs::path scenegraph
        = configured.project_path / "ordered.scenegraph";
    const std::string source = R"scene(
model: First {
    path: models/first.obj
    scale: (1, 2, 3)
}
model: Second {
    path: models/second.obj
    uv_origin: upper_left
}
surface: Color { path: textures/color.png }
pass: UseSecond {
    vs: shaders/second.vert
    fs: shaders/second.frag
    geometry: Mesh { model: Second }
}
pass: UseFirst {
    vs: shaders/first.vert
    fs: shaders/first.frag
    geometry: Mesh { model: First }
}
)scene";

    auto parsed = rezonality::parse_scene_text(
        configured, scenegraph, source);
    REQUIRE(parsed.scene);
    REQUIRE(parsed.scene->models.size() == 2);
    CHECK(parsed.scene->models[0].name == "First");
    CHECK(parsed.scene->models[0].path
        == configured.project_path / "models/first.obj");
    CHECK(parsed.scene->models[0].scale.x == 1.0f);
    CHECK(parsed.scene->models[0].scale.y == 2.0f);
    CHECK(parsed.scene->models[0].scale.z == 3.0f);
    CHECK(parsed.scene->models[0].flip_texture_y);
    CHECK(parsed.scene->models[1].name == "Second");
    CHECK_FALSE(parsed.scene->models[1].flip_texture_y);
    REQUIRE(parsed.scene->surfaces.size() == 1);
    CHECK(parsed.scene->surfaces[0].path
        == configured.project_path / "textures/color.png");
    REQUIRE(parsed.scene->passes.size() == 2);
    REQUIRE(parsed.scene->passes[0].model_index);
    REQUIRE(parsed.scene->passes[1].model_index);
    CHECK(*parsed.scene->passes[0].model_index == 1);
    CHECK(*parsed.scene->passes[1].model_index == 0);

    const auto malformed = rezonality::parse_scene_text(configured,
        scenegraph, "pass: Broken { geometry: Screen { path: screen_rect }");
    CHECK_FALSE(malformed.scene);
    CHECK(malformed.diagnostic_path == scenegraph);
    CHECK(malformed.error.find("at least one enabled pass")
        != std::string::npos);
}

TEST_CASE("Rezonality candidate resolution reports missing assets",
    "[rezonality][project][pipeline]")
{
    rezonality::ProjectOptions configured;
    configured.project_path = fs::temp_directory_path()
        / "draxul-rezonality-missing-candidate";
    const fs::path scenegraph
        = configured.project_path / "missing.scenegraph";
    const fs::path output = configured.project_path / "output";
    std::error_code ec;
    fs::remove_all(configured.project_path, ec);

    const auto missing_model = rezonality::parse_scene_text(configured,
        scenegraph, R"scene(
model: Missing { path: models/missing.obj }
pass: Main {
    vs: main.vert
    fs: main.frag
    geometry: Mesh { model: Missing }
}
)scene");
    REQUIRE(missing_model.scene);
    const auto model_result = rezonality::build_candidate(plugin_root(),
        configured, *missing_model.scene, 21, output, accept_shader);
    CHECK_FALSE(model_result.build);
    CHECK(model_result.generation == 21);
    CHECK(model_result.diagnostic_path
        == configured.project_path / "models/missing.obj");
    CHECK(model_result.error.find("Could not load model")
        != std::string::npos);

    const auto missing_image = rezonality::parse_scene_text(configured,
        scenegraph, R"scene(
surface: Missing { path: textures/missing.png }
pass: Main {
    vs: main.vert
    fs: main.frag
    geometry: Screen { path: screen_rect }
}
)scene");
    REQUIRE(missing_image.scene);
    const auto image_result = rezonality::build_candidate(plugin_root(),
        configured, *missing_image.scene, 22, output, accept_shader);
    CHECK_FALSE(image_result.build);
    CHECK(image_result.diagnostic_path
        == configured.project_path / "textures/missing.png");
    CHECK(image_result.error.find("Could not load texture")
        != std::string::npos);
    fs::remove_all(configured.project_path, ec);
}

TEST_CASE("Rezonality candidate seam owns compiler output and diagnostics",
    "[rezonality][project][pipeline]")
{
    rezonality::ProjectOptions configured;
    configured.project_path = fs::path("virtual-project");
    const fs::path scenegraph
        = configured.project_path / "compiler.scenegraph";
    const auto parsed = rezonality::parse_scene_text(configured, scenegraph,
        R"scene(
pass: Main {
    vs: shaders/main.vert
    fs: shaders/main.frag
    geometry: Screen { path: screen_rect }
}
)scene");
    REQUIRE(parsed.scene);
    const fs::path output = fs::temp_directory_path()
        / "draxul-rezonality-candidate-seam";

    auto owned_scene = *parsed.scene;
    const auto built = rezonality::build_candidate(plugin_root(), configured,
        owned_scene, 31, output,
        [](const fs::path& shader, std::vector<uint32_t>& spirv,
            std::vector<rezonality::DiagnosticEntry>&) {
            spirv = { 0x07230203u,
                shader.extension() == ".vert" ? 1u : 2u };
            return true;
        });
    REQUIRE(built.build);
    CHECK(built.build->generation == 31);
    CHECK(built.build->scenegraph_path == scenegraph);
    REQUIRE(built.build->passes.size() == 1);
    const std::vector<uint32_t> expected_vertex{ 0x07230203u, 1u };
    const std::vector<uint32_t> expected_fragment{ 0x07230203u, 2u };
    CHECK(built.build->passes[0].vertex_spirv
        == expected_vertex);
    CHECK(built.build->passes[0].fragment_spirv
        == expected_fragment);
    owned_scene.passes[0].name = "mutated after build";
    CHECK(built.build->passes[0].name == "Main");

    const auto empty = rezonality::build_candidate(plugin_root(), configured,
        *parsed.scene, 32, output,
        [](const fs::path&, std::vector<uint32_t>&,
            std::vector<rezonality::DiagnosticEntry>&) { return true; });
    CHECK_FALSE(empty.build);
    CHECK(empty.error.find("produced no SPIR-V") != std::string::npos);

    const auto failed = rezonality::build_candidate(plugin_root(), configured,
        *parsed.scene, 33, output,
        [](const fs::path& shader, std::vector<uint32_t>&,
            std::vector<rezonality::DiagnosticEntry>& diagnostics) {
            diagnostics.push_back({
                .path = shader,
                .stage = "compile",
                .severity = "error",
                .line = 17,
                .message = "injected seam diagnostic",
            });
            return false;
        });
    CHECK_FALSE(failed.build);
    CHECK(failed.diagnostic_path
        == configured.project_path / "shaders/main.vert");
    CHECK(failed.diagnostic_line == 17);
    CHECK(failed.error == "injected seam diagnostic");

    std::error_code ec;
    fs::remove_all(output, ec);
}
