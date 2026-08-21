#include "live_project.h"
#include "image_loader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <map>
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

constexpr size_t kMaximumBuildDiagnostics = 128;

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
    std::vector<ShaderBuild::Surface> surfaces;
    std::vector<ModelData> models;
    std::vector<ShaderBuild::Pass> passes;
};

std::optional<std::string> first_match(const std::string& source,
    const std::regex& expression)
{
    std::smatch match;
    if (!std::regex_search(source, match, expression) || match.size() < 2)
        return std::nullopt;
    return match[1].str();
}

std::string without_comments(std::string source)
{
    static const std::regex comments(R"(//[^\r\n]*)");
    return std::regex_replace(source, comments, "");
}

size_t matching_brace(std::string_view source, size_t open)
{
    size_t depth = 0;
    for (size_t index = open; index < source.size(); ++index)
    {
        if (source[index] == '{')
            ++depth;
        else if (source[index] == '}' && --depth == 0)
            return index;
    }
    return std::string_view::npos;
}

std::vector<std::string> parse_list(
    const std::string& body, std::string_view key)
{
    const std::regex expression("\\b" + std::string(key)
        + R"(\s*:\s*\(([^)]*)\))");
    const auto matched = first_match(body, expression);
    if (!matched)
        return {};
    std::vector<std::string> values;
    std::istringstream stream(*matched);
    std::string value;
    while (std::getline(stream, value, ','))
    {
        value = trim(value);
        if (!value.empty())
            values.push_back(std::move(value));
    }
    return values;
}

void parse_vec4(const std::string& body, std::string_view key,
    float (&value)[4], bool* found = nullptr)
{
    const std::regex expression("\\b" + std::string(key)
        + R"(\s*:\s*\(\s*([-+.0-9]+)\s*,\s*([-+.0-9]+)\s*,\s*([-+.0-9]+)\s*,\s*([-+.0-9]+)\s*\))");
    std::smatch match;
    if (!std::regex_search(body, match, expression))
    {
        if (found)
            *found = false;
        return;
    }
    for (size_t index = 0; index < 4; ++index)
        value[index] = std::stof(match[index + 1].str());
    if (found)
        *found = true;
}

bool parse_vec3(const std::string& body, std::string_view key,
    glm::vec3& value)
{
    const std::regex expression("\\b" + std::string(key)
        + R"(\s*:\s*\(\s*([-+.0-9]+)\s*,\s*([-+.0-9]+)\s*,\s*([-+.0-9]+)\s*\))");
    std::smatch match;
    if (!std::regex_search(body, match, expression))
        return false;
    value = { std::stof(match[1].str()), std::stof(match[2].str()),
        std::stof(match[3].str()) };
    return true;
}

bool parse_vec2(const std::string& body, std::string_view key,
    glm::vec2& value)
{
    const std::regex expression("\\b" + std::string(key)
        + R"(\s*:\s*\(\s*([-+.0-9]+)\s*,\s*([-+.0-9]+)\s*\))");
    std::smatch match;
    if (!std::regex_search(body, match, expression))
        return false;
    value = { std::stof(match[1].str()), std::stof(match[2].str()) };
    return true;
}

bool parse_scalar(const std::string& body, std::string_view key,
    float& value)
{
    const std::regex expression("\\b" + std::string(key)
        + R"(\s*:\s*([-+.0-9]+))");
    const auto matched = first_match(body, expression);
    if (!matched)
        return false;
    value = std::stof(*matched);
    return true;
}

bool parse_uv_origin(const std::string& body, std::string_view owner,
    bool& flip_texture_y, std::string& error)
{
    flip_texture_y = true;
    const auto origin = first_match(body,
        std::regex(R"(\buv_origin\s*:\s*([A-Za-z_]+))"));
    if (!origin || *origin == "lower_left")
        return true;
    if (*origin == "upper_left")
    {
        flip_texture_y = false;
        return true;
    }
    error = std::string(owner) + " has unknown uv_origin '" + *origin + "'";
    return false;
}

