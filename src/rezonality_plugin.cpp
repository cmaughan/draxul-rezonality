#include "live_project.h"

#include "camera.h"
#include "diagnostics.h"
#include "model_loader.h"

#include <draxul/plugin_adapter.h>
#include <draxul/plugin_api.h>
#include <draxul/plugin_host_services.h>

#include <nlohmann/json.hpp>

#if defined(__APPLE__)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <spirv_msl.hpp>
#else
#include <draxul/vulkan/vk_plugin_allocator.h>
#include <draxul/vulkan/vk_resource_helpers.h>
#include <vulkan/vulkan.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace
{

using draxul::plugin_support::render_result;
using draxul::plugin_support::tick_result;
using rezonality::AudioAnalyzer;
using rezonality::AudioOptions;
using rezonality::AudioTextureFrame;
using rezonality::BuildResult;
using rezonality::DiagnosticEntry;
using rezonality::DiagnosticState;
using rezonality::DiagnosticsPublisher;
using rezonality::LiveProject;
using rezonality::ProjectOptions;
using rezonality::ShaderBuild;

constexpr const char* kPluginId = "dev.draxul.rezonality";
constexpr const char* kPluginVersion = "0.7.0";
constexpr size_t kCommonUniformFloatCount = 192;
using CommonUniformBlock = std::array<float, kCommonUniformFloatCount>;

size_t align_up(size_t value, size_t alignment)
{
    return alignment == 0
        ? value
        : (value + alignment - 1) / alignment * alignment;
}

CommonUniformBlock make_common_uniforms(double elapsed_seconds,
    uint32_t width, uint32_t height, int32_t origin_x, int32_t origin_y,
    const rezonality::Camera& camera)
{
    CommonUniformBlock uniform{};
    const float time = static_cast<float>(std::max(0.0, elapsed_seconds));
    uniform[0] = time;
    uniform[1] = time;
    uniform[2] = 1.0f / 60.0f;
    uniform[3] = time * 60.0f;
    uniform[4] = 60.0f;
    uniform[6] = 1.0f;
    const uint32_t vertex_size = sizeof(rezonality::ModelVertex);
    std::memcpy(&uniform[7], &vertex_size, sizeof(vertex_size));
    uniform[8] = static_cast<float>(width);
    uniform[9] = static_cast<float>(height);
    uniform[10] = 1.0f;
    uniform[48] = static_cast<float>(origin_x);
    uniform[49] = static_cast<float>(origin_y);
    uniform[52] = camera.position.x;
    uniform[53] = camera.position.y;
    uniform[54] = camera.position.z;
    uniform[55] = 1.0f;
    const glm::mat4 model(1.0f);
    const glm::mat4 view = rezonality::camera_view(camera);
    const glm::mat4 projection
        = rezonality::camera_projection(camera, width, height);
    const glm::mat4 model_view_projection = projection * view * model;
    const glm::mat4 view_inverse = glm::inverse(view);
    const glm::mat4 projection_inverse = glm::inverse(projection);
    const auto copy_matrix = [&uniform](size_t offset,
                                 const glm::mat4& matrix) {
        std::memcpy(uniform.data() + offset, glm::value_ptr(matrix),
            sizeof(glm::mat4));
    };
    copy_matrix(56, model);
    copy_matrix(72, view);
    copy_matrix(88, projection);
    copy_matrix(104, model_view_projection);
    copy_matrix(120, view_inverse);
    copy_matrix(136, projection_inverse);
    return uniform;
}

struct ScreenVertex
{
    float position[4];
    float uv[2];
    float color[3];
    float normal[3];
};

constexpr ScreenVertex kScreenVertices[] = {
    { { -1, -1, 0, 1 }, { 0, 1 }, { 1, 1, 1 }, { 0, 0, 1 } },
    { { 1, -1, 0, 1 }, { 1, 1 }, { 1, 1, 1 }, { 0, 0, 1 } },
    { { 1, 1, 0, 1 }, { 1, 0 }, { 1, 1, 1 }, { 0, 0, 1 } },
    { { -1, -1, 0, 1 }, { 0, 1 }, { 1, 1, 1 }, { 0, 0, 1 } },
    { { 1, 1, 0, 1 }, { 1, 0 }, { 1, 1, 1 }, { 0, 0, 1 } },
    { { -1, 1, 0, 1 }, { 0, 0 }, { 1, 1, 1 }, { 0, 0, 1 } },
};

#if defined(__APPLE__)

struct MetalGeneration
{
    struct Surface
    {
        std::string name;
        id<MTLTexture> texture = nil;
        bool depth = false;
        bool repeat = false;
        bool audio_analysis = false;
        uint64_t audio_generation = 0;
    };
    struct Pass
    {
        id<MTLRenderPipelineState> pipeline = nil;
        id<MTLComputePipelineState> ray_pipeline = nil;
        std::vector<size_t> targets;
        std::vector<size_t> samplers;
        std::optional<size_t> model_index;
        bool ray_trace = false;
        bool direct = false;
        bool has_clear = false;
        float clear[4] = { 0, 0, 0, 1 };
    };
    struct Model
    {
        id<MTLBuffer> vertex_buffer = nil;
        id<MTLBuffer> index_buffer = nil;
        id<MTLBuffer> material_buffer = nil;
        std::array<std::vector<id<MTLTexture>>, 5> textures;
        std::vector<rezonality::ModelPart> parts;
        id<MTLAccelerationStructure> blas = nil;
        id<MTLAccelerationStructure> tlas = nil;
        id<MTLBuffer> acceleration_scratch = nil;
        id<MTLBuffer> acceleration_instances = nil;
        MTLPrimitiveAccelerationStructureDescriptor* blas_descriptor = nil;
        MTLInstanceAccelerationStructureDescriptor* tlas_descriptor = nil;
        bool acceleration_structures_built = false;
    };
    std::vector<Surface> surfaces;
    std::vector<Model> models;
    std::vector<Pass> passes;
    id<MTLBuffer> uniform_buffer = nil;
    size_t uniform_stride = 0;
    uint32_t buffered_frame_count = 1;
    id<MTLSamplerState> clamp_sampler = nil;
    id<MTLSamplerState> repeat_sampler = nil;
    MTLPixelFormat format = MTLPixelFormatInvalid;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t source_generation = 0;
    uint64_t used_slots = 0;
    bool ray_project = false;
};

struct RetiredGeneration
{
    MetalGeneration generation;
    uint64_t pending_slots = 0;
};

struct BackendState
{
    id<MTLDevice> device = nil;
    id<MTLBuffer> vertex_buffer = nil;
    std::optional<MetalGeneration> active;
    std::vector<RetiredGeneration> retired;
};

std::string ns_error(NSError* error)
{
    return error ? std::string(error.localizedDescription.UTF8String
                           ?: "unknown error")
                 : std::string("unknown error");
}

bool convert_to_msl(const std::vector<uint32_t>& spirv,
    spv::ExecutionModel model, const char* entry_point,
    std::string& source, std::string& error)
{
    try
    {
        spirv_cross::CompilerMSL compiler(spirv);
        auto options = compiler.get_msl_options();
        options.platform = spirv_cross::CompilerMSL::Options::macOS;
        options.msl_version
            = spirv_cross::CompilerMSL::Options::make_msl_version(2, 3);
        compiler.set_msl_options(options);
        spirv_cross::MSLResourceBinding uniform{};
        uniform.stage = model;
        uniform.desc_set = 0;
        uniform.binding = 0;
        uniform.msl_buffer = 1;
        compiler.add_msl_resource_binding(uniform);
        for (uint32_t index = 0; index < 16; ++index)
        {
            spirv_cross::MSLResourceBinding sampler{};
            sampler.stage = model;
            sampler.desc_set = 1;
            sampler.binding = index;
            sampler.msl_texture = index;
            sampler.msl_sampler = index;
            compiler.add_msl_resource_binding(sampler);
        }
        spirv_cross::MSLResourceBinding materials{};
        materials.stage = model;
        materials.desc_set = 2;
        materials.binding = 0;
        materials.msl_buffer = 2;
        compiler.add_msl_resource_binding(materials);
        for (uint32_t binding = 1; binding <= 5; ++binding)
        {
            spirv_cross::MSLResourceBinding texture{};
            texture.stage = model;
            texture.desc_set = 2;
            texture.binding = binding;
            texture.msl_texture = 16
                + (binding - 1) * rezonality::kMaxModelMaterials;
            texture.msl_sampler = texture.msl_texture;
            compiler.add_msl_resource_binding(texture);
        }
        compiler.rename_entry_point("main", entry_point, model);
        compiler.set_entry_point(entry_point, model);
        source = compiler.compile();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("SPIRV-Cross failed: ") + exception.what();
        return false;
    }
}

std::optional<MetalGeneration::Model> create_metal_model(
    id<MTLDevice> device, const rezonality::ModelData& source,
    std::string& error)
{
    MetalGeneration::Model model;
    model.vertex_buffer = [device newBufferWithBytes:source.vertices.data()
                                              length:source.vertices.size() * sizeof(rezonality::ModelVertex)
                                             options:MTLResourceStorageModeShared];
    model.index_buffer = [device newBufferWithBytes:source.indices.data()
                                             length:source.indices.size() * sizeof(uint32_t)
                                            options:MTLResourceStorageModeShared];
    struct alignas(16) GpuMaterial
    {
        glm::vec4 base_color;
        glm::vec4 emissive;
        glm::vec4 metallic_roughness_occlusion;
        glm::ivec4 texture_indices;
    };
    std::array<GpuMaterial, rezonality::kMaxModelMaterials> materials{};
    for (size_t index = 0; index < source.materials.size(); ++index)
    {
        const auto& material = source.materials[index];
        materials[index] = { material.base_color_factor,
            material.emissive_factor,
            { material.metallic_factor, material.roughness_factor,
                material.occlusion_strength, 0.0f },
            glm::ivec4(static_cast<int>(index)) };
    }
    model.material_buffer = [device newBufferWithBytes:materials.data()
                                                length:sizeof(materials)
                                               options:MTLResourceStorageModeShared];
    if (!model.vertex_buffer || !model.index_buffer
        || !model.material_buffer)
    {
        error = "Rezonality could not create Metal model buffers";
        return std::nullopt;
    }
    for (const auto& material : source.materials)
    {
        const rezonality::ModelTexture* textures[] = {
            &material.base_color, &material.normal,
            &material.metallic_roughness, &material.emissive,
            &material.occlusion
        };
        for (size_t kind = 0; kind < std::size(textures); ++kind)
        {
            const auto& source_texture = *textures[kind];
            MTLTextureDescriptor* descriptor
                = [[MTLTextureDescriptor alloc] init];
            descriptor.textureType = MTLTextureType2D;
            descriptor.width = source_texture.width;
            descriptor.height = source_texture.height;
            descriptor.pixelFormat = source_texture.srgb
                ? MTLPixelFormatRGBA8Unorm_sRGB
                : MTLPixelFormatRGBA8Unorm;
            descriptor.storageMode = MTLStorageModeManaged;
            descriptor.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> texture
                = [device newTextureWithDescriptor:descriptor];
            if (!texture)
            {
                error = "Rezonality could not create a Metal model texture";
                return std::nullopt;
            }
            [texture replaceRegion:MTLRegionMake2D(0, 0,
                                       source_texture.width, source_texture.height)
                       mipmapLevel:0
                         withBytes:source_texture.pixels.data()
                       bytesPerRow:source_texture.width * 4];
            model.textures[kind].push_back(texture);
        }
    }
    model.parts = source.parts;
    return model;
}

bool create_metal_acceleration_resources(id<MTLDevice> device,
    const rezonality::ModelData& source, MetalGeneration::Model& model,
    std::string& error)
{
    if (@available(macOS 11.0, *))
    {
        if (![device supportsRaytracing])
        {
            error = "Metal ray tracing is unsupported by this device";
            return false;
        }
        MTLAccelerationStructureTriangleGeometryDescriptor* geometry
            = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        geometry.vertexBuffer = model.vertex_buffer;
        geometry.vertexBufferOffset
            = offsetof(rezonality::ModelVertex, position);
        geometry.vertexStride = sizeof(rezonality::ModelVertex);
        if (@available(macOS 13.0, *))
            geometry.vertexFormat = MTLAttributeFormatFloat3;
        geometry.indexBuffer = model.index_buffer;
        geometry.indexBufferOffset = 0;
        geometry.indexType = MTLIndexTypeUInt32;
        geometry.triangleCount = source.indices.size() / 3;
        geometry.opaque = YES;
        model.blas_descriptor
            = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
        model.blas_descriptor.geometryDescriptors = @[ geometry ];
        const MTLAccelerationStructureSizes blas_sizes
            = [device accelerationStructureSizesWithDescriptor:
                          model.blas_descriptor];
        model.blas = [device newAccelerationStructureWithSize:
                                 blas_sizes.accelerationStructureSize];
        MTLAccelerationStructureInstanceDescriptor instance{};
        instance.transformationMatrix = MTLPackedFloat4x3(
            MTLPackedFloat3(1.0f, 0.0f, 0.0f),
            MTLPackedFloat3(0.0f, 1.0f, 0.0f),
            MTLPackedFloat3(0.0f, 0.0f, 1.0f),
            MTLPackedFloat3(0.0f, 0.0f, 0.0f));
        instance.mask = 0xff;
        instance.accelerationStructureIndex = 0;
        model.acceleration_instances = [device newBufferWithBytes:&instance
                                                           length:sizeof(instance)
                                                          options:MTLResourceStorageModeShared];
        model.tlas_descriptor
            = [MTLInstanceAccelerationStructureDescriptor descriptor];
        model.tlas_descriptor.instanceDescriptorBuffer
            = model.acceleration_instances;
        model.tlas_descriptor.instanceDescriptorStride
            = sizeof(MTLAccelerationStructureInstanceDescriptor);
        model.tlas_descriptor.instanceCount = 1;
        model.tlas_descriptor.instancedAccelerationStructures
            = @[ model.blas ];
        if (@available(macOS 12.0, *))
            model.tlas_descriptor.instanceDescriptorType
                = MTLAccelerationStructureInstanceDescriptorTypeDefault;
        const MTLAccelerationStructureSizes tlas_sizes
            = [device accelerationStructureSizesWithDescriptor:
                          model.tlas_descriptor];
        model.tlas = [device newAccelerationStructureWithSize:
                                 tlas_sizes.accelerationStructureSize];
        model.acceleration_scratch = [device newBufferWithLength:
                                                 std::max(blas_sizes.buildScratchBufferSize,
                                                     tlas_sizes.buildScratchBufferSize)
                                                         options:MTLResourceStorageModePrivate];
        if (!model.blas || !model.tlas || !model.acceleration_instances
            || !model.acceleration_scratch)
        {
            error = "Rezonality could not allocate Metal ray resources";
            return false;
        }
        return true;
    }
    error = "Metal ray tracing requires macOS 11 or newer";
    return false;
}

std::optional<MetalGeneration> create_generation(BackendState& backend,
    const ShaderBuild& build, const DraxulPluginMetalFrameV2& frame,
    double animation_seconds, const rezonality::Camera& camera,
    std::string& error)
{
    id<MTLDevice> device = (__bridge id<MTLDevice>)frame.device;
    id<MTLTexture> target
        = (__bridge id<MTLTexture>)frame.drawable_texture;
    if (!backend.vertex_buffer || backend.device != device)
    {
        backend.device = device;
        backend.vertex_buffer = [device newBufferWithBytes:kScreenVertices
                                                    length:sizeof(kScreenVertices)
                                                   options:MTLResourceStorageModeShared];
        if (!backend.vertex_buffer)
        {
            error = "Rezonality could not create its Metal screen rectangle";
            return std::nullopt;
        }
    }

    MTLVertexDescriptor* vertices = [[MTLVertexDescriptor alloc] init];
    vertices.attributes[0].format = MTLVertexFormatFloat4;
    vertices.attributes[0].offset = offsetof(ScreenVertex, position);
    vertices.attributes[0].bufferIndex = 0;
    vertices.attributes[1].format = MTLVertexFormatFloat2;
    vertices.attributes[1].offset = offsetof(ScreenVertex, uv);
    vertices.attributes[1].bufferIndex = 0;
    vertices.attributes[2].format = MTLVertexFormatFloat3;
    vertices.attributes[2].offset = offsetof(ScreenVertex, color);
    vertices.attributes[2].bufferIndex = 0;
    vertices.attributes[3].format = MTLVertexFormatFloat3;
    vertices.attributes[3].offset = offsetof(ScreenVertex, normal);
    vertices.attributes[3].bufferIndex = 0;
    vertices.layouts[0].stride = sizeof(ScreenVertex);
    vertices.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    MTLVertexDescriptor* model_vertices
        = [[MTLVertexDescriptor alloc] init];
    const MTLVertexFormat model_formats[] = {
        MTLVertexFormatFloat4, MTLVertexFormatFloat2,
        MTLVertexFormatFloat3, MTLVertexFormatFloat3,
        MTLVertexFormatFloat3, MTLVertexFormatFloat3
    };
    const NSUInteger model_offsets[] = {
        offsetof(rezonality::ModelVertex, position),
        offsetof(rezonality::ModelVertex, uv),
        offsetof(rezonality::ModelVertex, color),
        offsetof(rezonality::ModelVertex, normal),
        offsetof(rezonality::ModelVertex, tangent),
        offsetof(rezonality::ModelVertex, bitangent)
    };
    for (NSUInteger index = 0; index < 6; ++index)
    {
        model_vertices.attributes[index].format = model_formats[index];
        model_vertices.attributes[index].offset = model_offsets[index];
        model_vertices.attributes[index].bufferIndex = 0;
    }
    model_vertices.layouts[0].stride = sizeof(rezonality::ModelVertex);
    model_vertices.layouts[0].stepFunction
        = MTLVertexStepFunctionPerVertex;

    MetalGeneration generation;
    generation.ray_project = std::any_of(build.passes.begin(),
        build.passes.end(), [](const auto& pass) {
            return pass.ray_trace;
        });
    generation.width = static_cast<uint32_t>(std::max(1, frame.viewport.width));
    generation.height = static_cast<uint32_t>(std::max(1, frame.viewport.height));
    generation.buffered_frame_count
        = std::max(1u, frame.buffered_frame_count);
    generation.uniform_stride = align_up(sizeof(CommonUniformBlock), 256);
    generation.uniform_buffer = [device newBufferWithLength:
                                            generation.uniform_stride * generation.buffered_frame_count
                                                    options:MTLResourceStorageModeShared];
    if (!generation.uniform_buffer)
    {
        error = "Rezonality could not create its Metal common uniforms";
        return std::nullopt;
    }
    const auto uniform = make_common_uniforms(
        animation_seconds,
        generation.width, generation.height,
        frame.viewport.x, frame.viewport.y, camera);
    for (uint32_t slot = 0; slot < generation.buffered_frame_count; ++slot)
    {
        std::memcpy(static_cast<uint8_t*>(
                        generation.uniform_buffer.contents)
                + generation.uniform_stride * slot,
            uniform.data(), sizeof(uniform));
    }
    MTLSamplerDescriptor* sampler_descriptor
        = [[MTLSamplerDescriptor alloc] init];
    sampler_descriptor.minFilter = MTLSamplerMinMagFilterLinear;
    sampler_descriptor.magFilter = MTLSamplerMinMagFilterLinear;
    sampler_descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    generation.clamp_sampler
        = [device newSamplerStateWithDescriptor:sampler_descriptor];
    sampler_descriptor.sAddressMode = MTLSamplerAddressModeRepeat;
    sampler_descriptor.tAddressMode = MTLSamplerAddressModeRepeat;
    generation.repeat_sampler
        = [device newSamplerStateWithDescriptor:sampler_descriptor];
    for (const auto& source : build.surfaces)
    {
        MetalGeneration::Surface surface;
        surface.name = source.name;
        surface.depth = source.format == ShaderBuild::SurfaceFormat::Depth32;
        surface.audio_analysis = source.audio_analysis;
        surface.repeat = !source.audio_analysis
            && (!source.image_pixels.empty()
                || !source.image_float_pixels.empty());
        MTLTextureDescriptor* texture = [[MTLTextureDescriptor alloc] init];
        texture.textureType = MTLTextureType2D;
        texture.width = source.image_width != 0 ? source.image_width
                                                : std::max<NSUInteger>(1, static_cast<NSUInteger>(generation.width * std::max(0.01f, source.scale_x)));
        texture.height = source.image_height != 0 ? source.image_height
                                                  : std::max<NSUInteger>(1, static_cast<NSUInteger>(generation.height * std::max(0.01f, source.scale_y)));
        const bool has_image = !source.image_pixels.empty()
            || !source.image_float_pixels.empty();
        texture.storageMode = !has_image
            ? MTLStorageModePrivate
            : MTLStorageModeManaged;
        texture.usage = surface.depth
            ? MTLTextureUsageRenderTarget
            : MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead
                | (generation.ray_project
                        ? MTLTextureUsageShaderWrite
                        : 0);
        switch (source.format)
        {
        case ShaderBuild::SurfaceFormat::Color16Float:
            texture.pixelFormat = MTLPixelFormatRGBA16Float;
            break;
        case ShaderBuild::SurfaceFormat::Color32Float:
            texture.pixelFormat = MTLPixelFormatRGBA32Float;
            break;
        case ShaderBuild::SurfaceFormat::Depth32:
            texture.pixelFormat = MTLPixelFormatDepth32Float;
            break;
        default:
            texture.pixelFormat = MTLPixelFormatRGBA8Unorm;
            break;
        }
        surface.texture = [device newTextureWithDescriptor:texture];
        if (!surface.texture)
        {
            error = "Rezonality could not create Metal surface '"
                + source.name + "'";
            return std::nullopt;
        }
        if (has_image)
        {
            const MTLRegion region = MTLRegionMake2D(
                0, 0, source.image_width, source.image_height);
            const void* bytes = !source.image_float_pixels.empty()
                ? static_cast<const void*>(source.image_float_pixels.data())
                : static_cast<const void*>(source.image_pixels.data());
            const size_t bytes_per_pixel
                = !source.image_float_pixels.empty() ? 16 : 4;
            [surface.texture replaceRegion:region
                               mipmapLevel:0
                                 withBytes:bytes
                               bytesPerRow:source.image_width * bytes_per_pixel];
        }
        generation.surfaces.push_back(surface);
    }
    for (const auto& source_model : build.models)
    {
        auto model = create_metal_model(device, source_model, error);
        if (!model)
            return std::nullopt;
        generation.models.push_back(std::move(*model));
    }
    const auto find_surface = [&generation](std::string_view name)
        -> std::optional<size_t> {
        for (size_t index = 0; index < generation.surfaces.size(); ++index)
            if (generation.surfaces[index].name == name)
                return index;
        return std::nullopt;
    };
    for (size_t index = 0; index < build.passes.size(); ++index)
    {
        const auto& source = build.passes[index];
        MetalGeneration::Pass pass;
        pass.model_index = source.model_index;
        pass.ray_trace = source.ray_trace;
        pass.has_clear = source.has_clear;
        std::copy(std::begin(source.clear), std::end(source.clear),
            std::begin(pass.clear));
        for (const auto& target_name : source.targets)
        {
            if (target_name == "default_color")
            {
                pass.direct = true;
                continue;
            }
            if (target_name == "default_depth")
                continue;
            const auto surface = find_surface(target_name);
            if (!surface)
            {
                error = "Pass '" + source.name
                    + "' references unknown target '" + target_name + "'";
                return std::nullopt;
            }
            pass.targets.push_back(*surface);
        }
        for (const auto& sampler : source.samplers)
        {
            const auto surface = find_surface(sampler.surface);
            if (!surface)
            {
                error = "Pass '" + source.name
                    + "' references unknown sampler '" + sampler.surface + "'";
                return std::nullopt;
            }
            pass.samplers.push_back(*surface);
        }
        if (source.ray_trace)
        {
            if (!source.model_index
                || *source.model_index >= generation.models.size()
                || pass.targets.size() != 1)
            {
                error = "Metal ray pass '" + source.name
                    + "' requires one model and one target";
                return std::nullopt;
            }
            auto& model = generation.models[*source.model_index];
            if (!model.blas
                && !create_metal_acceleration_resources(device,
                    build.models[*source.model_index], model, error))
                return std::nullopt;
            NSError* compile_error = nil;
            id<MTLLibrary> library = [device
                newLibraryWithSource:[NSString stringWithUTF8String:
                                                   source.metal_ray_source.c_str()]
                             options:nil
                               error:&compile_error];
            id<MTLFunction> function = library
                ? [library newFunctionWithName:@"vklive_ray_trace"]
                : nil;
            pass.ray_pipeline = function
                ? [device newComputePipelineStateWithFunction:function
                                                        error:&compile_error]
                : nil;
            if (!pass.ray_pipeline)
            {
                error = "Metal ray shader failed for pass '" + source.name
                    + "': " + ns_error(compile_error);
                return std::nullopt;
            }
            generation.passes.push_back(std::move(pass));
            continue;
        }
        const std::string vertex_entry
            = "rezonality_vertex_" + std::to_string(index);
        const std::string fragment_entry
            = "rezonality_fragment_" + std::to_string(index);
        std::string vertex_source;
        std::string fragment_source;
        if (!convert_to_msl(source.vertex_spirv, spv::ExecutionModelVertex,
                vertex_entry.c_str(), vertex_source, error)
            || !convert_to_msl(source.fragment_spirv,
                spv::ExecutionModelFragment, fragment_entry.c_str(),
                fragment_source, error))
            return std::nullopt;
        NSError* compile_error = nil;
        id<MTLLibrary> vertex_library = [device
            newLibraryWithSource:[NSString stringWithUTF8String:vertex_source.c_str()]
                         options:nil
                           error:&compile_error];
        id<MTLLibrary> fragment_library = [device
            newLibraryWithSource:[NSString stringWithUTF8String:fragment_source.c_str()]
                         options:nil
                           error:&compile_error];
        if (!vertex_library || !fragment_library)
        {
            error = "Metal shader compilation failed for pass '"
                + source.name + "': " + ns_error(compile_error);
            return std::nullopt;
        }
        id<MTLFunction> vertex = [vertex_library
            newFunctionWithName:[NSString stringWithUTF8String:vertex_entry.c_str()]];
        id<MTLFunction> fragment = [fragment_library
            newFunctionWithName:[NSString stringWithUTF8String:fragment_entry.c_str()]];
        MTLRenderPipelineDescriptor* descriptor
            = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        descriptor.vertexDescriptor = source.model_index
            ? model_vertices
            : vertices;
        if (pass.direct)
            descriptor.colorAttachments[0].pixelFormat = target.pixelFormat;
        else
        {
            size_t color_index = 0;
            for (size_t surface_index : pass.targets)
            {
                const auto& surface = generation.surfaces[surface_index];
                if (surface.depth)
                    descriptor.depthAttachmentPixelFormat
                        = surface.texture.pixelFormat;
                else
                    descriptor.colorAttachments[color_index++].pixelFormat
                        = surface.texture.pixelFormat;
            }
        }
        id<MTLRenderPipelineState> pipeline = [device
            newRenderPipelineStateWithDescriptor:descriptor
                                           error:&compile_error];
        if (!pipeline)
        {
            error = "Rezonality Metal pipeline failed for pass '"
                + source.name + "': " + ns_error(compile_error);
            return std::nullopt;
        }
        pass.pipeline = pipeline;
        generation.passes.push_back(std::move(pass));
    }
    generation.format = target.pixelFormat;
    generation.source_generation = build.generation;
    return generation;
}

#else

struct VulkanSurfaceResource
{
    std::string name;
    draxul::vkresources::AttachmentResource attachment;
    draxul::vkresources::BufferResource upload_buffer;
    std::vector<draxul::vkresources::BufferResource> audio_upload_buffers;
    VkSampler sampler = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0;
    uint32_t height = 0;
    bool initialized = false;
    bool audio_analysis = false;
    uint64_t audio_generation = 0;
};

struct VulkanModelTextureResource
{
    draxul::vkresources::AttachmentResource attachment;
    draxul::vkresources::BufferResource upload_buffer;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    bool initialized = false;
};

struct VulkanModelResource
{
    draxul::vkresources::BufferResource vertex_buffer;
    draxul::vkresources::BufferResource index_buffer;
    draxul::vkresources::BufferResource material_buffer;
    std::array<std::vector<VulkanModelTextureResource>, 5> textures;
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    std::vector<rezonality::ModelPart> parts;
    draxul::vkresources::BufferResource blas_buffer;
    draxul::vkresources::BufferResource tlas_buffer;
    draxul::vkresources::BufferResource as_scratch_buffer;
    draxul::vkresources::BufferResource as_instance_buffer;
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;
    bool acceleration_structures_built = false;
};

struct VulkanPassResource
{
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkDescriptorSetLayout uniform_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout sampler_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet uniform_set = VK_NULL_HANDLE;
    VkDescriptorSet sampler_set = VK_NULL_HANDLE;
    VkDescriptorSetLayout ray_layout = VK_NULL_HANDLE;
    VkDescriptorSet ray_set = VK_NULL_HANDLE;
    draxul::vkresources::BufferResource shader_binding_table;
    VkStridedDeviceAddressRegionKHR raygen_region{};
    VkStridedDeviceAddressRegionKHR miss_region{};
    VkStridedDeviceAddressRegionKHR hit_region{};
    VkStridedDeviceAddressRegionKHR callable_region{};
    std::vector<size_t> target_surfaces;
    std::optional<size_t> model_index;
    bool ray_trace = false;
    bool direct = false;
    bool has_clear = false;
    float clear[4] = { 0, 0, 0, 1 };
};

struct VulkanGeneration
{
    struct RayFunctions
    {
        PFN_vkCreateAccelerationStructureKHR create_acceleration_structure = nullptr;
        PFN_vkDestroyAccelerationStructureKHR destroy_acceleration_structure = nullptr;
        PFN_vkGetAccelerationStructureBuildSizesKHR get_build_sizes = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR get_acceleration_address = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR cmd_build = nullptr;
        PFN_vkCreateRayTracingPipelinesKHR create_pipeline = nullptr;
        PFN_vkGetRayTracingShaderGroupHandlesKHR get_group_handles = nullptr;
        PFN_vkCmdTraceRaysKHR cmd_trace = nullptr;
    } ray;
    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    draxul::vkresources::BufferResource uniform_buffer;
    size_t uniform_stride = 0;
    uint32_t buffered_frame_count = 1;
    std::vector<VulkanSurfaceResource> surfaces;
    std::vector<VulkanModelResource> models;
    std::vector<VulkanPassResource> passes;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t target_generation = 0;
    uint64_t source_generation = 0;
    uint64_t used_slots = 0;
    bool ray_project = false;
};

struct RetiredGeneration
{
    VulkanGeneration generation;
    uint64_t pending_slots = 0;
};

struct BackendState
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkBuffer vertex_buffer = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    std::optional<VulkanGeneration> active;
    std::vector<RetiredGeneration> retired;
};

