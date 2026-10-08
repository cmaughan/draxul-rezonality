#include "live_project.h"
#include "image_loader.h"
#include "path_utf8.h"
#include "shader_compiler.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace rezonality
{
namespace
{

using namespace std::chrono_literals;
namespace fs = std::filesystem;

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

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

fs::path normalized_path(const fs::path& path)
{
    std::error_code ec;
    fs::path normalized = fs::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : normalized;
}

void collect_shader_source(const fs::path& project_path,
    const fs::path& path, std::vector<fs::path>& sources,
    std::set<std::string>& seen)
{
    if (path.empty())
        return;
    const fs::path normalized = normalized_path(path);
    if (!seen.insert(generic_path_utf8(normalized)).second)
        return;
    sources.push_back(normalized);

    const auto contents = read_text(normalized);
    if (!contents)
        return;
    static const std::regex include_pattern(
        R"(^\s*#\s*include\s*\"([^\"]+)\")");
    std::istringstream lines(*contents);
    std::string line;
    while (std::getline(lines, line))
    {
        std::smatch match;
        if (!std::regex_search(line, match, include_pattern))
            continue;
        fs::path dependency = normalized.parent_path()
            / fs::u8path(match[1].str());
        if (!fs::is_regular_file(dependency))
            dependency = project_path / fs::u8path(match[1].str());
        if (fs::is_regular_file(dependency))
            collect_shader_source(project_path, dependency, sources, seen);
    }
}

void collect_active_sources(ShaderBuild& build)
{
    std::set<std::string> seen;
    build.source_files.clear();
    if (!build.scenegraph_path.empty())
    {
        const fs::path scenegraph = normalized_path(build.scenegraph_path);
        seen.insert(generic_path_utf8(scenegraph));
        build.source_files.push_back(scenegraph);
    }
    for (const auto& pass : build.passes)
    {
        collect_shader_source(build.project_path, pass.vertex_path,
            build.source_files, seen);
        collect_shader_source(build.project_path, pass.fragment_path,
            build.source_files, seen);
        collect_shader_source(build.project_path, pass.raygen_path,
            build.source_files, seen);
        collect_shader_source(build.project_path, pass.miss_path,
            build.source_files, seen);
        collect_shader_source(build.project_path, pass.closest_hit_path,
            build.source_files, seen);
        collect_shader_source(build.project_path, pass.metal_ray_path,
            build.source_files, seen);
    }
    std::sort(build.source_files.begin(), build.source_files.end());
}

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

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

float checked_float(std::string_view text, std::string_view field)
{
    std::string owned(text);
    char* end = nullptr;
    errno = 0;
    const float value = std::strtof(owned.c_str(), &end);
    if (end != owned.data() + owned.size() || errno == ERANGE
        || !std::isfinite(value))
    {
        throw std::runtime_error("invalid finite number for '"
            + std::string(field) + "': " + owned);
    }
    return value;
}

bool has_numeric_field(const std::string& body, std::string_view key)
{
    return std::regex_search(body,
        std::regex("\\b" + std::string(key) + R"(\s*:)"));
}

const std::string& numeric_token_capture()
{
    static const std::string capture = R"(([^\s,\)\}]+))";
    return capture;
}

[[noreturn]] void throw_invalid_numeric_field(std::string_view key)
{
    throw std::runtime_error("invalid numeric value for '"
        + std::string(key) + "'");
}

void parse_vec4(const std::string& body, std::string_view key,
    float (&value)[4], bool* found = nullptr)
{
    const auto& number = numeric_token_capture();
    const std::regex expression("\\b" + std::string(key)
        + R"(\s*:\s*\(\s*)" + number + R"(\s*,\s*)" + number
        + R"(\s*,\s*)" + number + R"(\s*,\s*)" + number
        + R"(\s*\))");
    std::smatch match;
    if (!std::regex_search(body, match, expression))
    {
        if (has_numeric_field(body, key))
            throw_invalid_numeric_field(key);
        if (found)
            *found = false;
        return;
    }
    for (size_t index = 0; index < 4; ++index)
        value[index] = checked_float(match[index + 1].str(), key);
    if (found)
        *found = true;
}

bool parse_vec3(const std::string& body, std::string_view key,
    glm::vec3& value)
{
    const auto& number = numeric_token_capture();
    const std::regex expression("\\b" + std::string(key)
        + R"(\s*:\s*\(\s*)" + number + R"(\s*,\s*)" + number
        + R"(\s*,\s*)" + number + R"(\s*\))");
    std::smatch match;
    if (!std::regex_search(body, match, expression))
    {
        if (has_numeric_field(body, key))
            throw_invalid_numeric_field(key);
        return false;
    }
    value = { checked_float(match[1].str(), key),
        checked_float(match[2].str(), key),
        checked_float(match[3].str(), key) };
    return true;
}

bool parse_vec2(const std::string& body, std::string_view key,
    glm::vec2& value)
{
    const auto& number = numeric_token_capture();
    const std::regex expression("\\b" + std::string(key)
        + R"(\s*:\s*\(\s*)" + number + R"(\s*,\s*)" + number
        + R"(\s*\))");
    std::smatch match;
    if (!std::regex_search(body, match, expression))
    {
        if (has_numeric_field(body, key))
            throw_invalid_numeric_field(key);
        return false;
    }
    value = { checked_float(match[1].str(), key),
        checked_float(match[2].str(), key) };
    return true;
}

bool parse_scalar(const std::string& body, std::string_view key,
    float& value)
{
    const std::regex expression("\\b" + std::string(key)
        + R"(\s*:\s*)" + numeric_token_capture());
    const auto matched = first_match(body, expression);
    if (!matched)
    {
        if (has_numeric_field(body, key))
            throw_invalid_numeric_field(key);
        return false;
    }
    value = checked_float(*matched, key);
    return true;
}

bool parse_surface_scale(const std::string& body, float& scale_x,
    float& scale_y)
{
    const auto& number = numeric_token_capture();
    const std::regex expression("\\bscale"
        R"(\s*:\s*\(\s*)" + number + R"(\s*,\s*)" + number
        + "(?:\\s*,\\s*" + number + ")?"
        + R"(\s*\))");
    std::smatch match;
    if (!std::regex_search(body, match, expression))
    {
        if (has_numeric_field(body, "scale"))
            throw_invalid_numeric_field("scale");
        return false;
    }
    scale_x = checked_float(match[1].str(), "scale");
    scale_y = checked_float(match[2].str(), "scale");
    return true;
}

void parse_surface_format(const std::string& body,
    ShaderBuild::Surface& surface)
{
    const auto format = first_match(body,
        std::regex(R"(\bformat\s*:\s*([A-Za-z0-9_]+))"));
    if (!format)
        return;
    surface.format_explicit = true;
    if (*format == "rgba16f")
        surface.format = ShaderBuild::SurfaceFormat::Color16Float;
    else if (*format == "rgba32f")
        surface.format = ShaderBuild::SurfaceFormat::Color32Float;
    else if (format->find("depth") != std::string::npos)
        surface.format = ShaderBuild::SurfaceFormat::Depth32;
    else
        surface.format = ShaderBuild::SurfaceFormat::Color8;
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

std::regex block_header(std::string_view kind)
{
    return std::regex("(^|[\\r\\n])\\s*" + std::string(kind)
        + R"(\s*:\s*([A-Za-z_][A-Za-z0-9_.-]*)\s*\{)");
}

// Name and offset of the opening brace of each block of kind.
std::vector<std::pair<std::string, size_t>> named_block_offsets(
    const std::string& source, std::string_view kind)
{
    std::vector<std::pair<std::string, size_t>> blocks;
    const std::regex header = block_header(kind);
    for (auto begin = std::sregex_iterator(source.begin(), source.end(), header);
         begin != std::sregex_iterator(); ++begin)
        blocks.emplace_back((*begin)[2].str(),
            static_cast<size_t>(begin->position() + begin->length() - 1));
    return blocks;
}

std::vector<std::pair<std::string, std::string>> named_blocks(
    const std::string& source, std::string_view kind)
{
    std::vector<std::pair<std::string, std::string>> blocks;
    const std::regex header = block_header(kind);
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

// One-based line of a named block header, or 1 when it cannot be found.
// Comment stripping preserves line breaks, so parsed text lines match the
// source the user edits.
int block_line(const std::string& parsed, std::string_view kind,
    std::string_view name)
{
    for (const auto& [block, open] : named_block_offsets(parsed, kind))
        if (block == name)
            return 1 + static_cast<int>(std::count(
                parsed.begin(), parsed.begin() + open, '\n'));
    return 1;
}

// Both native backends bind a sampler to the surface's current texture and
// keep no history copy. Reject inputs they cannot honour before preparation:
// a previous-frame sampler would silently read the current target, and a
// pass sampling its own target reads and writes one texture at once.
// Sampling a surface written by an earlier pass remains valid.
bool validate_pass_inputs(const std::string& parsed,
    const SceneDescription& description, int& diagnostic_line,
    std::string& error)
{
    for (const auto& pass : description.passes)
    {
        for (const auto& sampler : pass.samplers)
        {
            if (sampler.previous_frame)
            {
                error = "Pass '" + pass.name + "' samples '!" + sampler.surface
                    + "' (previous frame), which Rezonality does not support; "
                      "sample a surface written by an earlier pass instead";
            }
            else if (std::find(pass.targets.begin(), pass.targets.end(),
                         sampler.surface)
                != pass.targets.end())
            {
                error = "Pass '" + pass.name + "' samples '" + sampler.surface
                    + "' while writing it as a target; write a separate "
                      "surface and sample it in a later pass";
            }
            else
            {
                continue;
            }
            diagnostic_line = block_line(parsed, "pass", pass.name);
            return false;
        }
    }
    return true;
}

std::optional<SceneDescription> parse_scene_text_impl(
    const ProjectOptions& options, const fs::path& scenegraph,
    std::string_view source, fs::path& diagnostic_path,
    int& diagnostic_line, std::string& error)
{
    diagnostic_path = scenegraph;
    const std::string parsed = without_comments(std::string(source));
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
        const fs::path model_path
            = options.project_path / fs::u8path(*path);
        model_indices[name] = description.models.size();
        description.models.push_back({
            .name = name,
            .path = model_path,
            .scale = scale,
            .flip_texture_y = flip_texture_y,
        });
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
        parse_surface_format(body, surface);
        parse_surface_scale(body, surface.scale_x, surface.scale_y);
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
        parse_surface_format(body, surface);
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
            const fs::path model_path
                = options.project_path / fs::u8path(*geometry_path);
            pass.model_index = description.models.size();
            description.models.push_back({
                .name = name + ".geometry",
                .path = model_path,
                .scale = scale,
                .flip_texture_y = flip_texture_y,
            });
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
    if (!validate_pass_inputs(parsed, description, diagnostic_line, error))
        return std::nullopt;
    for (auto& pass : description.passes)
        for (const auto& target : pass.targets)
            if (target != "default_color" && target != "default_depth")
                for (auto& surface : description.surfaces)
                    surface.target = surface.target || surface.name == target;
    return description;
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

} // namespace

SceneParseResult parse_scene_text(const ProjectOptions& options,
    const fs::path& scenegraph, std::string_view source)
{
    SceneParseResult result;
    result.diagnostic_path = scenegraph;
    try
    {
        result.scene = parse_scene_text_impl(options, scenegraph, source,
            result.diagnostic_path, result.diagnostic_line, result.error);
    }
    catch (const std::exception& exception)
    {
        result.error = std::string("Rezonality project build failed: ")
            + exception.what();
    }
    catch (...)
    {
        result.error
            = "Rezonality project build failed with an unknown error";
    }
    return result;
}

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
            + display_path_utf8(options.project_path);
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

ProjectPipeline::ProjectPipeline(fs::path plugin_directory,
    ProjectOptions options, CompileShader compile_shader)
    : plugin_directory_(std::move(plugin_directory))
    , options_(std::move(options))
    , compile_shader_(std::move(compile_shader))
{
}

bool validate_surface_upload_storage(
    const ShaderBuild::Surface& surface, std::string& error)
{
    const bool byte_storage = !surface.image_pixels.empty();
    const bool float_storage = !surface.image_float_pixels.empty();
    if (!byte_storage && !float_storage)
        return true;
    if (surface.image_width == 0 || surface.image_height == 0
        || byte_storage == float_storage
        || surface.image_width > std::numeric_limits<size_t>::max()
                / surface.image_height / 4)
    {
        error = "Rezonality image surface '" + surface.name
            + "' has invalid upload storage";
        return false;
    }
    const size_t expected = static_cast<size_t>(surface.image_width)
        * surface.image_height * 4;
    const bool valid = byte_storage
        ? surface.format == ShaderBuild::SurfaceFormat::Color8
            && surface.image_pixels.size() == expected
        : surface.format == ShaderBuild::SurfaceFormat::Color32Float
            && surface.image_float_pixels.size() == expected;
    if (valid)
        return true;
    error = "Rezonality image surface '" + surface.name
        + "' has invalid upload storage";
    return false;
}

LiveProject::LiveProject(fs::path plugin_directory,
    ProjectOptions options, WakeCallback wake,
    ProjectPipeline::CompileShader compile_shader)
    : pipeline_(std::move(plugin_directory), std::move(options),
          std::move(compile_shader))
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

uint64_t ProjectPipeline::fingerprint() const
{
    uint64_t hash = kFnvOffset;
    std::error_code ec;
    fs::recursive_directory_iterator iterator(options_.project_path,
        fs::directory_options::skip_permission_denied, ec);
    if (ec)
        throw std::runtime_error("could not scan Rezonality project: "
            + ec.message());
    std::vector<fs::path> files;
    for (; iterator != fs::recursive_directory_iterator(); iterator.increment(ec))
    {
        if (ec)
            throw std::runtime_error("could not scan Rezonality project: "
                + ec.message());
        const auto& entry = *iterator;
        // Shader includes have no required suffix. Ignore only repository
        // metadata, not unrecognized files that a shader may include.
        if (entry.is_directory() && entry.path().filename() == ".git")
        {
            iterator.disable_recursion_pending();
            continue;
        }
        std::error_code entry_error;
        if (entry.is_regular_file(entry_error))
            files.push_back(entry.path());
        else if (entry_error)
            throw std::runtime_error("could not inspect Rezonality project file: "
                + entry_error.message());
    }
    if (ec)
        throw std::runtime_error("could not scan Rezonality project: "
            + ec.message());
    std::sort(files.begin(), files.end());
    for (const auto& file : files)
    {
        hash = hash_string(hash, generic_path_utf8(file));
        if (const auto contents = read_text(file))
            hash = hash_string(hash, *contents);
    }
    return hash;
}

BuildResult build_candidate(const fs::path& plugin_directory,
    const ProjectOptions& options, SceneDescription scene,
    uint64_t generation, const fs::path& output_directory,
    CompileShaderOperation compile_shader_operation)
{
    BuildResult result;
    result.generation = generation;
    try
    {
    const fs::path compiler = bundled_compiler_path(plugin_directory);
    std::error_code compiler_error;
    if (!compile_shader_operation
        && !fs::is_regular_file(compiler, compiler_error))
    {
        result.diagnostic_path = compiler;
        result.error = "Bundled glslangValidator is missing";
        return result;
    }

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
    candidate.project_path = options.project_path;
    candidate.scenegraph_path = scene.scenegraph;
    candidate.surfaces = std::move(scene.surfaces);
    candidate.passes = std::move(scene.passes);
    candidate.models.reserve(scene.models.size());
    for (const SceneModelSource& source : scene.models)
    {
        ModelData model;
        if (!load_model(source.path, source.scale, source.flip_texture_y,
                model, result.error))
        {
            result.diagnostic_path = source.path;
            result.diagnostic_line = 1;
            return result;
        }
        candidate.models.push_back(std::move(model));
    }
    collect_active_sources(candidate);
    bool shader_compile_failed = false;
    const auto compile = [&compile_shader_operation, &compiler, &options,
                             &result](
                             const fs::path& shader,
                             const fs::path& output,
                             std::vector<uint32_t>& spirv) {
        if (!compile_shader_operation)
        {
            return compile_shader({
                                      .compiler = compiler,
                                      .project_path = options.project_path,
                                      .shader = shader,
                                      .output_path = output,
                                  },
                CompilerEnvironment{}, spirv, result.diagnostics);
        }
        if (!compile_shader_operation(
                shader, spirv, result.diagnostics))
            return false;
        if (!spirv.empty())
            return true;
        if (result.diagnostics.size() < kMaximumBuildDiagnostics)
        {
            result.diagnostics.push_back({
                .path = shader,
                .stage = "compile",
                .severity = "error",
                .message = "injected compiler produced no SPIR-V for "
                    + path_utf8(shader.filename()),
            });
        }
        return false;
    };
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
        const auto storage_format = hdr
            ? ShaderBuild::SurfaceFormat::Color32Float
            : ShaderBuild::SurfaceFormat::Color8;
        if (surface.format_explicit && surface.format != storage_format)
        {
            result.diagnostic_path = surface.path;
            result.error = "Image surface '" + surface.name
                + "' has a format that is incompatible with decoded "
                  "pixel storage";
            return result;
        }
        surface.format = storage_format;
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
            const bool raygen_ok = compile(pass.raygen_path,
                raygen_output, pass.raygen_spirv);
            const bool miss_ok = compile(pass.miss_path,
                miss_output, pass.miss_spirv);
            const bool closest_ok = compile(pass.closest_hit_path,
                closest_output, pass.closest_hit_spirv);
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
        const bool vertex_ok = compile(pass.vertex_path,
            vertex_output, pass.vertex_spirv);
        const bool fragment_ok = compile(pass.fragment_path,
            fragment_output, pass.fragment_spirv);
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
    catch (const std::exception& exception)
    {
        result.diagnostic_path = options.project_path / options.scenegraph;
        result.error = std::string("Rezonality project build failed: ")
            + exception.what();
        return result;
    }
    catch (...)
    {
        result.diagnostic_path = options.project_path / options.scenegraph;
        result.error = "Rezonality project build failed with an unknown error";
        return result;
    }
}

BuildResult ProjectPipeline::build(uint64_t generation) const
{
    fs::path scenegraph = options_.scenegraph;
    if (scenegraph.is_relative())
        scenegraph = options_.project_path / scenegraph;
    const auto source = read_text(scenegraph);
    if (!source)
    {
        BuildResult result;
        result.generation = generation;
        result.diagnostic_path = scenegraph;
        result.error = "Scenegraph is missing: " + display_path_utf8(scenegraph);
        return result;
    }

    SceneParseResult parsed = parse_scene_text(
        options_, scenegraph, *source);
    if (!parsed.scene)
    {
        BuildResult result;
        result.generation = generation;
        result.diagnostic_path = std::move(parsed.diagnostic_path);
        result.diagnostic_line = parsed.diagnostic_line;
        result.error = std::move(parsed.error);
        return result;
    }

    // Temporary storage can disappear or be misconfigured (TMPDIR/TMP/TEMP)
    // while the editor is running. Report it as an ordinary failed
    // generation so the active scene is retained and a later rebuild can
    // succeed once storage is available again.
    std::error_code temp_error;
    const fs::path temp_root = fs::temp_directory_path(temp_error);
    if (temp_error || temp_root.empty())
    {
        BuildResult result;
        result.generation = generation;
        result.diagnostic_path = temp_root.empty() ? scenegraph : temp_root;
        result.error = "Temporary storage for shader output is unavailable: "
            + (temp_error ? temp_error.message()
                          : std::string("empty temporary directory"));
        return result;
    }
    const fs::path output_directory = temp_root / "draxul-rezonality"
        / std::to_string(reinterpret_cast<uintptr_t>(this));
    return build_candidate(plugin_directory_, options_,
        std::move(*parsed.scene), generation, output_directory,
        compile_shader_);
}

void LiveProject::run(std::stop_token stop_token)
{
    const ProjectOptions& options = pipeline_.options();
    uint64_t generation = 0;
    uint64_t observed_fingerprint = 0;
    std::string last_watch_error;
    try
    {
        observed_fingerprint = pipeline_.fingerprint();
    }
    catch (const std::exception&)
    {
        // The watch loop publishes the next fingerprint failure as a normal
        // build result and keeps polling for a repaired project directory.
    }
    bool dirty = true;
    auto dirty_since = std::chrono::steady_clock::now()
        - std::chrono::milliseconds(options.compile_debounce_ms);

    while (!stop_token.stop_requested())
    {
        bool forced = false;
        {
            std::unique_lock lock(mutex_);
            forced = force_requested_;
            force_requested_ = false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (forced || (dirty && now - dirty_since >= std::chrono::milliseconds(options.compile_debounce_ms)))
        {
            ++generation;
            BuildResult next;
            try
            {
                next = pipeline_.build(generation);
            }
            catch (const std::exception& exception)
            {
                next = BuildResult{};
                next.generation = generation;
                next.diagnostic_path = options.project_path;
                next.error = std::string("Rezonality project build failed: ")
                    + exception.what();
            }
            catch (...)
            {
                next = BuildResult{};
                next.generation = generation;
                next.diagnostic_path = options.project_path;
                next.error
                    = "Rezonality project build failed with an unknown error";
            }
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
        if (options.auto_reload)
        {
            uint64_t fingerprint = observed_fingerprint;
            try
            {
                fingerprint = pipeline_.fingerprint();
            }
            catch (const std::exception& exception)
            {
                const std::string watch_error = exception.what();
                if (watch_error == last_watch_error)
                    continue;
                last_watch_error = watch_error;
                BuildResult failed;
                failed.generation = ++generation;
                failed.diagnostic_path = options.project_path;
                failed.error = std::string("Rezonality project watch failed: ")
                    + watch_error;
                {
                    std::lock_guard result_lock(mutex_);
                    result_ = std::move(failed);
                }
                if (wake_ && !stop_token.stop_requested())
                    wake_();
                continue;
            }
            const bool recovered_from_watch_error
                = !last_watch_error.empty();
            last_watch_error.clear();
            if (recovered_from_watch_error
                || fingerprint != observed_fingerprint)
            {
                observed_fingerprint = fingerprint;
                dirty = true;
                dirty_since = std::chrono::steady_clock::now();
            }
        }
    }
}

} // namespace rezonality