std::vector<std::pair<std::string, std::string>> named_blocks(
    const std::string& source, std::string_view kind)
{
    std::vector<std::pair<std::string, std::string>> blocks;
    const std::regex header("(^|[\\r\\n])\\s*" + std::string(kind)
        + R"(\s*:\s*([A-Za-z_][A-Za-z0-9_.-]*)\s*\{)");
    for (auto begin = std::sregex_iterator(source.begin(), source.end(), header);
         begin != std::sregex_iterator(); ++begin)
    {
        const auto& match = *begin;
        const size_t open = static_cast<size_t>(match.position() + match.length() - 1);
        const size_t close = matching_brace(source, open);
        if (close == std::string::npos)
            continue;
        blocks.emplace_back(match[2].str(),
            source.substr(open + 1, close - open - 1));
    }
    return blocks;
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

    const std::string parsed = without_comments(*source);
    static const std::regex vertex_expression(
        R"(\bvs\s*:\s*([a-zA-Z_\-][a-zA-Z0-9_\-\/.]*))");
    static const std::regex fragment_expression(
        R"(\bfs\s*:\s*([a-zA-Z_\-][a-zA-Z0-9_\-\/.]*))");
    static const std::regex raygen_expression(
        R"(\bray_gen\s*:\s*([a-zA-Z_\-][a-zA-Z0-9_\-\/.]*))");
    static const std::regex miss_expression(
        R"(\bmiss\s*:\s*([a-zA-Z_\-][a-zA-Z0-9_\-\/.]*))");
    static const std::regex closest_hit_expression(
        R"(\bclosest_hit\s*:\s*([a-zA-Z_\-][a-zA-Z0-9_\-\/.]*))");
    static const std::regex metal_ray_expression(
        R"(\bmetal_ray\s*:\s*([a-zA-Z_\-][a-zA-Z0-9_\-\/.]*))");

    SceneDescription description;
    description.scenegraph = scenegraph;
    std::map<std::string, size_t> model_indices;
    std::map<std::string, Camera> cameras;
    Camera default_camera;
    camera_set_pos_lookat(default_camera, default_camera.position,
        default_camera.focal_point);
    cameras.emplace(default_camera.name, default_camera);

    for (const auto& [name, body] : named_blocks(parsed, "camera"))
    {
        Camera camera;
        camera.name = name;
        glm::vec3 position = camera.position;
        glm::vec3 look_at = camera.focal_point;
        parse_vec3(body, "position", position);
        parse_vec3(body, "look_at", look_at);
        parse_vec2(body, "near_far", camera.near_far);
        parse_scalar(body, "field_of_view", camera.field_of_view);
        camera_set_pos_lookat(camera, position, look_at);
        cameras[name] = camera;
    }

    for (const auto& [name, body] : named_blocks(parsed, "model"))
    {
        const auto path = first_match(body,
            std::regex(R"(\bpath\s*:\s*([A-Za-z0-9_\-\/.]+))"));
        if (!path)
        {
            error = "Model '" + name + "' is missing path:";
            diagnostic_line = 1;
            return std::nullopt;
        }
        glm::vec3 scale{ 1.0f };
        parse_vec3(body, "scale", scale);
        bool flip_texture_y = true;
        if (!parse_uv_origin(body, "Model '" + name + "'",
                flip_texture_y, error))
        {
            diagnostic_line = 1;
            return std::nullopt;
        }
        ModelData model;
        const fs::path model_path
            = options.project_path / fs::u8path(*path);
        if (!load_model(model_path, scale, flip_texture_y, model, error))
        {
            diagnostic_path = model_path;
            diagnostic_line = 1;
            return std::nullopt;
        }
        model_indices[name] = description.models.size();
        description.models.push_back(std::move(model));
    }
    for (const auto& [name, body] : named_blocks(parsed, "surface"))
    {
        ShaderBuild::Surface surface;
        surface.name = name;
        if (name == "AudioAnalysis")
        {
            surface.audio_analysis = true;
            surface.format = ShaderBuild::SurfaceFormat::Color32Float;
            surface.image_width = AudioTextureFrame::width;
            surface.image_height = AudioTextureFrame::height;
            surface.image_float_pixels.resize(
                AudioTextureFrame::width * AudioTextureFrame::height * 4,
                0.0f);
        }
        if (const auto path = first_match(body,
                std::regex(R"(\bpath\s*:\s*([A-Za-z0-9_\-\/.]+))")))
            surface.path = options.project_path / fs::u8path(*path);
        if (const auto format = first_match(body,
                std::regex(R"(\bformat\s*:\s*([A-Za-z0-9_]+))")))
        {
            if (*format == "rgba16f")
                surface.format = ShaderBuild::SurfaceFormat::Color16Float;
            else if (*format == "rgba32f")
                surface.format = ShaderBuild::SurfaceFormat::Color32Float;
            else if (format->find("depth") != std::string::npos)
                surface.format = ShaderBuild::SurfaceFormat::Depth32;
        }
        const std::regex scale_expression(
            R"(\bscale\s*:\s*\(\s*([-+.0-9]+)\s*,\s*([-+.0-9]+))");
        std::smatch scale;
        if (std::regex_search(body, scale, scale_expression))
        {
            surface.scale_x = std::stof(scale[1].str());
            surface.scale_y = std::stof(scale[2].str());
        }
        parse_vec4(body, "clear", surface.clear);
        description.surfaces.push_back(std::move(surface));
    }
    for (const auto& [name, body] : named_blocks(parsed, "environment"))
    {
        ShaderBuild::Surface surface;
        surface.name = name;
        if (const auto path = first_match(body,
                std::regex(R"(\bpath\s*:\s*([A-Za-z0-9_\-\/.]+))")))
            surface.path = options.project_path / fs::u8path(*path);
        if (const auto format = first_match(body,
                std::regex(R"(\bformat\s*:\s*([A-Za-z0-9_]+))"));
            format && *format == "rgba32f")
            surface.format = ShaderBuild::SurfaceFormat::Color32Float;
        description.surfaces.push_back(std::move(surface));
    }

    for (const auto& [name, body] : named_blocks(parsed, "pass"))
    {
        const auto vertex = first_match(body, vertex_expression);
        const auto fragment = first_match(body, fragment_expression);
        ShaderBuild::Pass pass;
        pass.name = name;
        pass.targets = parse_list(body, "targets");
        if (pass.targets.empty())
            pass.targets.push_back("default_color");
        for (std::string sampler : parse_list(body, "samplers"))
        {
            ShaderBuild::Sampler parsed_sampler;
            if (!sampler.empty() && sampler.front() == '!')
            {
                parsed_sampler.previous_frame = true;
                sampler.erase(sampler.begin());
            }
            parsed_sampler.surface = std::move(sampler);
            pass.samplers.push_back(std::move(parsed_sampler));
        }
        const auto geometries = named_blocks(body, "geometry");
        if (geometries.empty())
        {
            error = "Pass '" + name + "' must declare geometry";
            diagnostic_line = 1;
            return std::nullopt;
        }
        const std::string& geometry_body = geometries.front().second;
        const auto raygen = first_match(geometry_body, raygen_expression);
        const auto miss = first_match(geometry_body, miss_expression);
        const auto closest_hit = first_match(
            geometry_body, closest_hit_expression);
        const auto metal_ray = first_match(
            geometry_body, metal_ray_expression);
        pass.ray_trace = raygen || miss || closest_hit || metal_ray;
        if (pass.ray_trace)
        {
            if (!raygen || !miss || !closest_hit || !metal_ray)
            {
                error = "Ray pass '" + name
                    + "' must declare ray_gen, miss, closest_hit, and metal_ray";
                diagnostic_line = 1;
                return std::nullopt;
            }
            pass.raygen_path = options.project_path / fs::u8path(*raygen);
            pass.miss_path = options.project_path / fs::u8path(*miss);
            pass.closest_hit_path
                = options.project_path / fs::u8path(*closest_hit);
            pass.metal_ray_path
                = options.project_path / fs::u8path(*metal_ray);
        }
        else
        {
            if (!vertex || !fragment)
            {
                error = "Pass '" + name + "' must declare both vs: and fs:";
                diagnostic_line = 1;
                return std::nullopt;
            }
            pass.vertex_path = options.project_path / fs::u8path(*vertex);
            pass.fragment_path = options.project_path / fs::u8path(*fragment);
        }
        const auto model_name = first_match(geometry_body,
            std::regex(R"(\bmodel\s*:\s*([A-Za-z_][A-Za-z0-9_.-]*))"));
        const auto geometry_path = first_match(geometry_body,
            std::regex(R"(\bpath\s*:\s*([A-Za-z0-9_\-\/.]+))"));
        if (model_name)
        {
            const auto found = model_indices.find(*model_name);
            if (found == model_indices.end())
            {
                error = "Pass '" + name + "' references unknown model '"
                    + *model_name + "'";
                diagnostic_line = 1;
                return std::nullopt;
            }
            pass.model_index = found->second;
        }
        else if (geometry_path && *geometry_path != "screen_rect")
        {
            glm::vec3 scale{ 1.0f };
            parse_vec3(geometry_body, "scale", scale);
            bool flip_texture_y = true;
            if (!parse_uv_origin(geometry_body,
                    "Geometry in pass '" + name + "'",
                    flip_texture_y, error))
            {
                diagnostic_line = 1;
                return std::nullopt;
            }
            ModelData model;
            const fs::path model_path
                = options.project_path / fs::u8path(*geometry_path);
            if (!load_model(model_path, scale, flip_texture_y, model, error))
            {
                diagnostic_path = model_path;
                diagnostic_line = 1;
                return std::nullopt;
            }
            pass.model_index = description.models.size();
            description.models.push_back(std::move(model));
        }
        if (pass.ray_trace && !pass.model_index)
        {
            error = "Ray pass '" + name + "' must reference model geometry";
            diagnostic_line = 1;
            return std::nullopt;
        }
        const auto camera_name = first_match(body,
            std::regex(R"(\bcamera\s*:\s*([A-Za-z_][A-Za-z0-9_.-]*))"));
        const auto selected_camera = cameras.find(
            camera_name.value_or("default_camera"));
        if (selected_camera == cameras.end())
        {
            error = "Pass '" + name + "' references unknown camera '"
                + *camera_name + "'";
            diagnostic_line = 1;
            return std::nullopt;
        }
        pass.camera = selected_camera->second;
        parse_vec4(body, "clear", pass.clear, &pass.has_clear);
        description.passes.push_back(std::move(pass));
    }
    if (description.passes.empty())
    {
        error = "Scenegraph must declare at least one enabled pass";
        diagnostic_line = 1;
        return std::nullopt;
    }
    for (auto& pass : description.passes)
        for (const auto& target : pass.targets)
            if (target != "default_color" && target != "default_depth")
                for (auto& surface : description.surfaces)
                    surface.target = surface.target || surface.name == target;
    return description;
}