void destroy_generation(VulkanGeneration& generation)
{
    for (auto& pass : generation.passes)
    {
        if (pass.pipeline)
            vkDestroyPipeline(generation.device, pass.pipeline, nullptr);
        if (pass.layout)
            vkDestroyPipelineLayout(generation.device, pass.layout, nullptr);
        if (pass.framebuffer)
            vkDestroyFramebuffer(generation.device, pass.framebuffer, nullptr);
        if (pass.render_pass && !pass.direct)
            vkDestroyRenderPass(generation.device, pass.render_pass, nullptr);
        if (pass.descriptor_pool)
            vkDestroyDescriptorPool(generation.device, pass.descriptor_pool, nullptr);
        if (pass.uniform_layout)
            vkDestroyDescriptorSetLayout(generation.device, pass.uniform_layout, nullptr);
        if (pass.sampler_layout)
            vkDestroyDescriptorSetLayout(generation.device, pass.sampler_layout, nullptr);
        if (pass.ray_layout)
            vkDestroyDescriptorSetLayout(
                generation.device, pass.ray_layout, nullptr);
        draxul::vkresources::destroy_buffer(
            generation.allocator, pass.shader_binding_table);
    }
    for (auto& surface : generation.surfaces)
    {
        if (surface.sampler)
            vkDestroySampler(generation.device, surface.sampler, nullptr);
        draxul::vkresources::destroy_attachment(
            generation.device, generation.allocator, surface.attachment);
        draxul::vkresources::destroy_buffer(
            generation.allocator, surface.upload_buffer);
        for (auto& buffer : surface.audio_upload_buffers)
            draxul::vkresources::destroy_buffer(
                generation.allocator, buffer);
    }
    for (auto& model : generation.models)
    {
        if (model.tlas && generation.ray.destroy_acceleration_structure)
            generation.ray.destroy_acceleration_structure(
                generation.device, model.tlas, nullptr);
        if (model.blas && generation.ray.destroy_acceleration_structure)
            generation.ray.destroy_acceleration_structure(
                generation.device, model.blas, nullptr);
        if (model.descriptor_pool)
            vkDestroyDescriptorPool(
                generation.device, model.descriptor_pool, nullptr);
        if (model.descriptor_layout)
            vkDestroyDescriptorSetLayout(
                generation.device, model.descriptor_layout, nullptr);
        for (auto& slots : model.textures)
            for (auto& texture : slots)
            {
                if (texture.sampler)
                    vkDestroySampler(
                        generation.device, texture.sampler, nullptr);
                draxul::vkresources::destroy_attachment(generation.device,
                    generation.allocator, texture.attachment);
                draxul::vkresources::destroy_buffer(
                    generation.allocator, texture.upload_buffer);
            }
        draxul::vkresources::destroy_buffer(
            generation.allocator, model.vertex_buffer);
        draxul::vkresources::destroy_buffer(
            generation.allocator, model.index_buffer);
        draxul::vkresources::destroy_buffer(
            generation.allocator, model.material_buffer);
        draxul::vkresources::destroy_buffer(
            generation.allocator, model.as_instance_buffer);
        draxul::vkresources::destroy_buffer(
            generation.allocator, model.as_scratch_buffer);
        draxul::vkresources::destroy_buffer(
            generation.allocator, model.tlas_buffer);
        draxul::vkresources::destroy_buffer(
            generation.allocator, model.blas_buffer);
    }
    draxul::vkresources::destroy_buffer(
        generation.allocator, generation.uniform_buffer);
    generation = {};
}

