#pragma once

#include "diagnostics.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Product-internal shader compiler adapter for draxul-rezonality-project.
//
// Running glslangValidator is split into three replaceable steps so failure
// handling is testable without a real compiler:
//   1. plan_compiler_invocation  - pure: chooses arguments and working
//      directory for a platform path policy;
//   2. ProcessRunner             - OS boundary: spawn, capture, time out;
//   3. compile_shader            - interprets the typed process result into
//      SPIR-V or bounded DiagnosticEntry values.
// Temporary SPIR-V output files are named by the caller (build_candidate),
// which also removes them after each pass.
namespace rezonality
{

// Upper bound on compiler/scene diagnostics retained for one candidate.
inline constexpr size_t kMaximumBuildDiagnostics = 128;
// Merged stdout/stderr retained per compiler process. Output beyond this is
// still drained so the child cannot block on a full pipe.
inline constexpr size_t kMaximumCompilerOutputBytes = size_t{ 1 } << 20;
inline constexpr std::chrono::milliseconds kCompilerTimeout{ 10000 };

struct ProcessRequest
{
    // arguments[0] is the executable path.
    std::vector<std::filesystem::path> arguments;
    // Empty inherits the caller's working directory.
    std::filesystem::path working_directory;
    std::chrono::milliseconds timeout = kCompilerTimeout;
};

struct ProcessResult
{
    enum class Status
    {
        Exited,
        StartFailed,
        TimedOut,
    };

    Status status = Status::StartFailed;
    int exit_code = -1;
    // Merged stdout/stderr, capped at kMaximumCompilerOutputBytes.
    std::string output;
    bool output_truncated = false;
    // Start failure detail for StartFailed.
    std::string error;
};

using ProcessRunner = std::function<ProcessResult(const ProcessRequest&)>;

// Real OS process runner (CreateProcessW on Windows, posix_spawn elsewhere).
[[nodiscard]] ProcessResult run_process(const ProcessRequest& request);

// Windows command-line quoting for one argument, following the MSVC CRT
// argv parsing rules. Exposed on every platform so it can be tested.
[[nodiscard]] std::wstring quote_windows_argument(std::wstring_view value);

// How shader paths are spelled to the compiler.
enum class CompilerPathPolicy
{
    // Absolute shader, include, and output paths; inherited working
    // directory. Used on macOS, whose argv carries UTF-8 bytes unchanged.
    Absolute,
    // Child runs in the project directory and receives project-relative
    // shader and include paths. Used on Windows, where the bundled
    // glslangValidator receives a narrow argv converted through the active
    // code page: project-relative names avoid the (often international)
    // project and user-profile directories entirely.
    ProjectRelative,
};

// True when the compiler process can receive this path spelling intact.
using ArgumentEncodable = std::function<bool(const std::filesystem::path&)>;

[[nodiscard]] CompilerPathPolicy native_compiler_path_policy();
// Windows: the path round-trips through the active code page. POSIX: true.
[[nodiscard]] bool native_argument_encodable(const std::filesystem::path& path);

struct ShaderCompileRequest
{
    std::filesystem::path compiler;
    std::filesystem::path project_path;
    std::filesystem::path shader;
    std::filesystem::path output_path;
};

struct CompilerInvocation
{
    ProcessRequest process;
    // Nonempty when the request cannot be passed to the compiler intact.
    std::string error;
    std::filesystem::path error_path;
};

[[nodiscard]] CompilerInvocation plan_compiler_invocation(
    const ShaderCompileRequest& request, CompilerPathPolicy policy,
    const ArgumentEncodable& encodable);

// Appends glslangValidator diagnostics. Relative paths reported by the
// compiler are resolved against working_directory; output with no
// recognizable diagnostic produces one fallback entry for shader.
void parse_compiler_diagnostics(std::string_view output,
    const std::filesystem::path& shader,
    const std::filesystem::path& working_directory,
    std::vector<DiagnosticEntry>& diagnostics);

struct CompilerEnvironment
{
    ProcessRunner run = run_process;
    CompilerPathPolicy policy = native_compiler_path_policy();
    ArgumentEncodable encodable = native_argument_encodable;
};

// Plans, runs, and interprets one compile. On success spirv holds the module
// read from request.output_path; on failure at least one diagnostic is
// appended (bounded by kMaximumBuildDiagnostics).
[[nodiscard]] bool compile_shader(const ShaderCompileRequest& request,
    const CompilerEnvironment& environment, std::vector<uint32_t>& spirv,
    std::vector<DiagnosticEntry>& diagnostics);

// Bundled compiler location inside the staged plugin directory.
[[nodiscard]] std::filesystem::path bundled_compiler_path(
    const std::filesystem::path& plugin_directory);

} // namespace rezonality