void parse_compiler_diagnostics(const std::string& output,
    const fs::path& shader, std::vector<DiagnosticEntry>& diagnostics)
{
    const size_t initial_count = diagnostics.size();
    std::istringstream lines(output);
    std::string line;
    static const std::regex path_line(
        R"((ERROR|WARNING):\s*(.*?):([0-9]+):\s*(.*))",
        std::regex::icase);
    static const std::regex generic_line(
        R"((.*?):([0-9]+):\s*(.*))", std::regex::icase);
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
            diagnostics.push_back({
                .path = fs::u8path(trim(match[2].str())),
                .stage = "compile",
                .severity = std::move(severity),
                .line = std::max(1, std::stoi(match[3].str())),
                .message = trim(match[4].str()),
            });
            continue;
        }
        if (std::regex_search(line, match, generic_line))
        {
            diagnostics.push_back({
                .path = fs::u8path(trim(match[1].str())),
                .stage = "compile",
                .severity = "error",
                .line = std::max(1, std::stoi(match[2].str())),
                .message = trim(match[3].str()),
            });
            continue;
        }
        if (fallback.empty()
            && (line.find("ERROR") != std::string::npos
                || line.find("error") != std::string::npos))
        {
            fallback = trim(line);
        }
    }
    if (diagnostics.size() == initial_count
        && diagnostics.size() < kMaximumBuildDiagnostics)
    {
        if (fallback.empty())
            fallback = trim(output);
        if (fallback.size() > 300)
            fallback.resize(300);
        diagnostics.push_back({
            .path = shader,
            .stage = "compile",
            .severity = "error",
            .message = std::move(fallback),
        });
    }
}