void destroy_backend(BackendState& backend)
{
    if (backend.active)
        destroy_generation(*backend.active);
    for (auto& retired : backend.retired)
        destroy_generation(retired.generation);
    if (backend.vertex_buffer)
        vkDestroyBuffer(backend.device, backend.vertex_buffer, nullptr);
    if (backend.vertex_memory)
        vkFreeMemory(backend.device, backend.vertex_memory, nullptr);
    draxul::plugin_support::destroy_allocator(backend.allocator);
    backend = {};
}

uint32_t find_memory_type(VkPhysicalDevice physical_device,
    uint32_t allowed_types, VkMemoryPropertyFlags required)
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
    {
        if ((allowed_types & (uint32_t{ 1 } << index))
            && (properties.memoryTypes[index].propertyFlags & required)
                == required)
            return index;
    }
    return UINT32_MAX;
}

bool ensure_vertex_buffer(BackendState& backend,
    const DraxulPluginVulkanFrameV2& frame, std::string& error)
{
    const auto device = static_cast<VkDevice>(frame.device);
    const auto physical
        = static_cast<VkPhysicalDevice>(frame.physical_device);
    if (backend.vertex_buffer && backend.device == device)
        return true;
    if (backend.device && backend.device != device)
        destroy_backend(backend);
    backend.device = device;
    backend.physical_device = physical;

    VkBufferCreateInfo buffer_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    buffer_info.size = sizeof(kScreenVertices);
    buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &buffer_info, nullptr,
            &backend.vertex_buffer)
        != VK_SUCCESS)
    {
        error = "Rezonality could not create its Vulkan screen rectangle";
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(
        device, backend.vertex_buffer, &requirements);
    const uint32_t memory_type = find_memory_type(physical,
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory_type == UINT32_MAX)
    {
        error = "Rezonality could not find host-visible vertex memory";
        return false;
    }
    VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device, &allocation, nullptr,
            &backend.vertex_memory)
            != VK_SUCCESS
        || vkBindBufferMemory(device, backend.vertex_buffer,
               backend.vertex_memory, 0)
            != VK_SUCCESS)
    {
        error = "Rezonality could not allocate vertex memory";
        return false;
    }
    void* mapped = nullptr;
    if (vkMapMemory(device, backend.vertex_memory, 0,
            sizeof(kScreenVertices), 0, &mapped)
        != VK_SUCCESS)
    {
        error = "Rezonality could not map vertex memory";
        return false;
    }
    std::memcpy(mapped, kScreenVertices, sizeof(kScreenVertices));
    vkUnmapMemory(device, backend.vertex_memory);
    return true;
}

VkShaderModule create_shader(VkDevice device,
    const std::vector<uint32_t>& words)
{
    VkShaderModuleCreateInfo create_info{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
    };
    create_info.codeSize = words.size() * sizeof(uint32_t);
    create_info.pCode = words.data();
    VkShaderModule shader = VK_NULL_HANDLE;
    return vkCreateShaderModule(device, &create_info, nullptr, &shader)
            == VK_SUCCESS
        ? shader
        : VK_NULL_HANDLE;
}

VkFormat surface_format(ShaderBuild::SurfaceFormat format)
{
    switch (format)
    {
    case ShaderBuild::SurfaceFormat::Color16Float:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case ShaderBuild::SurfaceFormat::Color32Float:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case ShaderBuild::SurfaceFormat::Depth32:
        return VK_FORMAT_D32_SFLOAT;
    default:
        return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

std::optional<size_t> find_surface(
    const VulkanGeneration& generation, std::string_view name)
{
    for (size_t index = 0; index < generation.surfaces.size(); ++index)
        if (generation.surfaces[index].name == name)
            return index;
    return std::nullopt;
}

bool create_surface(VulkanGeneration& generation,
    const ShaderBuild::Surface& source, uint32_t pane_width,
    uint32_t pane_height, std::string& error)
{
    VulkanSurfaceResource surface;
    surface.name = source.name;
    surface.audio_analysis = source.audio_analysis;
    surface.format = surface_format(source.format);
    surface.aspect = source.format == ShaderBuild::SurfaceFormat::Depth32
        ? VK_IMAGE_ASPECT_DEPTH_BIT
        : VK_IMAGE_ASPECT_COLOR_BIT;
    surface.width = source.image_width != 0 ? source.image_width
                                            : std::max(1u, static_cast<uint32_t>(pane_width * std::max(0.01f, source.scale_x)));
    surface.height = source.image_height != 0 ? source.image_height
                                              : std::max(1u, static_cast<uint32_t>(pane_height * std::max(0.01f, source.scale_y)));
    const bool depth = surface.aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
    const VkImageUsageFlags usage = depth
        ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
        : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
            | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
            | (generation.ray_project ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    const draxul::vkresources::AttachmentRequest request(
        static_cast<int>(surface.width), static_cast<int>(surface.height),
        surface.format, usage, surface.aspect, VK_SAMPLE_COUNT_1_BIT, 0,
        depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
              : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        draxul::vkresources::LifetimeScope::Persistent,
        "rezonality-" + source.name);
    if (!draxul::vkresources::create_attachment(generation.device,
            generation.allocator, request, surface.attachment, error))
        return false;
    if (!depth)
    {
        VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        const VkSamplerAddressMode address_mode
            = source.audio_analysis
                || (source.image_pixels.empty()
                    && source.image_float_pixels.empty())
            ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
            : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeU = address_mode;
        sampler.addressModeV = address_mode;
        sampler.addressModeW = address_mode;
        sampler.maxLod = 1.0f;
        if (vkCreateSampler(generation.device, &sampler, nullptr,
                &surface.sampler)
            != VK_SUCCESS)
        {
            draxul::vkresources::destroy_attachment(generation.device,
                generation.allocator, surface.attachment);
            error = "Rezonality could not create sampler for surface '"
                + source.name + "'";
            return false;
        }
    }
    if (source.audio_analysis)
    {
        const size_t byte_size = static_cast<size_t>(surface.width)
            * surface.height * 4 * sizeof(float);
        surface.audio_upload_buffers.reserve(
            generation.buffered_frame_count);
        for (uint32_t index = 0; index < generation.buffered_frame_count;
             ++index)
        {
            draxul::vkresources::ScopedBuffer upload;
            const draxul::vkresources::BufferRequest upload_request(
                byte_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                draxul::vkresources::MemoryPolicy::HostSequentialWrite,
                "rezonality-audio-upload",
                draxul::vkresources::LifetimeScope::Persistent);
            if (!draxul::vkresources::create_buffer(generation.device,
                    generation.allocator, upload_request, upload, error))
            {
                for (auto& buffer : surface.audio_upload_buffers)
                    draxul::vkresources::destroy_buffer(
                        generation.allocator, buffer);
                if (surface.sampler)
                    vkDestroySampler(
                        generation.device, surface.sampler, nullptr);
                draxul::vkresources::destroy_attachment(generation.device,
                    generation.allocator, surface.attachment);
                return false;
            }
            surface.audio_upload_buffers.push_back(upload.release());
        }
    }
    else if (!source.image_pixels.empty()
        || !source.image_float_pixels.empty())
    {
        const size_t byte_size = !source.image_float_pixels.empty()
            ? source.image_float_pixels.size() * sizeof(float)
            : source.image_pixels.size();
        const void* bytes = !source.image_float_pixels.empty()
            ? static_cast<const void*>(source.image_float_pixels.data())
            : static_cast<const void*>(source.image_pixels.data());
        draxul::vkresources::ScopedBuffer upload;
        const draxul::vkresources::BufferRequest upload_request(
            byte_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            draxul::vkresources::MemoryPolicy::HostSequentialWrite,
            "rezonality-texture-upload",
            draxul::vkresources::LifetimeScope::Persistent);
        if (!draxul::vkresources::create_buffer(generation.device,
                generation.allocator, upload_request, upload, error))
        {
            if (surface.sampler)
                vkDestroySampler(
                    generation.device, surface.sampler, nullptr);
            draxul::vkresources::destroy_attachment(generation.device,
                generation.allocator, surface.attachment);
            return false;
        }
        surface.upload_buffer = upload.release();
        std::memcpy(surface.upload_buffer.mapped, bytes, byte_size);
        vmaFlushAllocation(generation.allocator,
            surface.upload_buffer.allocation, 0, byte_size);
    }
    generation.surfaces.push_back(std::move(surface));
    return true;
}

bool create_model_texture(VulkanGeneration& generation,
    const rezonality::ModelTexture& source,
    VulkanModelTextureResource& texture, std::string& error)
{
    texture.width = source.width;
    texture.height = source.height;
    const VkFormat format = source.srgb
        ? VK_FORMAT_R8G8B8A8_SRGB
        : VK_FORMAT_R8G8B8A8_UNORM;
    const draxul::vkresources::AttachmentRequest request(
        static_cast<int>(source.width), static_cast<int>(source.height),
        format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 0,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        draxul::vkresources::LifetimeScope::Persistent,
        "rezonality-model-texture");
    if (!draxul::vkresources::create_attachment(generation.device,
            generation.allocator, request, texture.attachment, error))
        return false;
    VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.maxLod = 1.0f;
    if (vkCreateSampler(generation.device, &sampler, nullptr,
            &texture.sampler)
        != VK_SUCCESS)
    {
        error = "Rezonality could not create a model texture sampler";
        return false;
    }
    draxul::vkresources::ScopedBuffer upload;
    const draxul::vkresources::BufferRequest upload_request(
        source.pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        draxul::vkresources::MemoryPolicy::HostSequentialWrite,
        "rezonality-model-texture-upload",
        draxul::vkresources::LifetimeScope::Persistent);
    if (!draxul::vkresources::create_buffer(generation.device,
            generation.allocator, upload_request, upload, error))
        return false;
    texture.upload_buffer = upload.release();
    std::memcpy(texture.upload_buffer.mapped,
        source.pixels.data(), source.pixels.size());
    vmaFlushAllocation(generation.allocator,
        texture.upload_buffer.allocation, 0, source.pixels.size());
    return true;
}

VkDeviceAddress buffer_address(
    VkDevice device, VkBuffer buffer)
{
    VkBufferDeviceAddressInfo info{
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO
    };
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(device, &info);
}

bool create_owned_buffer(VulkanGeneration& generation, VkDeviceSize size,
    VkBufferUsageFlags usage,
    draxul::vkresources::MemoryPolicy memory,
    std::string_view name, const void* data,
    draxul::vkresources::BufferResource& output, std::string& error)
{
    draxul::vkresources::ScopedBuffer scoped;
    const draxul::vkresources::BufferRequest request(size, usage, memory,
        std::string(name),
        draxul::vkresources::LifetimeScope::Persistent);
    if (!draxul::vkresources::create_buffer(generation.device,
            generation.allocator, request, scoped, error))
        return false;
    output = scoped.release();
    if (data)
    {
        if (!output.mapped)
        {
            error = "Rezonality GPU upload buffer was not mapped";
            return false;
        }
        std::memcpy(output.mapped, data, static_cast<size_t>(size));
        vmaFlushAllocation(
            generation.allocator, output.allocation, 0, size);
    }
    return true;
}

bool create_acceleration_structure(VulkanGeneration& generation,
    VkAccelerationStructureTypeKHR type, VkDeviceSize size,
    std::string_view name,
    draxul::vkresources::BufferResource& buffer,
    VkAccelerationStructureKHR& acceleration_structure,
    std::string& error)
{
    if (!create_owned_buffer(generation, size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            draxul::vkresources::MemoryPolicy::DevicePreferred,
            name, nullptr, buffer, error))
        return false;
    VkAccelerationStructureCreateInfoKHR create_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR
    };
    create_info.buffer = buffer.buffer;
    create_info.size = size;
    create_info.type = type;
    if (generation.ray.create_acceleration_structure(
            generation.device, &create_info, nullptr,
            &acceleration_structure)
        != VK_SUCCESS)
    {
        error = "Rezonality could not create a Vulkan acceleration structure";
        return false;
    }
    return true;
}

bool create_model_acceleration_resources(VulkanGeneration& generation,
    const rezonality::ModelData& source, VulkanModelResource& model,
    std::string& error)
{
    model.vertex_count = static_cast<uint32_t>(source.vertices.size());
    model.index_count = static_cast<uint32_t>(source.indices.size());
    const VkDeviceAddress vertex_address
        = buffer_address(generation.device, model.vertex_buffer.buffer);
    const VkDeviceAddress index_address
        = buffer_address(generation.device, model.index_buffer.buffer);
    if (!vertex_address || !index_address)
    {
        error = "Rezonality could not address its Vulkan ray geometry";
        return false;
    }

    VkAccelerationStructureGeometryTrianglesDataKHR triangles{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR
    };
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = vertex_address;
    triangles.vertexStride = sizeof(rezonality::ModelVertex);
    triangles.maxVertex = model.vertex_count - 1;
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = index_address;
    VkAccelerationStructureGeometryKHR geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR
    };
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.triangles = triangles;
    VkAccelerationStructureBuildGeometryInfoKHR build_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
    };
    build_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build_info.flags
        = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build_info.geometryCount = 1;
    build_info.pGeometries = &geometry;
    const uint32_t primitive_count = model.index_count / 3;
    VkAccelerationStructureBuildSizesInfoKHR blas_sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
    };
    generation.ray.get_build_sizes(generation.device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build_info,
        &primitive_count, &blas_sizes);
    if (!create_acceleration_structure(generation,
            VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            blas_sizes.accelerationStructureSize, "rezonality-ray-blas",
            model.blas_buffer, model.blas, error))
        return false;

    VkAccelerationStructureDeviceAddressInfoKHR address_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR
    };
    address_info.accelerationStructure = model.blas;
    VkAccelerationStructureInstanceKHR instance{};
    instance.transform.matrix[0][0] = 1.0f;
    instance.transform.matrix[1][1] = 1.0f;
    instance.transform.matrix[2][2] = 1.0f;
    instance.mask = 0xff;
    instance.flags
        = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference
        = generation.ray.get_acceleration_address(
            generation.device, &address_info);
    if (!create_owned_buffer(generation, sizeof(instance),
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            draxul::vkresources::MemoryPolicy::HostSequentialWrite,
            "rezonality-ray-instance", &instance,
            model.as_instance_buffer, error))
        return false;

    VkAccelerationStructureGeometryInstancesDataKHR instances{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR
    };
    instances.data.deviceAddress = buffer_address(
        generation.device, model.as_instance_buffer.buffer);
    VkAccelerationStructureGeometryKHR tlas_geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR
    };
    tlas_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlas_geometry.geometry.instances = instances;
    VkAccelerationStructureBuildGeometryInfoKHR tlas_build{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
    };
    tlas_build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlas_build.flags
        = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlas_build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlas_build.geometryCount = 1;
    tlas_build.pGeometries = &tlas_geometry;
    constexpr uint32_t instance_count = 1;
    VkAccelerationStructureBuildSizesInfoKHR tlas_sizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
    };
    generation.ray.get_build_sizes(generation.device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlas_build,
        &instance_count, &tlas_sizes);
    if (!create_acceleration_structure(generation,
            VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
            tlas_sizes.accelerationStructureSize, "rezonality-ray-tlas",
            model.tlas_buffer, model.tlas, error))
        return false;
    return create_owned_buffer(generation,
        std::max(blas_sizes.buildScratchSize, tlas_sizes.buildScratchSize),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        draxul::vkresources::MemoryPolicy::DevicePreferred,
        "rezonality-ray-scratch", nullptr, model.as_scratch_buffer, error);
}

