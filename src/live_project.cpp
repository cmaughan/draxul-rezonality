#include "live_project.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

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

using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

struct ProcessResult
{
    int exit_code = -1;
    std::string output;
    std::string error;
};

uint64_t hash_bytes(uint64_t hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
    return hash;
}

uint64_t hash_string(uint64_t hash, std::string_view value)
{
    return hash_bytes(hash, value.data(), value.size());
}

std::optional<std::string> read_text(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    std::ostringstream stream;
    stream << input.rdbuf();
    if (!input.good() && !input.eof())
        return std::nullopt;
    return stream.str();
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

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

#if defined(_WIN32)

std::wstring quote_windows_argument(std::wstring_view value)
{
    bool quote = value.empty();
    for (const wchar_t ch : value)
        quote = quote || ch == L' ' || ch == L'\t' || ch == L'"';
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

ProcessResult run_process(const std::vector<fs::path>& arguments)
{
    ProcessResult result;
    if (arguments.empty())
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
    for (const auto& argument : arguments)
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
    const std::wstring executable = arguments.front().wstring();
    const BOOL created = CreateProcessW(executable.c_str(),
        mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
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
    const auto deadline
        = std::chrono::steady_clock::now() + std::chrono::seconds(10);
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
            result.output.append(buffer.data(), count);
            available -= count;
        }
        finished = WaitForSingleObject(process.hProcess, 10)
            == WAIT_OBJECT_0;
    }
    if (!finished)
    {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 2000);
        result.error = "glslangValidator timed out";
    }
    DWORD count = 0;
    while (ReadFile(read_pipe, buffer.data(),
        static_cast<DWORD>(buffer.size()), &count, nullptr)
        && count > 0)
        result.output.append(buffer.data(), count);
    CloseHandle(read_pipe);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    result.exit_code = static_cast<int>(exit_code);
    return result;
}

#else

ProcessResult run_process(const std::vector<fs::path>& arguments)
{
    ProcessResult result;
    if (arguments.empty())
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

    std::vector<std::string> storage;
    storage.reserve(arguments.size());
    for (const auto& argument : arguments)
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

    pid_t child = 0;
    const int spawn_result = posix_spawn(&child, storage.front().c_str(),
        &actions, nullptr, argv.data(), environ);
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
    const auto deadline
        = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    int status = 0;
    bool finished = false;
    while (!finished && std::chrono::steady_clock::now() < deadline)
    {
        ssize_t count = 0;
        while ((count = read(
                    pipe_handles[0], buffer.data(), buffer.size()))
            > 0)
            result.output.append(buffer.data(), static_cast<size_t>(count));
        finished = waitpid(child, &status, WNOHANG) == child;
        if (!finished)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!finished)
    {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        result.error = "glslangValidator timed out";
    }
    ssize_t count = 0;
    while ((count = read(pipe_handles[0], buffer.data(), buffer.size())) > 0)
        result.output.append(buffer.data(), static_cast<size_t>(count));
    close(pipe_handles[0]);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

#endif

struct SceneDescription
{
    fs::path scenegraph;
    fs::path vertex;
    fs::path fragment;
};

std::optional<std::string> first_match(const std::string& source,
    const std::regex& expression)
{
    std::smatch match;
    if (!std::regex_search(source, match, expression) || match.size() < 2)
        return std::nullopt;
    return match[1].str();
}

std::optional<SceneDescription> load_scene(const ProjectOptions& options,
    fs::path& diagnostic_path, int& diagnostic_line, std::string& error)
{
    fs::path scenegraph = options.scenegraph;
    if (scenegraph.is_relative())
        scenegraph = options.project_path / scenegraph;
    diagnostic_path = scenegraph;
    const auto source = read_text(scenegraph);
    if (!source)
    {
        error = "Scenegraph is missing: " + scenegraph.string();
        return std::nullopt;
    }

    // This is the single-pass subset of VkLive's existing scene grammar. The
    // complete MPC grammar is ported in the following multipass slice; keeping
    // the spelling here identical lets real projects move across unchanged.
    static const std::regex vertex_expression(
        R"(\bvs\s*:\s*([a-zA-Z_\-][a-zA-Z0-9_\-\/.]*))");
    static const std::regex fragment_expression(
        R"(\bfs\s*:\s*([a-zA-Z_\-][a-zA-Z0-9_\-\/.]*))");
    const auto vertex = first_match(*source, vertex_expression);
    const auto fragment = first_match(*source, fragment_expression);
    if (!vertex || !fragment)
    {
        error = "Single-pass scenegraph must declare both vs: and fs:";
        diagnostic_line = 1;
        return std::nullopt;
    }

    SceneDescription description;
    description.scenegraph = scenegraph;
    description.vertex = options.project_path / fs::u8path(*vertex);
    description.fragment = options.project_path / fs::u8path(*fragment);
    return description;
}

void parse_compiler_diagnostic(const std::string& output,
    const fs::path& shader, fs::path& diagnostic_path,
    int& diagnostic_line, std::string& message)
{
    diagnostic_path = shader;
    diagnostic_line = -1;
    std::istringstream lines(output);
    std::string line;
    static const std::regex path_line(
        R"((?:ERROR|WARNING):\s*(.*?):([0-9]+):\s*(.*))",
        std::regex::icase);
    static const std::regex generic_line(
        R"((.*?):([0-9]+):\s*(.*))", std::regex::icase);
    while (std::getline(lines, line))
    {
        if (line.empty())
            continue;
        std::smatch match;
        if (std::regex_search(line, match, path_line)
            || std::regex_search(line, match, generic_line))
        {
            diagnostic_path = fs::u8path(trim(match[1].str()));
            diagnostic_line = std::max(1, std::stoi(match[2].str()));
            message = trim(match[3].str());
            return;
        }
        if (message.empty()
            && (line.find("ERROR") != std::string::npos
                || line.find("error") != std::string::npos))
        {
            message = trim(line);
        }
    }
    if (message.empty())
        message = trim(output);
    if (message.size() > 300)
        message.resize(300);
}

bool compile_shader(const fs::path& compiler, const fs::path& project_path,
    const fs::path& shader, const fs::path& output_path,
    std::vector<uint32_t>& spirv, fs::path& diagnostic_path,
    int& diagnostic_line, std::string& error)
{
    std::vector<fs::path> arguments{
        compiler, "-V", shader, "-o", output_path, "-l", "-g",
        fs::path("-I" + project_path.string())
    };
    ProcessResult process = run_process(arguments);
    if (!process.error.empty())
    {
        diagnostic_path = shader;
        error = process.error;
        return false;
    }
    if (process.exit_code != 0)
    {
        parse_compiler_diagnostic(process.output, shader,
            diagnostic_path, diagnostic_line, error);
        if (error.empty())
            error = "glslangValidator rejected " + shader.filename().string();
        return false;
    }
    spirv = read_spirv(output_path);
    if (spirv.empty())
    {
        diagnostic_path = shader;
        error = "glslangValidator produced no SPIR-V for "
            + shader.filename().string();
        return false;
    }
    return true;
}

fs::path compiler_path(const fs::path& plugin_directory)
{
#if defined(_WIN32)
    return plugin_directory / "tools" / "win" / "glslangValidator.exe";
#elif defined(__APPLE__)
    return plugin_directory / "tools" / "mac" / "glslangValidator";
#else
    return plugin_directory / "tools" / "linux" / "glslangValidator";
#endif
}

} // namespace