bool compile_shader(const fs::path& compiler, const fs::path& project_path,
    const fs::path& shader, const fs::path& output_path,
    std::vector<uint32_t>& spirv, std::vector<DiagnosticEntry>& diagnostics)
{
    std::vector<fs::path> arguments{
        compiler, "-V", "--target-env", "vulkan1.2", shader,
        "-o", output_path, "-l", "-g",
        fs::path("-I" + project_path.string())
    };
#if defined(__APPLE__)
    arguments.emplace_back("-DREZONALITY_METAL_SEPARATE_MODEL_SAMPLER=1");
#endif
    ProcessResult process = run_process(arguments);
    if (!process.error.empty())
    {
        if (diagnostics.size() < kMaximumBuildDiagnostics)
        {
            diagnostics.push_back({
                .path = shader,
                .stage = "compile",
                .severity = "error",
                .message = process.error,
            });
        }
        return false;
    }
    if (process.exit_code != 0)
    {
        parse_compiler_diagnostics(process.output, shader, diagnostics);
        return false;
    }
    spirv = read_spirv(output_path);
    if (spirv.empty())
    {
        if (diagnostics.size() < kMaximumBuildDiagnostics)
        {
            diagnostics.push_back({
                .path = shader,
                .stage = "compile",
                .severity = "error",
                .message = "glslangValidator produced no SPIR-V for "
                    + shader.filename().string(),
            });
        }
        return false;
    }
    return true;
}