bool create_model(VulkanGeneration& generation,
    const rezonality::ModelData& source, VulkanModelResource& model,
    std::string& error)
{
    const auto create_buffer = [&](size_t size, VkBufferUsageFlags usage,
                                   const void* data,
                                   draxul::vkresources::BufferResource& out,
                                   std::string_view name) {
        draxul::vkresources::ScopedBuffer scoped;
        const draxul::vkresources::BufferRequest request(size, usage,
            draxul::vkresources::MemoryPolicy::HostSequentialWrite,
            std::string(name),
            draxul::vkresources::LifetimeScope::Persistent);
        if (!draxul::vkresources::create_buffer(generation.device,
                generation.allocator, request, scoped, error))
            return false;
        out = scoped.release();
        std::memcpy(out.mapped, data, size);
        vmaFlushAllocation(generation.allocator, out.allocation, 0, size);
        return true;
    };
    const VkBufferUsageFlags ray_vertex_usage = generation.ray_project
        ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
            | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
        : 0;
    if (!create_buffer(source.vertices.size()
                * sizeof(rezonality::ModelVertex),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                | ray_vertex_usage,
            source.vertices.data(),
            model.vertex_buffer, "rezonality-model-vertices")
        || !create_buffer(source.indices.size() * sizeof(uint32_t),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                | ray_vertex_usage,
            source.indices.data(),
            model.index_buffer, "rezonality-model-indices"))
        return false;

    struct alignas(16) GpuMaterial
    {
        glm::vec4 base_color;
        glm::vec4 emissive;
        glm::vec4 metallic_roughness_occlusion;
        glm::ivec4 texture_indices;
    };
    std::array<GpuMaterial, rezonality::kMaxModelMaterials> materials{};
    for (size_t index = 0; index < source.materials.size(); ++index)
    {
        const auto& material = source.materials[index];
        materials[index] = { material.base_color_factor,
            material.emissive_factor,
            { material.metallic_factor, material.roughness_factor,
                material.occlusion_strength, 0.0f },
            glm::ivec4(static_cast<int>(index)) };
    }
    if (!create_buffer(sizeof(materials), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            materials.data(), model.material_buffer,
            "rezonality-model-materials"))
        return false;

    for (size_t material_index = 0;
         material_index < source.materials.size(); ++material_index)
    {
        const auto& material = source.materials[material_index];
        const rezonality::ModelTexture* textures[] = {
            &material.base_color, &material.normal,
            &material.metallic_roughness, &material.emissive,
            &material.occlusion
        };
        for (size_t kind = 0; kind < std::size(textures); ++kind)
        {
            VulkanModelTextureResource texture;
            if (!create_model_texture(
                    generation, *textures[kind], texture, error))
                return false;
            model.textures[kind].push_back(std::move(texture));
        }
    }

    std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
    bindings[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
        VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    for (uint32_t binding = 1; binding < bindings.size(); ++binding)
        bindings[binding] = { binding,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            rezonality::kMaxModelMaterials,
            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
    layout_info.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(generation.device, &layout_info,
            nullptr, &model.descriptor_layout)
        != VK_SUCCESS)
        return false;
    const VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            rezonality::kMaxModelMaterials * 5 }
    };
    VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool.maxSets = 1;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(generation.device, &pool, nullptr,
            &model.descriptor_pool)
        != VK_SUCCESS)
        return false;
    VkDescriptorSetAllocateInfo allocation{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    allocation.descriptorPool = model.descriptor_pool;
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &model.descriptor_layout;
    if (vkAllocateDescriptorSets(generation.device, &allocation,
            &model.descriptor_set)
        != VK_SUCCESS)
        return false;
    VkDescriptorBufferInfo material_buffer{
        model.material_buffer.buffer, 0, sizeof(materials)
    };
    std::vector<std::array<VkDescriptorImageInfo,
        rezonality::kMaxModelMaterials>>
        image_arrays(5);
    std::array<VkWriteDescriptorSet, 6> writes{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    writes[0].dstSet = model.descriptor_set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &material_buffer;
    for (size_t kind = 0; kind < model.textures.size(); ++kind)
    {
        for (size_t index = 0; index < rezonality::kMaxModelMaterials;
             ++index)
        {
            const auto& texture = model.textures[kind][std::min(index, model.textures[kind].size() - 1)];
            image_arrays[kind][index] = { texture.sampler,
                texture.attachment.view,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        }
        writes[kind + 1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        writes[kind + 1].dstSet = model.descriptor_set;
        writes[kind + 1].dstBinding = static_cast<uint32_t>(kind + 1);
        writes[kind + 1].descriptorCount
            = rezonality::kMaxModelMaterials;
        writes[kind + 1].descriptorType
            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[kind + 1].pImageInfo = image_arrays[kind].data();
    }
    vkUpdateDescriptorSets(generation.device,
        static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    model.parts = source.parts;
    return !generation.ray_project
        || create_model_acceleration_resources(
            generation, source, model, error);
}

bool create_pass_render_target(VulkanGeneration& generation,
    const ShaderBuild::Pass& source, VulkanPassResource& pass,
    std::string& error)
{
    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> colors;
    std::optional<VkAttachmentReference> depth;
    std::vector<VkImageView> views;
    for (const std::string& target : source.targets)
    {
        if (target == "default_color" || target == "default_depth")
        {
            if (target == "default_color")
                pass.direct = true;
            continue;
        }
        const auto index = find_surface(generation, target);
        if (!index)
        {
            error = "Pass '" + source.name
                + "' references unknown target '" + target + "'";
            return false;
        }
        pass.target_surfaces.push_back(*index);
        const auto& surface = generation.surfaces[*index];
        VkAttachmentDescription attachment{};
        attachment.format = surface.format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = source.has_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                             : VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout
            = surface.aspect == VK_IMAGE_ASPECT_DEPTH_BIT
            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachment.finalLayout = attachment.initialLayout;
        const uint32_t attachment_index
            = static_cast<uint32_t>(attachments.size());
        attachments.push_back(attachment);
        views.push_back(surface.attachment.view);
        if (surface.aspect == VK_IMAGE_ASPECT_DEPTH_BIT)
            depth = VkAttachmentReference{ attachment_index,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        else
            colors.push_back({ attachment_index,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
    }
    if (pass.direct)
    {
        pass.render_pass = generation.render_pass;
        return true;
    }
    if (colors.empty())
    {
        error = "Pass '" + source.name + "' has no color target";
        return false;
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<uint32_t>(colors.size());
    subpass.pColorAttachments = colors.data();
    subpass.pDepthStencilAttachment = depth ? &*depth : nullptr;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo render_pass{
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO
    };
    render_pass.attachmentCount = static_cast<uint32_t>(attachments.size());
    render_pass.pAttachments = attachments.data();
    render_pass.subpassCount = 1;
    render_pass.pSubpasses = &subpass;
    render_pass.dependencyCount = 2;
    render_pass.pDependencies = dependencies;
    if (vkCreateRenderPass(generation.device, &render_pass, nullptr,
            &pass.render_pass)
        != VK_SUCCESS)
    {
        error = "Rezonality could not create render pass '" + source.name + "'";
        return false;
    }
    const auto& first = generation.surfaces[pass.target_surfaces.front()];
    VkFramebufferCreateInfo framebuffer{
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO
    };
    framebuffer.renderPass = pass.render_pass;
    framebuffer.attachmentCount = static_cast<uint32_t>(views.size());
    framebuffer.pAttachments = views.data();
    framebuffer.width = first.width;
    framebuffer.height = first.height;
    framebuffer.layers = 1;
    if (vkCreateFramebuffer(generation.device, &framebuffer, nullptr,
            &pass.framebuffer)
        != VK_SUCCESS)
    {
        error = "Rezonality could not create framebuffer '" + source.name + "'";
        return false;
    }
    return true;
}

bool create_pass_descriptors(VulkanGeneration& generation,
    const ShaderBuild::Pass& source, VulkanPassResource& pass,
    std::string& error)
{
    VkDescriptorSetLayoutBinding uniform_binding{};
    uniform_binding.binding = 0;
    uniform_binding.descriptorType
        = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    uniform_binding.descriptorCount = 1;
    uniform_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT
        | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo uniform_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    uniform_info.bindingCount = 1;
    uniform_info.pBindings = &uniform_binding;
    if (vkCreateDescriptorSetLayout(generation.device, &uniform_info,
            nullptr, &pass.uniform_layout)
        != VK_SUCCESS)
        return false;

    std::vector<VkDescriptorSetLayoutBinding> sampler_bindings;
    for (uint32_t index = 0; index < source.samplers.size(); ++index)
        sampler_bindings.push_back({ index,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });
    VkDescriptorSetLayoutCreateInfo sampler_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    sampler_info.bindingCount
        = static_cast<uint32_t>(sampler_bindings.size());
    sampler_info.pBindings = sampler_bindings.data();
    if (vkCreateDescriptorSetLayout(generation.device, &sampler_info,
            nullptr, &pass.sampler_layout)
        != VK_SUCCESS)
        return false;

    const VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            std::max(1u, static_cast<uint32_t>(source.samplers.size())) }
    };
    VkDescriptorPoolCreateInfo pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool.maxSets = 2;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(generation.device, &pool, nullptr,
            &pass.descriptor_pool)
        != VK_SUCCESS)
        return false;
    const VkDescriptorSetLayout layouts[]
        = { pass.uniform_layout, pass.sampler_layout };
    VkDescriptorSet sets[2]{};
    VkDescriptorSetAllocateInfo allocation{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    allocation.descriptorPool = pass.descriptor_pool;
    allocation.descriptorSetCount = 2;
    allocation.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(generation.device, &allocation, sets)
        != VK_SUCCESS)
        return false;
    pass.uniform_set = sets[0];
    pass.sampler_set = sets[1];
    VkDescriptorBufferInfo buffer{
        generation.uniform_buffer.buffer, 0, sizeof(CommonUniformBlock)
    };
    VkWriteDescriptorSet uniform_write{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
    };
    uniform_write.dstSet = pass.uniform_set;
    uniform_write.dstBinding = 0;
    uniform_write.descriptorCount = 1;
    uniform_write.descriptorType
        = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    uniform_write.pBufferInfo = &buffer;
    vkUpdateDescriptorSets(generation.device, 1, &uniform_write, 0, nullptr);

    std::vector<VkDescriptorImageInfo> images;
    std::vector<VkWriteDescriptorSet> writes;
    images.reserve(source.samplers.size());
    writes.reserve(source.samplers.size());
    for (uint32_t index = 0; index < source.samplers.size(); ++index)
    {
        const auto surface_index
            = find_surface(generation, source.samplers[index].surface);
        if (!surface_index
            || generation.surfaces[*surface_index].aspect
                != VK_IMAGE_ASPECT_COLOR_BIT)
        {
            error = "Pass '" + source.name + "' references unknown sampler '"
                + source.samplers[index].surface + "'";
            return false;
        }
        const auto& surface = generation.surfaces[*surface_index];
        images.push_back({ surface.sampler, surface.attachment.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = pass.sampler_set;
        write.dstBinding = index;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &images.back();
        writes.push_back(write);
    }
    if (!writes.empty())
        vkUpdateDescriptorSets(generation.device,
            static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    std::vector<VkDescriptorSetLayout> set_layouts
        = { pass.uniform_layout, pass.sampler_layout };
    if (source.model_index)
    {
        if (*source.model_index >= generation.models.size())
        {
            error = "Pass '" + source.name
                + "' references an invalid model";
            return false;
        }
        pass.model_index = source.model_index;
        set_layouts.push_back(
            generation.models[*source.model_index].descriptor_layout);
    }
    VkPipelineLayoutCreateInfo layout{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    layout.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout.pSetLayouts = set_layouts.data();
    VkPushConstantRange material_index{};
    if (source.model_index)
    {
        material_index.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        material_index.size = sizeof(uint32_t);
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &material_index;
    }
    if (vkCreatePipelineLayout(generation.device, &layout, nullptr,
            &pass.layout)
        != VK_SUCCESS)
    {
        error = "Rezonality could not create descriptors for pass '"
            + source.name + "'";
        return false;
    }
    return true;
}

bool load_ray_functions(VulkanGeneration& generation,
    VkPhysicalDevice physical_device, std::string& error)
{
    const auto load = [&generation](const char* name) {
        return vkGetDeviceProcAddr(generation.device, name);
    };
    generation.ray.create_acceleration_structure
        = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
            load("vkCreateAccelerationStructureKHR"));
    generation.ray.destroy_acceleration_structure
        = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
            load("vkDestroyAccelerationStructureKHR"));
    generation.ray.get_build_sizes
        = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
            load("vkGetAccelerationStructureBuildSizesKHR"));
    generation.ray.get_acceleration_address
        = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            load("vkGetAccelerationStructureDeviceAddressKHR"));
    generation.ray.cmd_build
        = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
            load("vkCmdBuildAccelerationStructuresKHR"));
    generation.ray.create_pipeline
        = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
            load("vkCreateRayTracingPipelinesKHR"));
    generation.ray.get_group_handles
        = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
            load("vkGetRayTracingShaderGroupHandlesKHR"));
    generation.ray.cmd_trace
        = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(
            load("vkCmdTraceRaysKHR"));
    if (!generation.ray.create_acceleration_structure
        || !generation.ray.destroy_acceleration_structure
        || !generation.ray.get_build_sizes
        || !generation.ray.get_acceleration_address
        || !generation.ray.cmd_build || !generation.ray.create_pipeline
        || !generation.ray.get_group_handles || !generation.ray.cmd_trace)
    {
        error = "Vulkan ray tracing is unsupported by this device";
        return false;
    }

    VkPhysicalDeviceBufferDeviceAddressFeatures address{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES
    };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR
    };
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR ray_pipeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR
    };
    address.pNext = &acceleration;
    acceleration.pNext = &ray_pipeline;
    VkPhysicalDeviceFeatures2 features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2
    };
    features.pNext = &address;
    vkGetPhysicalDeviceFeatures2(physical_device, &features);
    if (!address.bufferDeviceAddress || !acceleration.accelerationStructure
        || !ray_pipeline.rayTracingPipeline)
    {
        error = "Vulkan ray tracing is unsupported by this device";
        return false;
    }
    return true;
}

