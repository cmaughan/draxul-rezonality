#include "shader_compiler.h"
#include "path_utf8.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <regex>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace rezonality
{
namespace
{

namespace fs = std::filesystem;

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

void append_output(ProcessResult& result, const char* data, size_t count)
{
    const size_t room = kMaximumCompilerOutputBytes
        - std::min(kMaximumCompilerOutputBytes, result.output.size());
    if (count > room)
        result.output_truncated = true;
    result.output.append(data, std::min(count, room));
}

std::vector<uint32_t> read_spirv(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {};
    const auto size = input.tellg();
    if (size <= 0 || size % 4 != 0)
        return {};
    std::vector<uint32_t> words(static_cast<size_t>(size) / 4);
    input.seekg(0);
    input.read(reinterpret_cast<char*>(words.data()), size);
    return input ? words : std::vector<uint32_t>{};
}

void push_diagnostic(std::vector<DiagnosticEntry>& diagnostics,
    const fs::path& path, std::string message)
{
    if (diagnostics.size() >= kMaximumBuildDiagnostics)
        return;
    diagnostics.push_back({
        .path = path,
        .stage = "compile",
        .severity = "error",
        .message = std::move(message),
    });
}

// Compiler text is UTF-8 on POSIX. A Windows compiler may report names in
// the active code page; never let an undecodable name escape as an
// exception that would replace every collected diagnostic.
fs::path reported_path(const std::string& text,
    const fs::path& working_directory)
{
    fs::path path;
    try
    {
        path = fs::u8path(text);
    }
    catch (const std::exception&)
    {
        return {};
    }
    if (path.is_relative() && !working_directory.empty())
        path = (working_directory / path).lexically_normal();
    return path;
}

// Picks the first spelling of a path that the compiler can receive intact.
std::optional<fs::path> first_encodable(
    std::initializer_list<fs::path> spellings,
    const ArgumentEncodable& encodable)
{
    for (const fs::path& spelling : spellings)
        if (!spelling.empty() && (!encodable || encodable(spelling)))
            return spelling;
    return std::nullopt;
}

std::string unencodable_message(std::string_view what, const fs::path& path)
{
    return std::string(what) + " '" + display_path_utf8(path)
        + "' cannot be passed to glslangValidator: it contains characters "
          "outside the system code page";
}

#if !defined(_WIN32)

int add_working_directory(posix_spawn_file_actions_t& actions,
    const fs::path& directory)
{
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    return posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
}

#endif

} // namespace

std::wstring quote_windows_argument(std::wstring_view value)
{
    bool quote = value.empty();
    for (const wchar_t ch : value)
        quote = quote || ch == L' ' || ch == L'\t' || ch == L'\n'
            || ch == L'\v' || ch == L'"';
    if (!quote)
        return std::wstring(value);

    std::wstring result(1, L'"');
    size_t slashes = 0;
    for (const wchar_t ch : value)
    {
        if (ch == L'\\')
        {
            ++slashes;
            continue;
        }
        if (ch == L'"')
            result.append(slashes * 2 + 1, L'\\');
        else
            result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(ch);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

#if defined(_WIN32)

ProcessResult run_process(const ProcessRequest& request)
{
    ProcessResult result;
    if (request.arguments.empty())
    {
        result.error = "No compiler command was supplied";
        return result;
    }

    SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0))
    {
        result.error = "Could not create compiler output pipe";
        return result;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring command;
    for (const auto& argument : request.arguments)
    {
        if (!command.empty())
            command.push_back(L' ');
        command += quote_windows_argument(argument.wstring());
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    PROCESS_INFORMATION process{};
    const std::wstring executable = request.arguments.front().wstring();
    // The working directory is passed as UTF-16, so the child may run in a
    // directory whose name its narrow argv could not represent.
    const std::wstring working_directory
        = request.working_directory.wstring();
    const BOOL created = CreateProcessW(executable.c_str(),
        mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr,
        working_directory.empty() ? nullptr : working_directory.c_str(),
        &startup, &process);
    CloseHandle(write_pipe);
    if (!created)
    {
        CloseHandle(read_pipe);
        result.error = "Could not start glslangValidator (error "
            + std::to_string(GetLastError()) + ")";
        return result;
    }
    CloseHandle(process.hThread);

    std::array<char, 4096> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + request.timeout;
    bool finished = false;
    while (!finished && std::chrono::steady_clock::now() < deadline)
    {
        DWORD available = 0;
        while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr,
                   &available, nullptr)
            && available > 0)
        {
            DWORD count = 0;
            const DWORD amount = std::min<DWORD>(
                available, static_cast<DWORD>(buffer.size()));
            if (!ReadFile(read_pipe, buffer.data(), amount,
                    &count, nullptr)
                || count == 0)
                break;
            append_output(result, buffer.data(), count);
            available -= count;
        }
        finished = WaitForSingleObject(process.hProcess, 10)
            == WAIT_OBJECT_0;
    }
    if (!finished)
    {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 2000);
        result.status = ProcessResult::Status::TimedOut;
    }
    DWORD count = 0;
    while (ReadFile(read_pipe, buffer.data(),
               static_cast<DWORD>(buffer.size()), &count, nullptr)
        && count > 0)
        append_output(result, buffer.data(), count);
    CloseHandle(read_pipe);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    result.exit_code = static_cast<int>(exit_code);
    if (finished)
        result.status = ProcessResult::Status::Exited;
    return result;
}