void finalize_compile_diagnostics(BuildResult& result)
{
    std::vector<DiagnosticEntry> unique;
    unique.reserve(result.diagnostics.size());
    for (auto& diagnostic : result.diagnostics)
    {
        const bool duplicate = std::any_of(unique.begin(), unique.end(),
            [&diagnostic](const DiagnosticEntry& existing) {
                return existing.path == diagnostic.path
                    && existing.line == diagnostic.line
                    && existing.column == diagnostic.column
                    && existing.severity == diagnostic.severity
                    && existing.message == diagnostic.message;
            });
        if (!duplicate)
            unique.push_back(std::move(diagnostic));
    }
    result.diagnostics = std::move(unique);
    if (result.diagnostics.empty())
    {
        result.error = "Shader compilation failed";
        return;
    }

    const auto primary = std::find_if(result.diagnostics.begin(),
        result.diagnostics.end(), [](const DiagnosticEntry& diagnostic) {
            return diagnostic.severity == "error";
        });
    const auto& selected = primary != result.diagnostics.end()
        ? *primary
        : result.diagnostics.front();
    result.diagnostic_path = selected.path;
    result.diagnostic_line = selected.line;
    result.error = selected.message;
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
        options.paused = config.value("paused", false);
        options.compile_debounce_ms = std::clamp(
            config.value("compile_debounce_ms", 150u), 25u, 5000u);
        if (const auto diagnostics = config.find("diagnostics_id");
            diagnostics != config.end())
        {
            if (!diagnostics->is_string())
                throw std::runtime_error("diagnostics_id must be a string");
            options.diagnostics_id = diagnostics->get<std::string>();
            if (options.diagnostics_id.empty()
                || options.diagnostics_id.size() > 64
                || !std::all_of(options.diagnostics_id.begin(),
                    options.diagnostics_id.end(), [](unsigned char value) {
                        return (value >= 'a' && value <= 'z')
                            || (value >= '0' && value <= '9')
                            || value == '.' || value == '_'
                            || value == '-';
                    }))
            {
                throw std::runtime_error(
                    "diagnostics_id must use 1-64 lowercase letters, digits, '.', '_', or '-'");
            }
        }
        if (const auto source = config.find("audio_source");
            source != config.end())
        {
            if (!source->is_string())
                throw std::runtime_error("audio_source must be a string");
            const std::string value = source->get<std::string>();
            if (value == "input")
                options.audio.source = AudioOptions::Source::Input;
            else if (value == "synthetic")
                options.audio.source = AudioOptions::Source::Synthetic;
            else if (value == "silent")
                options.audio.source = AudioOptions::Source::Silent;
            else
                throw std::runtime_error(
                    "audio_source must be input, synthetic, or silent");
        }
        if (const auto device = config.find("audio_device");
            device != config.end())
        {
            if (!device->is_string())
                throw std::runtime_error("audio_device must be a string");
            options.audio.device_name = device->get<std::string>();
            if (options.audio.device_name.size() > 256)
                throw std::runtime_error("audio_device is too long");
        }
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
                || extension == ".glsl" || extension == ".h"
                || extension == ".rgen" || extension == ".rmiss"
                || extension == ".rchit" || extension == ".metal"
                || extension == ".png" || extension == ".jpg"
                || extension == ".jpeg" || extension == ".bmp"
                || extension == ".hdr" || extension == ".gltf"
                || extension == ".glb" || extension == ".obj"
                || extension == ".mtl" || extension == ".bin")
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
    auto scene = load_scene(options_, diagnostic, line, error);
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
        / "draxul-rezonality" / std::to_string(reinterpret_cast<uintptr_t>(this));
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
    candidate.surfaces = scene->surfaces;
    candidate.models = std::move(scene->models);
    candidate.passes = scene->passes;
    bool shader_compile_failed = false;
    for (auto& surface : candidate.surfaces)
    {
        if (surface.path.empty())
            continue;
        const bool hdr = surface.path.extension() == ".hdr";
        const bool loaded = hdr
            ? load_rgba32f_image(surface.path, surface.image_width,
                  surface.image_height, surface.image_float_pixels,
                  result.error)
            : load_rgba8_image(surface.path, surface.image_width,
                  surface.image_height, surface.image_pixels, result.error);
        if (!loaded)
        {
            result.diagnostic_path = surface.path;
            return result;
        }
        if (hdr)
            surface.format = ShaderBuild::SurfaceFormat::Color32Float;
    }
    for (size_t index = 0; index < candidate.passes.size(); ++index)
    {
        auto& pass = candidate.passes[index];
        const std::string stem = std::to_string(generation) + "-"
            + std::to_string(index);
        if (pass.ray_trace)
        {
            const fs::path raygen_output
                = output_directory / ("raygen-" + stem + ".spv");
            const fs::path miss_output
                = output_directory / ("miss-" + stem + ".spv");
            const fs::path closest_output
                = output_directory / ("closest-" + stem + ".spv");
            const bool raygen_ok = compile_shader(compiler,
                options_.project_path, pass.raygen_path, raygen_output,
                pass.raygen_spirv, result.diagnostics);
            const bool miss_ok = compile_shader(compiler,
                options_.project_path, pass.miss_path, miss_output,
                pass.miss_spirv, result.diagnostics);
            const bool closest_ok = compile_shader(compiler,
                options_.project_path, pass.closest_hit_path, closest_output,
                pass.closest_hit_spirv, result.diagnostics);
            if (!raygen_ok || !miss_ok || !closest_ok)
            {
                shader_compile_failed = true;
                fs::remove(raygen_output, ec);
                fs::remove(miss_output, ec);
                fs::remove(closest_output, ec);
                continue;
            }
            fs::remove(raygen_output, ec);
            fs::remove(miss_output, ec);
            fs::remove(closest_output, ec);
            const auto metal_source = read_text(pass.metal_ray_path);
            if (!metal_source)
            {
                result.diagnostic_path = pass.metal_ray_path;
                result.error = "Native Metal ray shader is missing";
                return result;
            }
            pass.metal_ray_source = *metal_source;
            continue;
        }
        const fs::path vertex_output
            = output_directory / ("vertex-" + stem + ".spv");
        const fs::path fragment_output
            = output_directory / ("fragment-" + stem + ".spv");
        const bool vertex_ok = compile_shader(compiler,
            options_.project_path, pass.vertex_path, vertex_output,
            pass.vertex_spirv, result.diagnostics);
        const bool fragment_ok = compile_shader(compiler,
            options_.project_path, pass.fragment_path, fragment_output,
            pass.fragment_spirv, result.diagnostics);
        if (!vertex_ok || !fragment_ok)
        {
            shader_compile_failed = true;
            fs::remove(vertex_output, ec);
            fs::remove(fragment_output, ec);
            continue;
        }
        fs::remove(vertex_output, ec);
        fs::remove(fragment_output, ec);
    }
    if (shader_compile_failed)
    {
        finalize_compile_diagnostics(result);
        return result;
    }
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
        if (forced || (dirty && now - dirty_since >= std::chrono::milliseconds(options_.compile_debounce_ms)))
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