bool create_ray_pass(VulkanGeneration& generation,
    const ShaderBuild::Pass& source, VulkanPassResource& pass,
    VkPhysicalDevice physical_device, std::string& error)
{
    if (!source.model_index
        || *source.model_index >= generation.models.size())
    {
        error = "Ray pass '" + source.name + "' has no valid model";
        return false;
    }
    if (source.targets.size() != 1)
    {
        error = "Ray pass '" + source.name
            + "' must have exactly one storage-image target";
        return false;
    }
    const auto target = find_surface(generation, source.targets.front());
    if (!target
        || generation.surfaces[*target].aspect != VK_IMAGE_ASPECT_COLOR_BIT)
    {
        error = "Ray pass '" + source.name
            + "' references an invalid storage-image target";
        return false;
    }
    pass.ray_trace = true;
    pass.model_index = source.model_index;
    pass.target_surfaces.push_back(*target);

    VkDescriptorSetLayoutBinding uniform_binding{};
    uniform_binding.binding = 0;
    uniform_binding.descriptorType
        = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    uniform_binding.descriptorCount = 1;
    uniform_binding.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR
        | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    VkDescriptorSetLayoutCreateInfo uniform_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    uniform_info.bindingCount = 1;
    uniform_info.pBindings = &uniform_binding;
    if (vkCreateDescriptorSetLayout(generation.device, &uniform_info,
            nullptr, &pass.uniform_layout)
        != VK_SUCCESS)
        return false;
    const std::array ray_bindings{
        VkDescriptorSetLayoutBinding{ 0,
            VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
            VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr },
        VkDescriptorSetLayoutBinding{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
            VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr },
        VkDescriptorSetLayoutBinding{ 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr },
        VkDescriptorSetLayoutBinding{ 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr }
    };
    VkDescriptorSetLayoutCreateInfo ray_layout{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    ray_layout.bindingCount = static_cast<uint32_t>(ray_bindings.size());
    ray_layout.pBindings = ray_bindings.data();
    if (vkCreateDescriptorSetLayout(generation.device, &ray_layout,
            nullptr, &pass.ray_layout)
        != VK_SUCCESS)
        return false;
    const std::array pool_sizes{
        VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 },
        VkDescriptorPoolSize{
            VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 },
        VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
        VkDescriptorPoolSize{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 }
    };
    VkDescriptorPoolCreateInfo pool{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
    };
    pool.maxSets = 2;
    pool.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool.pPoolSizes = pool_sizes.data();
    if (vkCreateDescriptorPool(generation.device, &pool, nullptr,
            &pass.descriptor_pool)
        != VK_SUCCESS)
        return false;
    const VkDescriptorSetLayout layouts[]
        = { pass.uniform_layout, pass.ray_layout };
    VkDescriptorSet sets[2]{};
    VkDescriptorSetAllocateInfo allocation{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    allocation.descriptorPool = pass.descriptor_pool;
    allocation.descriptorSetCount = 2;
    allocation.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(generation.device, &allocation, sets)
        != VK_SUCCESS)
        return false;
    pass.uniform_set = sets[0];
    pass.ray_set = sets[1];
    VkDescriptorBufferInfo uniform_buffer{
        generation.uniform_buffer.buffer, 0, sizeof(CommonUniformBlock)
    };
    VkWriteDescriptorSet uniform_write{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
    };
    uniform_write.dstSet = pass.uniform_set;
    uniform_write.descriptorCount = 1;
    uniform_write.descriptorType
        = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    uniform_write.pBufferInfo = &uniform_buffer;
    vkUpdateDescriptorSets(
        generation.device, 1, &uniform_write, 0, nullptr);

    auto& model = generation.models[*source.model_index];
    VkWriteDescriptorSetAccelerationStructureKHR acceleration_write{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
    };
    acceleration_write.accelerationStructureCount = 1;
    acceleration_write.pAccelerationStructures = &model.tlas;
    VkDescriptorImageInfo target_image{ VK_NULL_HANDLE,
        generation.surfaces[*target].attachment.view,
        VK_IMAGE_LAYOUT_GENERAL };
    const VkDescriptorBufferInfo vertex_buffer{
        model.vertex_buffer.buffer, 0, VK_WHOLE_SIZE
    };
    const VkDescriptorBufferInfo index_buffer{
        model.index_buffer.buffer, 0, VK_WHOLE_SIZE
    };
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (auto& write : writes)
    {
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = pass.ray_set;
        write.descriptorCount = 1;
    }
    writes[0].pNext = &acceleration_write;
    writes[0].dstBinding = 0;
    writes[0].descriptorType
        = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &target_image;
    writes[2].dstBinding = 2;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &vertex_buffer;
    writes[3].dstBinding = 3;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &index_buffer;
    vkUpdateDescriptorSets(generation.device,
        static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    VkPipelineLayoutCreateInfo pipeline_layout{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    pipeline_layout.setLayoutCount = 2;
    pipeline_layout.pSetLayouts = layouts;
    if (vkCreatePipelineLayout(generation.device, &pipeline_layout,
            nullptr, &pass.layout)
        != VK_SUCCESS)
        return false;

    const VkShaderModule raygen
        = create_shader(generation.device, source.raygen_spirv);
    const VkShaderModule miss
        = create_shader(generation.device, source.miss_spirv);
    const VkShaderModule closest
        = create_shader(generation.device, source.closest_hit_spirv);
    if (!raygen || !miss || !closest)
    {
        if (raygen)
            vkDestroyShaderModule(generation.device, raygen, nullptr);
        if (miss)
            vkDestroyShaderModule(generation.device, miss, nullptr);
        if (closest)
            vkDestroyShaderModule(generation.device, closest, nullptr);
        error = "Rezonality could not create ray shaders for pass '"
            + source.name + "'";
        return false;
    }
    const std::array stages{
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_RAYGEN_BIT_KHR, raygen, "main", nullptr },
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_MISS_BIT_KHR, miss, "main", nullptr },
        VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, closest, "main", nullptr }
    };
    std::array<VkRayTracingShaderGroupCreateInfoKHR, 3> groups{};
    for (auto& group : groups)
    {
        group.sType
            = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        group.generalShader = VK_SHADER_UNUSED_KHR;
        group.closestHitShader = VK_SHADER_UNUSED_KHR;
        group.anyHitShader = VK_SHADER_UNUSED_KHR;
        group.intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].closestHitShader = 2;
    VkRayTracingPipelineCreateInfoKHR pipeline{
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR
    };
    pipeline.stageCount = static_cast<uint32_t>(stages.size());
    pipeline.pStages = stages.data();
    pipeline.groupCount = static_cast<uint32_t>(groups.size());
    pipeline.pGroups = groups.data();
    pipeline.maxPipelineRayRecursionDepth = 1;
    pipeline.layout = pass.layout;
    const VkResult pipeline_result = generation.ray.create_pipeline(
        generation.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipeline,
        nullptr, &pass.pipeline);
    vkDestroyShaderModule(generation.device, raygen, nullptr);
    vkDestroyShaderModule(generation.device, miss, nullptr);
    vkDestroyShaderModule(generation.device, closest, nullptr);
    if (pipeline_result != VK_SUCCESS)
    {
        error = "Rezonality could not create Vulkan ray pipeline for pass '"
            + source.name + "'";
        return false;
    }

    VkPhysicalDeviceRayTracingPipelinePropertiesKHR properties{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR
    };
    VkPhysicalDeviceProperties2 properties2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
    };
    properties2.pNext = &properties;
    vkGetPhysicalDeviceProperties2(physical_device, &properties2);
    const size_t handle_size = properties.shaderGroupHandleSize;
    const size_t handle_stride
        = align_up(handle_size, properties.shaderGroupHandleAlignment);
    const size_t section_stride
        = align_up(handle_stride, properties.shaderGroupBaseAlignment);
    std::vector<uint8_t> handles(handle_size * groups.size());
    if (generation.ray.get_group_handles(generation.device, pass.pipeline,
            0, static_cast<uint32_t>(groups.size()), handles.size(),
            handles.data())
            != VK_SUCCESS
        || !create_owned_buffer(generation, section_stride * groups.size(),
            VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR
                | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            draxul::vkresources::MemoryPolicy::HostSequentialWrite,
            "rezonality-ray-sbt", nullptr,
            pass.shader_binding_table, error))
        return false;
    auto* mapped = static_cast<uint8_t*>(pass.shader_binding_table.mapped);
    std::memset(mapped, 0, section_stride * groups.size());
    for (size_t index = 0; index < groups.size(); ++index)
        std::memcpy(mapped + section_stride * index,
            handles.data() + handle_size * index, handle_size);
    vmaFlushAllocation(generation.allocator,
        pass.shader_binding_table.allocation, 0,
        section_stride * groups.size());
    const VkDeviceAddress sbt_address = buffer_address(
        generation.device, pass.shader_binding_table.buffer);
    pass.raygen_region
        = { sbt_address, handle_stride, handle_stride };
    pass.miss_region = { sbt_address + section_stride,
        handle_stride, handle_stride };
    pass.hit_region = { sbt_address + section_stride * 2,
        handle_stride, handle_stride };
    return true;
}

std::optional<VulkanGeneration> create_generation(BackendState& backend,
    const ShaderBuild& build, const DraxulPluginVulkanFrameV2& frame,
    double animation_seconds, const rezonality::Camera& camera,
    std::string& error)
{
    if (!ensure_vertex_buffer(backend, frame, error))
        return std::nullopt;
    if (!draxul::plugin_support::ensure_allocator(
            backend.allocator, frame, "Rezonality", error))
        return std::nullopt;
    VulkanGeneration generation;
    generation.device = static_cast<VkDevice>(frame.device);
    generation.allocator = backend.allocator;
    generation.render_pass = reinterpret_cast<VkRenderPass>(
        static_cast<uintptr_t>(frame.continuation_render_pass));
    generation.target_generation = frame.target_generation;
    generation.source_generation = build.generation;
    generation.ray_project = std::any_of(build.passes.begin(),
        build.passes.end(), [](const auto& pass) {
            return pass.ray_trace;
        });
    generation.width = static_cast<uint32_t>(std::max(1, frame.viewport.width));
    generation.height = static_cast<uint32_t>(std::max(1, frame.viewport.height));
    generation.buffered_frame_count
        = std::max(1u, frame.buffered_frame_count);
    if (generation.ray_project
        && !load_ray_functions(generation,
            static_cast<VkPhysicalDevice>(frame.physical_device), error))
        return std::nullopt;
    for (const auto& surface : build.surfaces)
        if (!create_surface(generation, surface, generation.width,
                generation.height, error))
        {
            destroy_generation(generation);
            return std::nullopt;
        }
    generation.models.resize(build.models.size());
    for (size_t index = 0; index < build.models.size(); ++index)
        if (!create_model(generation, build.models[index],
                generation.models[index], error))
        {
            destroy_generation(generation);
            return std::nullopt;
        }
    VkPhysicalDeviceProperties device_properties{};
    vkGetPhysicalDeviceProperties(
        static_cast<VkPhysicalDevice>(frame.physical_device),
        &device_properties);
    generation.uniform_stride = align_up(sizeof(CommonUniformBlock),
        static_cast<size_t>(device_properties.limits
                                .minUniformBufferOffsetAlignment));
    const auto uniform = make_common_uniforms(
        animation_seconds,
        generation.width, generation.height,
        frame.viewport.x, frame.viewport.y, camera);
    draxul::vkresources::ScopedBuffer uniform_buffer;
    const draxul::vkresources::BufferRequest uniform_request(
        generation.uniform_stride * generation.buffered_frame_count,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        draxul::vkresources::MemoryPolicy::HostSequentialWrite,
        "rezonality-common-uniform",
        draxul::vkresources::LifetimeScope::Persistent);
    if (!draxul::vkresources::create_buffer(generation.device,
            generation.allocator, uniform_request, uniform_buffer, error))
    {
        destroy_generation(generation);
        return std::nullopt;
    }
    generation.uniform_buffer = uniform_buffer.release();
    for (uint32_t slot = 0; slot < generation.buffered_frame_count; ++slot)
    {
        std::memcpy(static_cast<uint8_t*>(
                        generation.uniform_buffer.mapped)
                + generation.uniform_stride * slot,
            uniform.data(), sizeof(uniform));
    }
    vmaFlushAllocation(generation.allocator,
        generation.uniform_buffer.allocation, 0,
        generation.uniform_buffer.size);

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(ScreenVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attributes[4]{};
    attributes[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
        offsetof(ScreenVertex, position) };
    attributes[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT,
        offsetof(ScreenVertex, uv) };
    attributes[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT,
        offsetof(ScreenVertex, color) };
    attributes[3] = { 3, 0, VK_FORMAT_R32G32B32_SFLOAT,
        offsetof(ScreenVertex, normal) };
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 4;
    vertex_input.pVertexAttributeDescriptions = attributes;
    VkVertexInputBindingDescription model_binding{};
    model_binding.binding = 0;
    model_binding.stride = sizeof(rezonality::ModelVertex);
    model_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription model_attributes[6]{};
    model_attributes[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
        offsetof(rezonality::ModelVertex, position) };
    model_attributes[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT,
        offsetof(rezonality::ModelVertex, uv) };
    model_attributes[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT,
        offsetof(rezonality::ModelVertex, color) };
    model_attributes[3] = { 3, 0, VK_FORMAT_R32G32B32_SFLOAT,
        offsetof(rezonality::ModelVertex, normal) };
    model_attributes[4] = { 4, 0, VK_FORMAT_R32G32B32_SFLOAT,
        offsetof(rezonality::ModelVertex, tangent) };
    model_attributes[5] = { 5, 0, VK_FORMAT_R32G32B32_SFLOAT,
        offsetof(rezonality::ModelVertex, bitangent) };
    VkPipelineVertexInputStateCreateInfo model_vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
    };
    model_vertex_input.vertexBindingDescriptionCount = 1;
    model_vertex_input.pVertexBindingDescriptions = &model_binding;
    model_vertex_input.vertexAttributeDescriptionCount = 6;
    model_vertex_input.pVertexAttributeDescriptions = model_attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO
    };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO
    };
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO
    };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO
    };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_stencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
    };
    depth_stencil.depthTestEnable = VK_FALSE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    const VkDynamicState states[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO
    };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    for (const auto& source : build.passes)
    {
        VulkanPassResource pass;
        pass.has_clear = source.has_clear;
        std::copy(std::begin(source.clear), std::end(source.clear),
            std::begin(pass.clear));
        if (source.ray_trace)
        {
            if (!create_ray_pass(generation, source, pass,
                    static_cast<VkPhysicalDevice>(frame.physical_device),
                    error))
            {
                generation.passes.push_back(std::move(pass));
                destroy_generation(generation);
                return std::nullopt;
            }
            generation.passes.push_back(std::move(pass));
            continue;
        }
        if (!create_pass_render_target(
                generation, source, pass, error)
            || !create_pass_descriptors(generation, source, pass, error))
        {
            generation.passes.push_back(std::move(pass));
            destroy_generation(generation);
            return std::nullopt;
        }
        const VkShaderModule vertex
            = create_shader(generation.device, source.vertex_spirv);
        const VkShaderModule fragment
            = create_shader(generation.device, source.fragment_spirv);
        if (!vertex || !fragment)
        {
            if (vertex)
                vkDestroyShaderModule(generation.device, vertex, nullptr);
            if (fragment)
                vkDestroyShaderModule(generation.device, fragment, nullptr);
            generation.passes.push_back(std::move(pass));
            destroy_generation(generation);
            error = "Rezonality could not create Vulkan shaders for pass '"
                + source.name + "'";
            return std::nullopt;
        }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";
        uint32_t color_count = 1;
        bool has_depth = false;
        if (!pass.direct)
        {
            color_count = 0;
            for (size_t surface_index : pass.target_surfaces)
            {
                if (generation.surfaces[surface_index].aspect
                    == VK_IMAGE_ASPECT_DEPTH_BIT)
                    has_depth = true;
                else
                    ++color_count;
            }
        }
        std::vector<VkPipelineColorBlendAttachmentState> color_attachments(
            color_count);
        for (auto& attachment : color_attachments)
            attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT
                | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
                | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
        };
        blend.attachmentCount = color_count;
        blend.pAttachments = color_attachments.data();
        const bool model_depth = has_depth && source.model_index.has_value();
        depth_stencil.depthTestEnable = model_depth ? VK_TRUE : VK_FALSE;
        depth_stencil.depthWriteEnable = model_depth ? VK_TRUE : VK_FALSE;
        depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkGraphicsPipelineCreateInfo pipeline_info{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO
        };
        pipeline_info.stageCount = 2;
        pipeline_info.pStages = stages;
        pipeline_info.pVertexInputState = source.model_index
            ? &model_vertex_input
            : &vertex_input;
        pipeline_info.pInputAssemblyState = &assembly;
        pipeline_info.pViewportState = &viewport;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pDepthStencilState = &depth_stencil;
        pipeline_info.pColorBlendState = &blend;
        pipeline_info.pDynamicState = &dynamic;
        pipeline_info.layout = pass.layout;
        pipeline_info.renderPass = pass.render_pass;
        pipeline_info.subpass = 0;
        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult pipeline_result = vkCreateGraphicsPipelines(
            generation.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
            &pass.pipeline);
        vkDestroyShaderModule(generation.device, vertex, nullptr);
        vkDestroyShaderModule(generation.device, fragment, nullptr);
        if (pipeline_result != VK_SUCCESS)
        {
            generation.passes.push_back(std::move(pass));
            destroy_generation(generation);
            error = "Rezonality could not create Vulkan pipeline for pass '"
                + source.name + "'";
            return std::nullopt;
        }
        generation.passes.push_back(std::move(pass));
    }
    return generation;
}