#else

ProcessResult run_process(const ProcessRequest& request)
{
    ProcessResult result;
    if (request.arguments.empty())
    {
        result.error = "No compiler command was supplied";
        return result;
    }

    int pipe_handles[2]{};
    if (pipe(pipe_handles) != 0)
    {
        result.error = "Could not create compiler output pipe";
        return result;
    }

    // Each path is one argv element; POSIX passes the bytes unchanged.
    std::vector<std::string> storage;
    storage.reserve(request.arguments.size());
    for (const auto& argument : request.arguments)
        storage.push_back(argument.string());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& argument : storage)
        argv.push_back(argument.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe_handles[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe_handles[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_handles[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_handles[1]);
    int action_result = 0;
    if (!request.working_directory.empty())
        action_result = add_working_directory(
            actions, request.working_directory);

    pid_t child = 0;
    const int spawn_result = action_result != 0
        ? action_result
        : posix_spawn(&child, storage.front().c_str(), &actions, nullptr,
              argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_handles[1]);
    if (spawn_result != 0)
    {
        close(pipe_handles[0]);
        result.error = "Could not start glslangValidator (error "
            + std::to_string(spawn_result) + ")";
        return result;
    }

    const int flags = fcntl(pipe_handles[0], F_GETFL, 0);
    fcntl(pipe_handles[0], F_SETFL, flags | O_NONBLOCK);
    std::array<char, 4096> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + request.timeout;
    int status = 0;
    bool finished = false;
    while (!finished && std::chrono::steady_clock::now() < deadline)
    {
        ssize_t count = 0;
        while ((count = read(
                    pipe_handles[0], buffer.data(), buffer.size()))
            > 0)
            append_output(result, buffer.data(), static_cast<size_t>(count));
        finished = waitpid(child, &status, WNOHANG) == child;
        if (!finished)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!finished)
    {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        result.status = ProcessResult::Status::TimedOut;
    }
    else
    {
        result.status = ProcessResult::Status::Exited;
    }
    ssize_t count = 0;
    while ((count = read(pipe_handles[0], buffer.data(), buffer.size())) > 0)
        append_output(result, buffer.data(), static_cast<size_t>(count));
    close(pipe_handles[0]);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

#endif

CompilerPathPolicy native_compiler_path_policy()
{
#if defined(_WIN32)
    return CompilerPathPolicy::ProjectRelative;
#else
    return CompilerPathPolicy::Absolute;
#endif
}

bool native_argument_encodable(const fs::path& path)
{
#if defined(_WIN32)
    // The bundled compiler's CRT builds its narrow argv from the command
    // line through the active code page, using best-fit or '?' for
    // characters it cannot map. Accept only spellings that round-trip.
    const std::wstring& wide = path.native();
    if (wide.empty() || GetACP() == CP_UTF8)
        return true;
    const int wide_size = static_cast<int>(wide.size());
    const int narrow_size = WideCharToMultiByte(CP_ACP, 0, wide.data(),
        wide_size, nullptr, 0, nullptr, nullptr);
    if (narrow_size <= 0)
        return false;
    std::string narrow(static_cast<size_t>(narrow_size), '\0');
    if (WideCharToMultiByte(CP_ACP, 0, wide.data(), wide_size, narrow.data(),
            narrow_size, nullptr, nullptr)
        != narrow_size)
        return false;
    const int round_trip_size = MultiByteToWideChar(
        CP_ACP, 0, narrow.data(), narrow_size, nullptr, 0);
    if (round_trip_size <= 0)
        return false;
    std::wstring round_trip(static_cast<size_t>(round_trip_size), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, narrow.data(), narrow_size,
            round_trip.data(), round_trip_size)
        != round_trip_size)
        return false;
    return round_trip == wide;
#else
    (void)path;
    return true;
#endif
}

CompilerInvocation plan_compiler_invocation(
    const ShaderCompileRequest& request, CompilerPathPolicy policy,
    const ArgumentEncodable& encodable)
{
    CompilerInvocation invocation;
    fs::path shader = request.shader;
    fs::path output = request.output_path;
    fs::path include = fs::path("-I").concat(request.project_path.native());
    if (policy == CompilerPathPolicy::ProjectRelative)
    {
        const fs::path project = request.project_path.lexically_normal();
        const fs::path normalized_shader = request.shader.lexically_normal();
        const fs::path normalized_output
            = request.output_path.lexically_normal();
        invocation.process.working_directory = request.project_path;
        include = "-I.";
        const auto shader_spelling = first_encodable(
            { normalized_shader.lexically_relative(project),
                normalized_shader },
            encodable);
        // The temporary output usually shares no international directory
        // with the project; when it does (for example both live under the
        // user profile) the relative spelling skips the shared part.
        const auto output_spelling = first_encodable(
            { normalized_output,
                normalized_output.lexically_relative(project) },
            encodable);
        if (!shader_spelling)
        {
            invocation.error = unencodable_message("Shader path",
                request.shader);
            invocation.error_path = request.shader;
            return invocation;
        }
        if (!output_spelling)
        {
            invocation.error = unencodable_message(
                "Shader output directory", request.output_path.parent_path())
                + "; set TMP/TEMP to a directory with a representable name";
            invocation.error_path = request.shader;
            return invocation;
        }
        shader = *shader_spelling;
        output = *output_spelling;
    }
    else if (encodable)
    {
        for (const fs::path* path : { &request.shader, &request.output_path,
                 &request.project_path })
        {
            if (encodable(*path))
                continue;
            invocation.error = unencodable_message("Path", *path);
            invocation.error_path = request.shader;
            return invocation;
        }
    }

    invocation.process.arguments = {
        request.compiler, "-V", "--target-env", "vulkan1.2", shader,
        "-o", output, "-l", "-g", include
    };
#if defined(__APPLE__)
    invocation.process.arguments.emplace_back(
        "-DREZONALITY_METAL_SEPARATE_MODEL_SAMPLER=1");
#endif
    return invocation;
}

void parse_compiler_diagnostics(std::string_view output,
    const fs::path& shader, const fs::path& working_directory,
    std::vector<DiagnosticEntry>& diagnostics)
{
    const size_t initial_count = diagnostics.size();
    std::istringstream lines{ std::string(output) };
    std::string line;
    static const std::regex path_line(
        R"((ERROR|WARNING):\s*(.*?):([0-9]+):\s*(.*))",
        std::regex::icase);
    static const std::regex generic_line(
        R"((.*?):([0-9]+):\s*(.*))", std::regex::icase);
    const auto line_number = [](const std::string& text) {
        try
        {
            return std::max(1, std::stoi(text));
        }
        catch (const std::exception&)
        {
            return 1;
        }
    };
    std::string fallback;
    while (std::getline(lines, line))
    {
        if (line.empty() || diagnostics.size() >= kMaximumBuildDiagnostics)
            continue;
        std::smatch match;
        if (std::regex_search(line, match, path_line))
        {
            std::string severity = trim(match[1].str());
            std::transform(severity.begin(), severity.end(), severity.begin(),
                [](unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
            const fs::path path
                = reported_path(trim(match[2].str()), working_directory);
            diagnostics.push_back({
                .path = path.empty() ? shader : path,
                .stage = "compile",
                .severity = std::move(severity),
                .line = line_number(match[3].str()),
                .message = trim(match[4].str()),
            });
            continue;
        }
        if (std::regex_search(line, match, generic_line))
        {
            const fs::path path
                = reported_path(trim(match[1].str()), working_directory);
            diagnostics.push_back({
                .path = path.empty() ? shader : path,
                .stage = "compile",
                .severity = "error",
                .line = line_number(match[2].str()),
                .message = trim(match[3].str()),
            });
            continue;
        }
        if (fallback.empty()
            && (line.find("ERROR") != std::string::npos
                || line.find("error") != std::string::npos
                || line.find("Error") != std::string::npos))
        {
            fallback = trim(line);
        }
    }
    if (diagnostics.size() == initial_count
        && diagnostics.size() < kMaximumBuildDiagnostics)
    {
        if (fallback.empty())
            fallback = trim(std::string(output));
        if (fallback.size() > 300)
            fallback.resize(300);
        if (fallback.empty())
            fallback = "glslangValidator failed without diagnostics";
        push_diagnostic(diagnostics, shader, std::move(fallback));
    }
}

bool compile_shader(const ShaderCompileRequest& request,
    const CompilerEnvironment& environment, std::vector<uint32_t>& spirv,
    std::vector<DiagnosticEntry>& diagnostics)
{
    spirv.clear();
    const CompilerInvocation invocation = plan_compiler_invocation(
        request, environment.policy, environment.encodable);
    if (!invocation.error.empty())
    {
        push_diagnostic(diagnostics, invocation.error_path, invocation.error);
        return false;
    }
    const ProcessResult process = environment.run
        ? environment.run(invocation.process)
        : run_process(invocation.process);
    switch (process.status)
    {
    case ProcessResult::Status::StartFailed:
        push_diagnostic(diagnostics, request.shader,
            process.error.empty() ? "Could not start glslangValidator"
                                  : process.error);
        return false;
    case ProcessResult::Status::TimedOut:
        push_diagnostic(diagnostics, request.shader,
            "glslangValidator timed out after "
                + std::to_string(std::chrono::duration_cast<
                    std::chrono::seconds>(invocation.process.timeout)
                                     .count())
                + "s compiling " + display_path_utf8(request.shader.filename()));
        return false;
    case ProcessResult::Status::Exited:
        break;
    }
    if (process.exit_code != 0)
    {
        parse_compiler_diagnostics(process.output, request.shader,
            invocation.process.working_directory, diagnostics);
        return false;
    }
    spirv = read_spirv(request.output_path);
    if (!spirv.empty())
        return true;
    // glslangValidator exits 0 when it cannot write its output file; keep
    // its explanation with the generic message.
    std::string message = "glslangValidator produced no SPIR-V for "
        + display_path_utf8(request.shader.filename());
    std::istringstream lines(process.output);
    std::string line;
    while (std::getline(lines, line))
    {
        if (line.find("ERROR") == std::string::npos)
            continue;
        line = trim(line);
        if (line.size() > 300)
            line.resize(300);
        message += ": " + line;
        break;
    }
    push_diagnostic(diagnostics, request.shader, std::move(message));
    return false;
}

fs::path bundled_compiler_path(const fs::path& plugin_directory)
{
#if defined(_WIN32)
    return plugin_directory / "tools" / "win" / "glslangValidator.exe";
#elif defined(__APPLE__)
    return plugin_directory / "tools" / "mac" / "glslangValidator";
#else
    return plugin_directory / "tools" / "linux" / "glslangValidator";
#endif
}

} // namespace rezonality