std::optional<ProjectOptions> parse_project_options(
    const fs::path& plugin_directory, const char* config_json,
    size_t config_json_length, std::string& error)
{
    ProjectOptions options;
    options.project_path = plugin_directory / "examples" / "simple";
    bool scenegraph_explicit = false;
    try
    {
        const auto config = nlohmann::json::parse(
            config_json ? std::string_view(config_json, config_json_length)
                        : std::string_view("{}"));
        if (!config.is_object())
        {
            error = "Rezonality configuration must be a JSON object";
            return std::nullopt;
        }
        if (const auto project = config.find("project_path");
            project != config.end())
        {
            if (!project->is_string())
                throw std::runtime_error("project_path must be a string");
            options.project_path = fs::u8path(project->get<std::string>());
        }
        if (const auto scenegraph = config.find("scenegraph");
            scenegraph != config.end())
        {
            if (!scenegraph->is_string())
                throw std::runtime_error("scenegraph must be a string");
            options.scenegraph = fs::u8path(scenegraph->get<std::string>());
            scenegraph_explicit = true;
        }
        options.auto_reload = config.value("auto_reload", true);
        options.compile_debounce_ms = std::clamp(
            config.value("compile_debounce_ms", 150u), 25u, 5000u);
    }
    catch (const std::exception& exception)
    {
        error = std::string("Invalid Rezonality configuration: ")
            + exception.what();
        return std::nullopt;
    }
    if (options.project_path.is_relative())
        options.project_path = plugin_directory / options.project_path;
    std::error_code ec;
    options.project_path = fs::weakly_canonical(options.project_path, ec);
    if (ec || !fs::is_directory(options.project_path))
    {
        error = "Rezonality project directory is missing: "
            + options.project_path.string();
        return std::nullopt;
    }
    if (!scenegraph_explicit)
    {
        const auto project = read_text(options.project_path / "project.toml");
        if (project)
        {
            static const std::regex scenegraph_expression(
                R"regex(scenegraph\s*=\s*"([^"]+)")regex");
            if (const auto configured
                = first_match(*project, scenegraph_expression))
                options.scenegraph = fs::u8path(*configured);
        }
    }
    return options;
}

LiveProject::LiveProject(fs::path plugin_directory,
    ProjectOptions options, WakeCallback wake)
    : plugin_directory_(std::move(plugin_directory))
    , options_(std::move(options))
    , wake_(std::move(wake))
{
}

LiveProject::~LiveProject()
{
    stop();
}

void LiveProject::start()
{
    if (!worker_.joinable())
        worker_ = std::jthread([this](std::stop_token token) { run(token); });
}

void LiveProject::stop()
{
    if (!worker_.joinable())
        return;
    worker_.request_stop();
    wake_condition_.notify_all();
    worker_.join();
}

void LiveProject::force_reload()
{
    {
        std::lock_guard lock(mutex_);
        force_requested_ = true;
    }
    wake_condition_.notify_all();
}

std::optional<BuildResult> LiveProject::take_result()
{
    std::lock_guard lock(mutex_);
    auto result = std::move(result_);
    result_.reset();
    return result;
}

uint64_t LiveProject::project_fingerprint() const
{
    uint64_t hash = kFnvOffset;
    std::error_code ec;
    fs::recursive_directory_iterator iterator(options_.project_path,
        fs::directory_options::skip_permission_denied, ec);
    std::vector<fs::path> files;
    for (const auto& entry : iterator)
    {
        if (entry.is_regular_file(ec))
        {
            const auto extension = entry.path().extension().string();
            if (extension == ".toml" || extension == ".scenegraph"
                || extension == ".vert" || extension == ".frag"
                || extension == ".glsl" || extension == ".h")
                files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files)
    {
        hash = hash_string(hash, file.generic_string());
        if (const auto contents = read_text(file))
            hash = hash_string(hash, *contents);
    }
    return hash;
}

BuildResult LiveProject::build(uint64_t generation) const
{
    BuildResult result;
    result.generation = generation;
    int line = -1;
    fs::path diagnostic;
    std::string error;
    const auto scene = load_scene(options_, diagnostic, line, error);
    if (!scene)
    {
        result.diagnostic_path = std::move(diagnostic);
        result.diagnostic_line = line;
        result.error = std::move(error);
        return result;
    }

    const fs::path compiler = compiler_path(plugin_directory_);
    if (!fs::is_regular_file(compiler))
    {
        result.diagnostic_path = compiler;
        result.error = "Bundled glslangValidator is missing";
        return result;
    }

    const fs::path output_directory = fs::temp_directory_path()
        / "draxul-rezonality" / std::to_string(
            reinterpret_cast<uintptr_t>(this));
    std::error_code ec;
    fs::create_directories(output_directory, ec);
    if (ec)
    {
        result.diagnostic_path = output_directory;
        result.error = "Could not create shader output directory";
        return result;
    }

    ShaderBuild candidate;
    candidate.generation = generation;
    candidate.project_path = options_.project_path;
    candidate.scenegraph_path = scene->scenegraph;
    candidate.vertex_path = scene->vertex;
    candidate.fragment_path = scene->fragment;
    const fs::path vertex_output = output_directory
        / ("vertex-" + std::to_string(generation) + ".spv");
    const fs::path fragment_output = output_directory
        / ("fragment-" + std::to_string(generation) + ".spv");
    if (!compile_shader(compiler, options_.project_path,
            candidate.vertex_path, vertex_output, candidate.vertex_spirv,
            result.diagnostic_path, result.diagnostic_line, result.error)
        || !compile_shader(compiler, options_.project_path,
            candidate.fragment_path, fragment_output,
            candidate.fragment_spirv, result.diagnostic_path,
            result.diagnostic_line, result.error))
    {
        fs::remove(vertex_output, ec);
        fs::remove(fragment_output, ec);
        return result;
    }
    fs::remove(vertex_output, ec);
    fs::remove(fragment_output, ec);
    result.build = std::move(candidate);
    return result;
}

void LiveProject::run(std::stop_token stop_token)
{
    uint64_t generation = 0;
    uint64_t observed_fingerprint = project_fingerprint();
    bool dirty = true;
    auto dirty_since = std::chrono::steady_clock::now()
        - std::chrono::milliseconds(options_.compile_debounce_ms);

    while (!stop_token.stop_requested())
    {
        bool forced = false;
        {
            std::unique_lock lock(mutex_);
            forced = force_requested_;
            force_requested_ = false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (forced || (dirty && now - dirty_since
                >= std::chrono::milliseconds(options_.compile_debounce_ms)))
        {
            BuildResult next = build(++generation);
            {
                std::lock_guard lock(mutex_);
                result_ = std::move(next);
            }
            if (wake_ && !stop_token.stop_requested())
                wake_();
            dirty = false;
        }

        std::unique_lock lock(mutex_);
        wake_condition_.wait_for(lock, stop_token, 100ms, [&] {
            return force_requested_;
        });
        lock.unlock();
        if (stop_token.stop_requested())
            break;
        if (options_.auto_reload)
        {
            const uint64_t fingerprint = project_fingerprint();
            if (fingerprint != observed_fingerprint)
            {
                observed_fingerprint = fingerprint;
                dirty = true;
                dirty_since = std::chrono::steady_clock::now();
            }
        }
    }
}

} // namespace rezonality