void initialize_generation_images(
    VkCommandBuffer command, VulkanGeneration& generation)
{
    for (auto& surface : generation.surfaces)
    {
        if (surface.initialized)
            continue;
        const bool depth = surface.aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
        VkImageMemoryBarrier before{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        before.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        before.newLayout = depth
            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
            : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.image = surface.attachment.image;
        before.subresourceRange = {
            surface.aspect, 0, 1, 0, 1
        };
        before.dstAccessMask = depth
            ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
            : VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            depth ? VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                  : VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &before);
        if (!depth)
        {
            if (surface.upload_buffer.buffer)
            {
                VkBufferImageCopy copy{};
                copy.imageSubresource = {
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1
                };
                copy.imageExtent = { surface.width, surface.height, 1 };
                vkCmdCopyBufferToImage(command, surface.upload_buffer.buffer,
                    surface.attachment.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            }
            else
            {
                const VkClearColorValue clear{
                    { 0.035f, 0.045f, 0.075f, 1.0f }
                };
                const VkImageSubresourceRange range{
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1
                };
                vkCmdClearColorImage(command, surface.attachment.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
            }
            VkImageMemoryBarrier after = before;
            after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            after.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &after);
        }
        surface.initialized = true;
    }
    for (auto& model : generation.models)
        for (auto& slots : model.textures)
            for (auto& texture : slots)
            {
                if (texture.initialized)
                    continue;
                VkImageMemoryBarrier before{
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER
                };
                before.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                before.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                before.image = texture.attachment.image;
                before.subresourceRange
                    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(command,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                    0, nullptr, 1, &before);
                VkBufferImageCopy copy{};
                copy.imageSubresource
                    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                copy.imageExtent = { texture.width, texture.height, 1 };
                vkCmdCopyBufferToImage(command,
                    texture.upload_buffer.buffer, texture.attachment.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                VkImageMemoryBarrier after = before;
                after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                after.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(command,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                    0, nullptr, 1, &after);
                texture.initialized = true;
            }
}

void update_audio_surfaces(VkCommandBuffer command,
    VulkanGeneration& generation, uint32_t frame_index,
    const AudioTextureFrame& audio)
{
    if (audio.rgba.empty())
        return;
    const VkDeviceSize byte_size = audio.rgba.size() * sizeof(float);
    for (auto& surface : generation.surfaces)
    {
        if (!surface.audio_analysis
            || surface.audio_generation == audio.generation
            || surface.audio_upload_buffers.empty())
            continue;
        auto& upload = surface.audio_upload_buffers[frame_index % surface.audio_upload_buffers.size()];
        std::memcpy(upload.mapped, audio.rgba.data(),
            static_cast<size_t>(byte_size));
        vmaFlushAllocation(generation.allocator,
            upload.allocation, 0, byte_size);

        VkImageMemoryBarrier before{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER
        };
        before.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        before.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        before.image = surface.attachment.image;
        before.subresourceRange
            = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        before.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
            1, &before);

        VkBufferImageCopy copy{};
        copy.imageSubresource
            = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { surface.width, surface.height, 1 };
        vkCmdCopyBufferToImage(command, upload.buffer,
            surface.attachment.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &copy);

        VkImageMemoryBarrier after = before;
        after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        after.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
            nullptr, 1, &after);
        surface.audio_generation = audio.generation;
    }
}

void build_model_acceleration_structures(VkCommandBuffer command,
    VulkanGeneration& generation, VulkanModelResource& model)
{
    if (model.acceleration_structures_built)
        return;
    VkAccelerationStructureGeometryTrianglesDataKHR triangles{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR
    };
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = buffer_address(
        generation.device, model.vertex_buffer.buffer);
    triangles.vertexStride = sizeof(rezonality::ModelVertex);
    triangles.maxVertex = model.vertex_count - 1;
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = buffer_address(
        generation.device, model.index_buffer.buffer);
    VkAccelerationStructureGeometryKHR geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR
    };
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.triangles = triangles;
    VkAccelerationStructureBuildGeometryInfoKHR build{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
    };
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.dstAccelerationStructure = model.blas;
    build.geometryCount = 1;
    build.pGeometries = &geometry;
    build.scratchData.deviceAddress = buffer_address(
        generation.device, model.as_scratch_buffer.buffer);
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = model.index_count / 3;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = { &range };
    generation.ray.cmd_build(command, 1, &build, ranges);
    VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    barrier.srcAccessMask
        = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstAccessMask
        = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(command,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        0, 1, &barrier, 0, nullptr, 0, nullptr);

    VkAccelerationStructureGeometryInstancesDataKHR instances{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR
    };
    instances.data.deviceAddress = buffer_address(
        generation.device, model.as_instance_buffer.buffer);
    VkAccelerationStructureGeometryKHR tlas_geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR
    };
    tlas_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlas_geometry.geometry.instances = instances;
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build.dstAccelerationStructure = model.tlas;
    build.pGeometries = &tlas_geometry;
    range.primitiveCount = 1;
    generation.ray.cmd_build(command, 1, &build, ranges);
    barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR
        | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
    model.acceleration_structures_built = true;
}

void transition_ray_target(VkCommandBuffer command,
    const VulkanSurfaceResource& surface, bool for_write)
{
    VkImageMemoryBarrier barrier{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER
    };
    barrier.oldLayout = for_write
        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        : VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = for_write
        ? VK_IMAGE_LAYOUT_GENERAL
        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = surface.attachment.image;
    barrier.subresourceRange
        = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.srcAccessMask = for_write
        ? VK_ACCESS_SHADER_READ_BIT
        : VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = for_write
        ? VK_ACCESS_SHADER_WRITE_BIT
        : VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command,
        for_write ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                  : VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        for_write ? VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
                  : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

#endif

void retire_completed_slot(BackendState& backend, uint32_t frame_index)
{
    if (frame_index >= 64)
        return;
    const uint64_t slot = uint64_t{ 1 } << frame_index;
    for (auto& retired : backend.retired)
        retired.pending_slots &= ~slot;
#if defined(__APPLE__)
    std::erase_if(backend.retired,
        [](const auto& retired) { return retired.pending_slots == 0; });
#else
    for (auto iterator = backend.retired.begin();
         iterator != backend.retired.end();)
    {
        if (iterator->pending_slots == 0)
        {
            destroy_generation(iterator->generation);
            iterator = backend.retired.erase(iterator);
        }
        else
            ++iterator;
    }
#endif
}

struct RezonalityInstance
{
    const DraxulPluginHostApiV2* host = nullptr;
    std::filesystem::path plugin_directory;
    ProjectOptions options;
    std::unique_ptr<draxul::plugin_support::HostServices> services;
    DiagnosticsPublisher diagnostics;
    DraxulPluginViewportV2 viewport{};
    std::unique_ptr<LiveProject> project;
    AudioOptions audio_options;
    std::unique_ptr<AudioAnalyzer> audio;
    std::optional<ShaderBuild> pending_build;
    std::optional<ShaderBuild> active_build;
    BackendState backend;
    uint64_t attempted_generation = 0;
    uint64_t active_generation = 0;
    uint64_t last_success_unix_ms = 0;
    bool visible = true;
    bool focused = false;
    bool paused = false;
    bool quiesced = false;
    double animation_elapsed_seconds = 0.0;
    double last_animation_seconds = -1.0;
    rezonality::Camera camera;
    bool camera_initialized = false;
    std::string status = "building g1";
    std::string presentation_status;
    std::string audio_status;
};

bool uses_audio(const ShaderBuild& build)
{
    return std::any_of(build.surfaces.begin(), build.surfaces.end(),
        [](const auto& surface) { return surface.audio_analysis; });
}

void configure_audio(RezonalityInstance* instance, const ShaderBuild& build)
{
    if (uses_audio(build))
    {
        if (!instance->audio)
        {
            instance->audio
                = std::make_unique<AudioAnalyzer>(instance->audio_options);
            instance->audio->set_visible(instance->visible);
        }
    }
    else
    {
        instance->audio.reset();
        instance->audio_status.clear();
    }
}

void ensure_camera(RezonalityInstance* instance, const ShaderBuild& build)
{
    if (!instance || instance->camera_initialized)
        return;
    const auto model_pass = std::find_if(build.passes.begin(),
        build.passes.end(), [](const auto& pass) {
            return pass.model_index.has_value();
        });
    if (model_pass != build.passes.end())
        instance->camera = model_pass->camera;
    else if (!build.passes.empty())
        instance->camera = build.passes.front().camera;
    instance->camera_initialized = true;
}

void log(RezonalityInstance* instance, uint32_t level,
    const std::string& message)
{
    if (instance && instance->host && instance->host->log)
        instance->host->log(instance->host->host_context,
            level, message.data(), message.size());
}

uint64_t unix_milliseconds()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string diagnostic_stage(const BuildResult& result)
{
    const std::string extension = result.diagnostic_path.extension().string();
    if (extension == ".scenegraph" || extension == ".toml")
        return "parse";
    if (extension == ".vert" || extension == ".frag"
        || extension == ".geom" || extension == ".rgen"
        || extension == ".rmiss" || extension == ".rchit"
        || extension == ".metal")
        return "compile";
    return "prepare";
}

void publish_diagnostics(RezonalityInstance* instance,
    std::string stage, std::string severity,
    const std::filesystem::path& path, int line,
    std::string message,
    const std::vector<DiagnosticEntry>* diagnostics = nullptr)
{
    if (!instance || !instance->diagnostics.available())
        return;
    DiagnosticState state;
    state.project_path = instance->options.project_path;
    state.scenegraph_path
        = instance->options.project_path / instance->options.scenegraph;
    state.path = path;
    state.attempted_generation = instance->attempted_generation;
    state.active_generation = instance->active_generation;
    state.last_success_unix_ms = instance->last_success_unix_ms;
    state.stage = std::move(stage);
    state.severity = std::move(severity);
    state.line = line;
    state.message = std::move(message);
    if (diagnostics && !diagnostics->empty())
    {
        state.diagnostics = *diagnostics;
    }
    else
    {
        state.diagnostics.push_back({
            .path = state.path,
            .stage = state.stage,
            .severity = state.severity,
            .line = state.line,
            .column = state.column,
            .message = state.message,
        });
    }
    std::string error;
    if (!instance->diagnostics.publish(state, error))
        log(instance, DRAXUL_PLUGIN_LOG_WARNING, error);
}

void request_tick(RezonalityInstance* instance)
{
    if (instance && instance->host && instance->host->request_tick)
        instance->host->request_tick(instance->host->host_context);
}

void request_redraw(RezonalityInstance* instance)
{
    if (instance && instance->host && instance->host->request_redraw)
        instance->host->request_redraw(instance->host->host_context);
}

void notify_presentation(RezonalityInstance* instance)
{
    if (instance && instance->host
        && instance->host->notify_presentation_changed)
        instance->host->notify_presentation_changed(
            instance->host->host_context);
}

double advance_animation(RezonalityInstance* instance,
    double monotonic_seconds)
{
    if (instance->last_animation_seconds >= 0.0
        && !instance->paused)
    {
        instance->animation_elapsed_seconds += std::max(
            0.0, monotonic_seconds - instance->last_animation_seconds);
    }
    instance->last_animation_seconds = monotonic_seconds;
    return instance->animation_elapsed_seconds;
}

std::string format_failure(uint64_t attempted_generation,
    uint64_t active_generation, const std::filesystem::path& diagnostic_path,
    int diagnostic_line, std::string_view error)
{
    std::string status = "BUILD FAILED g"
        + std::to_string(attempted_generation);
    if (active_generation != 0)
        status += " | rendering last good g"
            + std::to_string(active_generation);
    if (!diagnostic_path.empty())
    {
        status += " | " + diagnostic_path.filename().string();
        if (diagnostic_line > 0)
            status += ":" + std::to_string(diagnostic_line);
    }
    if (!error.empty())
        status += " | " + std::string(error);
    return status;
}

std::string format_error(const BuildResult& result,
    uint64_t active_generation)
{
    std::string status = format_failure(result.generation, active_generation,
        result.diagnostic_path, result.diagnostic_line, result.error);
    if (result.diagnostics.size() > 1)
        status += " | +" + std::to_string(result.diagnostics.size() - 1)
            + " more";
    return status;
}

void* create_instance(const DraxulPluginCreateInfoV2* info)
{
    if (!info || info->struct_size < sizeof(DraxulPluginCreateInfoV2)
        || !info->host
        || info->host->struct_size < sizeof(DraxulPluginHostApiV2)
        || info->host->abi_version != DRAXUL_PLUGIN_ABI_VERSION)
        return nullptr;

    auto instance = std::make_unique<RezonalityInstance>();
    instance->host = info->host;
    instance->plugin_directory = info->plugin_directory_utf8
        ? std::filesystem::u8path(info->plugin_directory_utf8)
        : std::filesystem::path{};
    instance->viewport = info->initial_viewport;
    std::string error;
    auto options = rezonality::parse_project_options(
        instance->plugin_directory, info->config_json,
        info->config_json_length, error);
    if (!options)
    {
        log(instance.get(), DRAXUL_PLUGIN_LOG_ERROR, error);
        return nullptr;
    }
    instance->options = *options;
    instance->services
        = std::make_unique<draxul::plugin_support::HostServices>(*info);
    instance->diagnostics = DiagnosticsPublisher(
        instance->services->path(DRAXUL_PLUGIN_PATH_CACHE),
        options->project_path, options->diagnostics_id);
    auto* raw = instance.get();
    instance->project = std::make_unique<LiveProject>(
        instance->plugin_directory, *options, [raw] {
            request_tick(raw);
        });
    instance->audio_options = options->audio;
    instance->paused = instance->project->options().paused;
    instance->project->start();
    publish_diagnostics(instance.get(), "watch", "info", {}, -1,
        "building generation 1");
    return instance.release();
}

void quiesce_instance(void* opaque)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || instance->quiesced)
        return;
    instance->quiesced = true;
    if (instance->project)
        instance->project->stop();
}

void destroy_instance(void* opaque)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance)
        return;
    if (instance->project)
        instance->project->stop();
#if !defined(__APPLE__)
    destroy_backend(instance->backend);
#endif
    delete instance;
}

void set_viewport(void* opaque, const DraxulPluginViewportV2* viewport)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (instance && viewport
        && viewport->struct_size >= sizeof(DraxulPluginViewportV2))
        instance->viewport = *viewport;
}

void set_visible(void* opaque, int32_t visible)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance)
        return;
    instance->visible = visible != 0;
    if (instance->audio)
        instance->audio->set_visible(instance->visible);
    if (instance->visible)
        request_redraw(instance);
    notify_presentation(instance);
}

void set_focused(void* opaque, int32_t focused)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance)
        return;
    instance->focused = focused != 0;
    notify_presentation(instance);
}

int32_t handle_input(void* opaque, const DraxulPluginInputEventV2* event)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || !event
        || event->struct_size < sizeof(DraxulPluginInputEventV2))
        return 0;

    if (event->kind == DRAXUL_PLUGIN_INPUT_KEY
        && event->pressed && event->logical_key == 32)
    {
        instance->paused = !instance->paused;
        if (!instance->paused)
            request_redraw(instance);
        notify_presentation(instance);
        return 1;
    }
    if (event->kind == DRAXUL_PLUGIN_INPUT_POINTER_MOVE
        && (event->buttons & 1u) != 0)
    {
        rezonality::camera_orbit(instance->camera,
            { event->delta_x * 0.25f, event->delta_y * 0.25f });
        request_redraw(instance);
        return 1;
    }
    if (event->kind == DRAXUL_PLUGIN_INPUT_WHEEL
        && event->delta_y != 0.0f)
    {
        rezonality::camera_dolly(instance->camera,
            event->delta_y * 0.35f);
        request_redraw(instance);
        return 1;
    }
    return event->kind == DRAXUL_PLUGIN_INPUT_POINTER_BUTTON
            && event->button == 1
        ? 1
        : 0;
}

DraxulPluginTickResultV2 tick(void* opaque,
    const DraxulPluginTickInfoV2*)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || instance->quiesced)
        return tick_result(false, DRAXUL_PLUGIN_NO_DEADLINE);
    if (auto result = instance->project->take_result())
    {
        instance->attempted_generation = result->generation;
        if (result->build)
        {
            instance->pending_build = std::move(*result->build);
            instance->status = "ready g"
                + std::to_string(result->generation);
            publish_diagnostics(instance, "build", "info", {}, -1,
                "candidate generation ready");
            if (instance->visible)
                request_redraw(instance);
        }
        else
        {
            instance->status = format_error(
                *result, instance->active_generation);
            log(instance, DRAXUL_PLUGIN_LOG_ERROR, instance->status);
            publish_diagnostics(instance, diagnostic_stage(*result),
                "error", result->diagnostic_path,
                result->diagnostic_line, result->error,
                &result->diagnostics);
        }
        notify_presentation(instance);
    }
    return tick_result(true, DRAXUL_PLUGIN_NO_DEADLINE);
}

#if defined(__APPLE__)

DraxulPluginRenderResultV2 render_metal(void* opaque,
    const DraxulPluginMetalFrameV2* frame)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || instance->quiesced || !instance->visible)
        return render_result(instance != nullptr,
            DRAXUL_PLUGIN_NO_DEADLINE);
    if (!frame || frame->struct_size < sizeof(*frame)
        || !frame->device || !frame->command_buffer
        || !frame->drawable_texture
        || !frame->continuation_render_pass_descriptor)
        return render_result(false, DRAXUL_PLUGIN_NO_DEADLINE,
            "Rezonality received an incomplete Metal frame");
    if (frame->viewport.width <= 0 || frame->viewport.height <= 0)
        return render_result(true, DRAXUL_PLUGIN_NO_DEADLINE);
    const double animation_seconds = advance_animation(
        instance, frame->monotonic_seconds);
    retire_completed_slot(instance->backend, frame->frame_index);

    id<MTLTexture> target
        = (__bridge id<MTLTexture>)frame->drawable_texture;
    const ShaderBuild* desired = nullptr;
    if (instance->pending_build)
        desired = &*instance->pending_build;
    else if (instance->active_build
        && (!instance->backend.active
            || instance->backend.active->format != target.pixelFormat
            || instance->backend.active->width
                != static_cast<uint32_t>(frame->viewport.width)
            || instance->backend.active->height
                != static_cast<uint32_t>(frame->viewport.height)))
        desired = &*instance->active_build;
    if (desired)
    {
        ensure_camera(instance, *desired);
        static thread_local std::string error;
        error.clear();
        const uint64_t desired_generation = desired->generation;
        auto candidate = create_generation(
            instance->backend, *desired, *frame,
            animation_seconds, instance->camera, error);
        if (candidate)
        {
            if (instance->backend.active)
            {
                const uint64_t used
                    = instance->backend.active->used_slots;
                instance->backend.retired.push_back({ std::move(*instance->backend.active), used });
            }
            instance->backend.active = std::move(*candidate);
            instance->active_build = *desired;
            configure_audio(instance, *instance->active_build);
            instance->active_generation = desired_generation;
            instance->pending_build.reset();
            instance->status = "live g"
                + std::to_string(instance->active_generation) + " | "
                + std::to_string(desired->passes.size()) + " passes | "
                + std::to_string(desired->surfaces.size()) + " surfaces";
            instance->last_success_unix_ms = unix_milliseconds();
            publish_diagnostics(instance, "render", "info", {}, -1,
                "active generation ready");
            notify_presentation(instance);
        }
        else
        {
            instance->pending_build.reset();
            instance->status = format_failure(desired_generation,
                instance->active_generation, {}, -1, error);
            log(instance, DRAXUL_PLUGIN_LOG_ERROR, instance->status);
            publish_diagnostics(instance, "prepare", "error", {}, -1,
                error);
            notify_presentation(instance);
        }
    }
    if (!instance->backend.active
        || instance->backend.active->format != target.pixelFormat)
        return render_result(true, DRAXUL_PLUGIN_NO_DEADLINE);

    if (instance->audio)
    {
        const AudioTextureFrame audio = instance->audio->frame();
        instance->audio_status = audio.status;
        for (auto& surface : instance->backend.active->surfaces)
        {
            if (!surface.audio_analysis
                || surface.audio_generation == audio.generation
                || audio.rgba.empty())
                continue;
            const MTLRegion region = MTLRegionMake2D(0, 0,
                AudioTextureFrame::width, AudioTextureFrame::height);
            [surface.texture replaceRegion:region
                               mipmapLevel:0
                                 withBytes:audio.rgba.data()
                               bytesPerRow:AudioTextureFrame::width * 4 * sizeof(float)];
            surface.audio_generation = audio.generation;
        }
    }

    id<MTLCommandBuffer> command
        = (__bridge id<MTLCommandBuffer>)frame->command_buffer;
    MTLRenderPassDescriptor* pass
        = (__bridge MTLRenderPassDescriptor*)
              frame->continuation_render_pass_descriptor;
    const uint32_t uniform_slot = frame->frame_index
        % instance->backend.active->buffered_frame_count;
    const size_t uniform_offset
        = instance->backend.active->uniform_stride * uniform_slot;
    const auto uniform = make_common_uniforms(
        animation_seconds,
        instance->backend.active->width,
        instance->backend.active->height,
        frame->viewport.x, frame->viewport.y, instance->camera);
    std::memcpy(static_cast<uint8_t*>(
                    instance->backend.active->uniform_buffer.contents)
            + uniform_offset,
        uniform.data(), sizeof(uniform));
    for (const auto& scene_pass : instance->backend.active->passes)
    {
        if (scene_pass.ray_trace)
        {
            auto& model = instance->backend.active->models[*scene_pass.model_index];
            if (!model.acceleration_structures_built)
            {
                id<MTLAccelerationStructureCommandEncoder> blas_encoder
                    = [command accelerationStructureCommandEncoder];
                [blas_encoder buildAccelerationStructure:model.blas
                                              descriptor:model.blas_descriptor
                                           scratchBuffer:model.acceleration_scratch
                                     scratchBufferOffset:0];
                [blas_encoder endEncoding];
                id<MTLAccelerationStructureCommandEncoder> tlas_encoder
                    = [command accelerationStructureCommandEncoder];
                [tlas_encoder buildAccelerationStructure:model.tlas
                                              descriptor:model.tlas_descriptor
                                           scratchBuffer:model.acceleration_scratch
                                     scratchBufferOffset:0];
                [tlas_encoder endEncoding];
                model.acceleration_structures_built = true;
            }
            id<MTLTexture> ray_target
                = instance->backend.active->surfaces[scene_pass.targets.front()].texture;
            id<MTLComputeCommandEncoder> encoder
                = [command computeCommandEncoder];
            [encoder setComputePipelineState:scene_pass.ray_pipeline];
            [encoder setTexture:ray_target atIndex:0];
            [encoder setAccelerationStructure:model.tlas atBufferIndex:0];
            [encoder setBuffer:instance->backend.active->uniform_buffer
                        offset:uniform_offset
                       atIndex:1];
            [encoder setBuffer:model.vertex_buffer offset:0 atIndex:2];
            [encoder setBuffer:model.index_buffer offset:0 atIndex:3];
            [encoder useResource:ray_target usage:MTLResourceUsageWrite];
            [encoder useResource:model.vertex_buffer
                           usage:MTLResourceUsageRead];
            [encoder useResource:model.index_buffer
                           usage:MTLResourceUsageRead];
            [encoder useResource:(id<MTLResource>)model.tlas
                           usage:MTLResourceUsageRead];
            [encoder useResource:(id<MTLResource>)model.blas
                           usage:MTLResourceUsageRead];
            const MTLSize grid
                = MTLSizeMake(ray_target.width, ray_target.height, 1);
            const MTLSize threads = MTLSizeMake(8, 8, 1);
            [encoder dispatchThreads:grid threadsPerThreadgroup:threads];
            [encoder endEncoding];
            continue;
        }
        MTLRenderPassDescriptor* render_pass = pass;
        NSUInteger width = static_cast<NSUInteger>(frame->viewport.width);
        NSUInteger height = static_cast<NSUInteger>(frame->viewport.height);
        NSUInteger origin_x = static_cast<NSUInteger>(
            std::max(0, frame->viewport.x));
        NSUInteger origin_y = static_cast<NSUInteger>(
            std::max(0, frame->viewport.y));
        if (!scene_pass.direct)
        {
            render_pass = [MTLRenderPassDescriptor renderPassDescriptor];
            size_t color_index = 0;
            for (size_t surface_index : scene_pass.targets)
            {
                const auto& surface
                    = instance->backend.active->surfaces[surface_index];
                width = surface.texture.width;
                height = surface.texture.height;
                origin_x = 0;
                origin_y = 0;
                if (surface.depth)
                {
                    render_pass.depthAttachment.texture = surface.texture;
                    render_pass.depthAttachment.loadAction
                        = scene_pass.has_clear
                        ? MTLLoadActionClear
                        : MTLLoadActionLoad;
                    render_pass.depthAttachment.storeAction
                        = MTLStoreActionStore;
                    render_pass.depthAttachment.clearDepth = 1.0;
                }
                else
                {
                    auto* attachment
                        = render_pass.colorAttachments[color_index++];
                    attachment.texture = surface.texture;
                    attachment.loadAction = scene_pass.has_clear
                        ? MTLLoadActionClear
                        : MTLLoadActionLoad;
                    attachment.storeAction = MTLStoreActionStore;
                    attachment.clearColor = MTLClearColorMake(
                        scene_pass.clear[0], scene_pass.clear[1],
                        scene_pass.clear[2], scene_pass.clear[3]);
                }
            }
        }
        id<MTLRenderCommandEncoder> encoder
            = [command renderCommandEncoderWithDescriptor:render_pass];
        [encoder setRenderPipelineState:scene_pass.pipeline];
        if (scene_pass.model_index)
            [encoder setVertexBuffer:instance->backend.active->models[*scene_pass.model_index].vertex_buffer
                              offset:0
                             atIndex:0];
        else
            [encoder setVertexBuffer:instance->backend.vertex_buffer
                              offset:0
                             atIndex:0];
        [encoder setVertexBuffer:instance->backend.active->uniform_buffer
                          offset:uniform_offset
                         atIndex:1];
        [encoder setFragmentBuffer:instance->backend.active->uniform_buffer
                            offset:uniform_offset
                           atIndex:1];
        for (NSUInteger index = 0; index < scene_pass.samplers.size(); ++index)
        {
            const auto& surface = instance->backend.active->surfaces[scene_pass.samplers[index]];
            [encoder setFragmentTexture:surface.texture atIndex:index];
            [encoder setFragmentSamplerState:surface.repeat
                         ? instance->backend.active->repeat_sampler
                         : instance->backend.active->clamp_sampler
                                     atIndex:index];
        }
        if (scene_pass.model_index)
        {
            const auto& model = instance->backend.active->models[*scene_pass.model_index];
            [encoder setFragmentBuffer:model.material_buffer
                                offset:0
                               atIndex:2];
            for (NSUInteger kind = 0; kind < model.textures.size(); ++kind)
            {
                for (NSUInteger index = 0;
                     index < rezonality::kMaxModelMaterials; ++index)
                {
                    const id<MTLTexture> texture = model.textures[kind][std::min(index, model.textures[kind].size() - 1)];
                    const NSUInteger binding = 16
                        + kind * rezonality::kMaxModelMaterials + index;
                    [encoder setFragmentTexture:texture atIndex:binding];
                    [encoder setFragmentSamplerState:
                                 instance->backend.active->repeat_sampler
                                             atIndex:binding];
                }
            }
        }
        const MTLViewport viewport{ static_cast<double>(origin_x),
            static_cast<double>(origin_y), static_cast<double>(width),
            static_cast<double>(height), 0.0, 1.0 };
        const MTLScissorRect scissor{ origin_x, origin_y, width, height };
        [encoder setViewport:viewport];
        [encoder setScissorRect:scissor];
        if (scene_pass.model_index)
        {
            const auto& model = instance->backend.active->models[*scene_pass.model_index];
            for (const auto& part : model.parts)
            {
                uint32_t material_index = part.material_index;
                [encoder setFragmentBytes:&material_index
                                   length:sizeof(material_index)
                                  atIndex:0];
                [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                    indexCount:part.index_count
                                     indexType:MTLIndexTypeUInt32
                                   indexBuffer:model.index_buffer
                             indexBufferOffset:part.index_offset
                             * sizeof(uint32_t)];
            }
        }
        else
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:6];
        [encoder endEncoding];
    }
    if (frame->frame_index < 64)
        instance->backend.active->used_slots
            |= uint64_t{ 1 } << frame->frame_index;
    return render_result(true, instance->paused ? DRAXUL_PLUGIN_NO_DEADLINE : draxul::plugin_support::kFrameDelayNs);
}

#else

DraxulPluginRenderResultV2 render_vulkan(void* opaque,
    const DraxulPluginVulkanFrameV2* frame)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || instance->quiesced || !instance->visible)
        return render_result(instance != nullptr,
            DRAXUL_PLUGIN_NO_DEADLINE);
    if (!frame || frame->struct_size < sizeof(*frame)
        || !frame->device || !frame->physical_device
        || !frame->command_buffer || !frame->continuation_render_pass
        || !frame->continuation_framebuffer)
        return render_result(false, DRAXUL_PLUGIN_NO_DEADLINE,
            "Rezonality received an incomplete Vulkan frame");
    if (frame->viewport.width <= 0 || frame->viewport.height <= 0)
        return render_result(true, DRAXUL_PLUGIN_NO_DEADLINE);
    const double animation_seconds = advance_animation(
        instance, frame->monotonic_seconds);
    retire_completed_slot(instance->backend, frame->frame_index);

    const VkRenderPass render_pass = reinterpret_cast<VkRenderPass>(
        static_cast<uintptr_t>(frame->continuation_render_pass));
    const ShaderBuild* desired = nullptr;
    if (instance->pending_build)
        desired = &*instance->pending_build;
    else if (instance->active_build
        && (!instance->backend.active
            || instance->backend.active->target_generation
                != frame->target_generation
            || instance->backend.active->render_pass != render_pass
            || instance->backend.active->width
                != static_cast<uint32_t>(frame->viewport.width)
            || instance->backend.active->height
                != static_cast<uint32_t>(frame->viewport.height)))
        desired = &*instance->active_build;
    if (desired)
    {
        ensure_camera(instance, *desired);
        static thread_local std::string error;
        error.clear();
        const uint64_t desired_generation = desired->generation;
        auto candidate = create_generation(
            instance->backend, *desired, *frame,
            animation_seconds, instance->camera, error);
        if (candidate)
        {
            if (instance->backend.active)
            {
                const uint64_t used
                    = instance->backend.active->used_slots;
                instance->backend.retired.push_back({ std::move(*instance->backend.active), used });
            }
            instance->backend.active = std::move(*candidate);
            instance->active_build = *desired;
            configure_audio(instance, *instance->active_build);
            instance->active_generation = desired_generation;
            instance->pending_build.reset();
            instance->status = "live g"
                + std::to_string(instance->active_generation) + " | "
                + std::to_string(desired->passes.size()) + " passes | "
                + std::to_string(desired->surfaces.size()) + " surfaces";
            instance->last_success_unix_ms = unix_milliseconds();
            publish_diagnostics(instance, "render", "info", {}, -1,
                "active generation ready");
            notify_presentation(instance);
        }
        else
        {
            instance->pending_build.reset();
            instance->status = format_failure(desired_generation,
                instance->active_generation, {}, -1, error);
            log(instance, DRAXUL_PLUGIN_LOG_ERROR, instance->status);
            publish_diagnostics(instance, "prepare", "error", {}, -1,
                error);
            notify_presentation(instance);
        }
    }
    if (!instance->backend.active
        || instance->backend.active->target_generation
            != frame->target_generation
        || instance->backend.active->render_pass != render_pass)
        return render_result(true, DRAXUL_PLUGIN_NO_DEADLINE);

    const VkCommandBuffer command
        = static_cast<VkCommandBuffer>(frame->command_buffer);
    initialize_generation_images(command, *instance->backend.active);
    if (instance->audio)
    {
        const AudioTextureFrame audio = instance->audio->frame();
        instance->audio_status = audio.status;
        update_audio_surfaces(command, *instance->backend.active,
            frame->frame_index, audio);
    }
    const uint32_t uniform_slot = frame->frame_index
        % instance->backend.active->buffered_frame_count;
    const size_t uniform_offset
        = instance->backend.active->uniform_stride * uniform_slot;
    const auto uniform = make_common_uniforms(
        animation_seconds,
        instance->backend.active->width,
        instance->backend.active->height,
        frame->viewport.x, frame->viewport.y, instance->camera);
    std::memcpy(static_cast<uint8_t*>(
                    instance->backend.active->uniform_buffer.mapped)
            + uniform_offset,
        uniform.data(), sizeof(uniform));
    vmaFlushAllocation(instance->backend.active->allocator,
        instance->backend.active->uniform_buffer.allocation,
        uniform_offset, sizeof(uniform));
    const VkDeviceSize offset = 0;
    for (auto& pass : instance->backend.active->passes)
    {
        if (pass.ray_trace)
        {
            auto& model = instance->backend.active->models[*pass.model_index];
            build_model_acceleration_structures(
                command, *instance->backend.active, model);
            const auto& target = instance->backend.active->surfaces[pass.target_surfaces.front()];
            transition_ray_target(command, target, true);
            vkCmdBindPipeline(command,
                VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pass.pipeline);
            const VkDescriptorSet sets[]
                = { pass.uniform_set, pass.ray_set };
            const uint32_t dynamic_offset
                = static_cast<uint32_t>(uniform_offset);
            vkCmdBindDescriptorSets(command,
                VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pass.layout, 0, 2,
                sets, 1, &dynamic_offset);
            instance->backend.active->ray.cmd_trace(command,
                &pass.raygen_region, &pass.miss_region, &pass.hit_region,
                &pass.callable_region, target.width, target.height, 1);
            transition_ray_target(command, target, false);
            continue;
        }
        uint32_t target_width = instance->backend.active->width;
        uint32_t target_height = instance->backend.active->height;
        int32_t target_x = 0;
        int32_t target_y = 0;
        VkFramebuffer framebuffer = pass.framebuffer;
        if (pass.direct)
        {
            target_width = static_cast<uint32_t>(frame->framebuffer_width);
            target_height = static_cast<uint32_t>(frame->framebuffer_height);
            target_x = frame->viewport.x;
            target_y = frame->viewport.y;
            framebuffer = reinterpret_cast<VkFramebuffer>(
                static_cast<uintptr_t>(frame->continuation_framebuffer));
        }
        else if (!pass.target_surfaces.empty())
        {
            const auto& target = instance->backend.active->surfaces[pass.target_surfaces.front()];
            target_width = target.width;
            target_height = target.height;
        }
        std::vector<VkClearValue> clears(pass.target_surfaces.size());
        for (size_t index = 0; index < pass.target_surfaces.size(); ++index)
        {
            const auto& target = instance->backend.active->surfaces[pass.target_surfaces[index]];
            if (target.aspect == VK_IMAGE_ASPECT_DEPTH_BIT)
                clears[index].depthStencil = { 1.0f, 0 };
            else
                std::copy(std::begin(pass.clear), std::end(pass.clear),
                    clears[index].color.float32);
        }
        VkRenderPassBeginInfo begin{
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO
        };
        begin.renderPass = pass.render_pass;
        begin.framebuffer = framebuffer;
        begin.renderArea.extent = { target_width, target_height };
        begin.clearValueCount = static_cast<uint32_t>(clears.size());
        begin.pClearValues = clears.data();
        vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{};
        viewport.x = static_cast<float>(target_x);
        viewport.y = static_cast<float>(target_y);
        viewport.width = static_cast<float>(pass.direct
                ? frame->viewport.width
                : static_cast<int32_t>(target_width));
        viewport.height = static_cast<float>(pass.direct
                ? frame->viewport.height
                : static_cast<int32_t>(target_height));
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{
            { target_x, target_y },
            { static_cast<uint32_t>(pass.direct
                      ? frame->viewport.width
                      : static_cast<int32_t>(target_width)),
                static_cast<uint32_t>(pass.direct
                        ? frame->viewport.height
                        : static_cast<int32_t>(target_height)) }
        };
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        vkCmdBindPipeline(
            command, VK_PIPELINE_BIND_POINT_GRAPHICS, pass.pipeline);
        std::vector<VkDescriptorSet> sets
            = { pass.uniform_set, pass.sampler_set };
        if (pass.model_index)
            sets.push_back(instance->backend.active->models[*pass.model_index].descriptor_set);
        const uint32_t dynamic_offset
            = static_cast<uint32_t>(uniform_offset);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pass.layout, 0, static_cast<uint32_t>(sets.size()),
            sets.data(), 1, &dynamic_offset);
        if (pass.model_index)
        {
            const auto& model = instance->backend.active->models[*pass.model_index];
            vkCmdBindVertexBuffers(command, 0, 1,
                &model.vertex_buffer.buffer, &offset);
            vkCmdBindIndexBuffer(command, model.index_buffer.buffer, 0,
                VK_INDEX_TYPE_UINT32);
            for (const auto& part : model.parts)
            {
                vkCmdPushConstants(command, pass.layout,
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uint32_t),
                    &part.material_index);
                vkCmdDrawIndexed(command, part.index_count, 1,
                    part.index_offset, 0, 0);
            }
        }
        else
        {
            vkCmdBindVertexBuffers(command, 0, 1,
                &instance->backend.vertex_buffer, &offset);
            vkCmdDraw(command, 6, 1, 0, 0);
        }
        vkCmdEndRenderPass(command);
    }
    if (frame->frame_index < 64)
        instance->backend.active->used_slots
            |= uint64_t{ 1 } << frame->frame_index;
    return render_result(true, instance->paused ? DRAXUL_PLUGIN_NO_DEADLINE : draxul::plugin_support::kFrameDelayNs);
}

#endif

int32_t get_presentation_state(void* opaque,
    DraxulPluginPresentationStateV2* state)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || !state
        || state->struct_size < sizeof(DraxulPluginPresentationStateV2))
        return 0;
    instance->presentation_status
        = instance->options.project_path.filename().string()
        + " | " + instance->status;
    if (!instance->audio_status.empty())
        instance->presentation_status += " | " + instance->audio_status;
    if (instance->paused)
        instance->presentation_status += " | paused";
    if (!instance->visible)
        instance->presentation_status += " | hidden";
    if (instance->focused)
        instance->presentation_status += " | focused";

    *state = {};
    state->struct_size = sizeof(*state);
    state->display_name = { "Rezonality", 10 };
    state->status_text = { instance->presentation_status.data(),
        instance->presentation_status.size() };
    state->background_red = 0.025f;
    state->background_green = 0.035f;
    state->background_blue = 0.055f;
    state->background_alpha = 1.0f;
    state->content_ready = instance->quiesced ? 0 : 1;
    state->mouse_cursor = DRAXUL_PLUGIN_CURSOR_DEFAULT;
    return 1;
}

int32_t dispatch_action(void* opaque, const char* action,
    size_t action_length)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || !action || instance->quiesced
        || std::string_view(action, action_length) != "rezonality_reload")
        return 0;
    instance->status = "building g"
        + std::to_string(instance->attempted_generation + 1);
    instance->project->force_reload();
    notify_presentation(instance);
    return 1;
}

constexpr draxul::plugin_support::AdapterAction kActions[] = {
    { "rezonality_reload", "Reload Rezonality Project" },
};

using Presentation = draxul::plugin_support::PresentationAdapter<kActions,
    &get_presentation_state, &dispatch_action>;

int32_t export_reload_json(void* opaque, char* buffer,
    size_t* in_out_size)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || !in_out_size)
        return 0;
    const std::string value = nlohmann::json{
        { "project_path", instance->options.project_path.generic_string() },
        { "scenegraph", instance->options.scenegraph.generic_string() },
        { "time_seconds", instance->animation_elapsed_seconds },
        { "paused", instance->paused },
        { "camera_position", {
            instance->camera.position.x,
            instance->camera.position.y,
            instance->camera.position.z } },
        { "camera_focal_point", {
            instance->camera.focal_point.x,
            instance->camera.focal_point.y,
            instance->camera.focal_point.z } },
    }.dump();
    const size_t required = value.size() + 1;
    if (!buffer)
    {
        *in_out_size = required;
        return required <= DRAXUL_PLUGIN_MAX_HOT_RELOAD_JSON_BYTES + 1;
    }
    if (*in_out_size < required)
    {
        *in_out_size = required;
        return 0;
    }
    std::memcpy(buffer, value.c_str(), required);
    *in_out_size = required;
    return 1;
}

std::optional<glm::vec3> reload_vec3(
    const nlohmann::json& state, const char* key)
{
    const auto value = state.find(key);
    if (value == state.end() || !value->is_array() || value->size() != 3)
        return std::nullopt;
    glm::vec3 result;
    for (size_t index = 0; index < 3; ++index)
    {
        if (!(*value)[index].is_number())
            return std::nullopt;
        result[index] = (*value)[index].get<float>();
        if (!std::isfinite(result[index]) || std::abs(result[index]) > 1e6f)
            return std::nullopt;
    }
    return result;
}

int32_t import_reload_json(void* opaque, const char* json,
    size_t json_length, const char* source_schema_id,
    uint32_t source_schema_version)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || !json || !source_schema_id
        || std::string_view(source_schema_id)
            != "dev.draxul.rezonality.state"
        || source_schema_version != 1
        || json_length > DRAXUL_PLUGIN_MAX_HOT_RELOAD_JSON_BYTES)
        return 0;
    try
    {
        const auto state = nlohmann::json::parse(json, json + json_length);
        if (!state.is_object()
            || state.value("project_path", std::string{})
                != instance->options.project_path.generic_string()
            || state.value("scenegraph", std::string{})
                != instance->options.scenegraph.generic_string())
            return 0;
        const double time = state.value("time_seconds", 0.0);
        const auto position = reload_vec3(state, "camera_position");
        const auto focal_point = reload_vec3(state, "camera_focal_point");
        if (!std::isfinite(time) || time < 0.0 || time > 1e12
            || !position || !focal_point)
            return 0;
        instance->animation_elapsed_seconds = time;
        instance->last_animation_seconds = -1.0;
        instance->paused = state.value("paused", instance->paused);
        rezonality::camera_set_pos_lookat(
            instance->camera, *position, *focal_point);
        instance->camera_initialized = true;
        request_redraw(instance);
        notify_presentation(instance);
        return 1;
    }
    catch (...)
    {
        return 0;
    }
}

int32_t query_extension(void* instance, const char* extension_id,
    size_t extension_id_length, uint32_t requested_version,
    void* extension_table, size_t extension_table_size)
{
    const std::string_view id(extension_id ? extension_id : "",
        extension_id ? extension_id_length : 0);
    if (id == DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_ID
        && requested_version == DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_VERSION
        && extension_table
        && extension_table_size >= sizeof(DraxulPluginHotReloadExtensionV2))
    {
        auto* extension = static_cast<DraxulPluginHotReloadExtensionV2*>(
            extension_table);
        *extension = {
            sizeof(*extension), DRAXUL_PLUGIN_HOT_RELOAD_EXTENSION_VERSION,
            "dev.draxul.rezonality.state", 1,
            &export_reload_json, &import_reload_json
        };
        return 1;
    }
    return Presentation::query_extension(instance, extension_id,
        extension_id_length, requested_version, extension_table,
        extension_table_size);
}

const DraxulPluginApiV2 kApi = draxul::plugin_support::make_plugin_api(
    { kPluginId, "Rezonality", kPluginVersion,
        draxul::plugin_support::kNativeBackendMask },
    {
        .create_instance = &create_instance,
        .quiesce_instance = &quiesce_instance,
        .destroy_instance = &destroy_instance,
        .set_viewport = &set_viewport,
        .set_visible = &set_visible,
        .set_focused = &set_focused,
        .handle_input = &handle_input,
        .tick = &tick,
#if defined(__APPLE__)
        .render_metal = &render_metal,
#else
        .render_vulkan = &render_vulkan,
#endif
        .query_extension = &query_extension,
    });

} // namespace

extern "C" DRAXUL_PLUGIN_EXPORT const DraxulPluginApiV2*
draxul_plugin_query_v2(uint32_t requested_abi)
{
    return requested_abi == DRAXUL_PLUGIN_ABI_VERSION ? &kApi : nullptr;
}
