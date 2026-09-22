#include "native_backend.h"

#include "gpu_resource_transaction.h"
#include "native_backend_helpers.h"

#include <draxul/plugin_adapter.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <spirv_msl.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using draxul::plugin_support::render_result;
using rezonality::AudioTextureFrame;
using rezonality::ShaderBuild;
using rezonality::detail::CommonUniformBlock;
using rezonality::detail::align_up;
using rezonality::detail::kCommonUniformFloatCount;
using rezonality::detail::kScreenVertices;
using rezonality::detail::make_common_uniforms;
using rezonality::detail::ScreenVertex;

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
        bool has_depth = false;
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
    id<MTLDepthStencilState> model_depth_state = nil;
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
        spirv_cross::MSLResourceBinding material_sampler{};
        material_sampler.stage = model;
        material_sampler.desc_set = 2;
        material_sampler.binding = 6;
        material_sampler.msl_sampler = 15;
        compiler.add_msl_resource_binding(material_sampler);
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
    MTLDepthStencilDescriptor* depth_descriptor
        = [[MTLDepthStencilDescriptor alloc] init];
    depth_descriptor.depthCompareFunction = MTLCompareFunctionLessEqual;
    depth_descriptor.depthWriteEnabled = YES;
    generation.model_depth_state
        = [device newDepthStencilStateWithDescriptor:depth_descriptor];
    if (!generation.model_depth_state)
    {
        error = "Rezonality could not create its Metal model depth state";
        return std::nullopt;
    }
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
        if (has_image
            && !rezonality::validate_surface_upload_storage(source, error))
            return std::nullopt;
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
            pass.has_depth = pass.has_depth
                || generation.surfaces[*surface].depth;
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

} // namespace

namespace rezonality
{

struct NativeBackend::Impl
{
    BackendState backend;
    std::optional<MetalGeneration> prepared;
    const DraxulPluginMetalFrameV2* frame = nullptr;
    const Camera* camera = nullptr;
    double animation_seconds = 0.0;
};

NativeBackend::NativeBackend()
    : impl_(std::make_unique<Impl>())
{
}

NativeBackend::~NativeBackend() = default;

void NativeBackend::bind_frame(const DraxulPluginMetalFrameV2& frame,
    double animation_seconds, const Camera& camera)
{
    impl_->frame = &frame;
    impl_->camera = &camera;
    impl_->animation_seconds = animation_seconds;
}

bool NativeBackend::active_compatible() const
{
    if (!impl_->frame || !impl_->backend.active)
        return false;
    id<MTLTexture> target
        = (__bridge id<MTLTexture>)impl_->frame->drawable_texture;
    return target
        && impl_->backend.active->format == target.pixelFormat
        && impl_->backend.active->width
            == static_cast<uint32_t>(impl_->frame->viewport.width)
        && impl_->backend.active->height
            == static_cast<uint32_t>(impl_->frame->viewport.height);
}

BackendPreparation NativeBackend::prepare(const ShaderBuild& build)
{
    if (!impl_->frame || !impl_->camera)
        return { false, "Rezonality Metal backend has no bound frame" };
    std::string error;
    impl_->prepared = create_generation(impl_->backend, build,
        *impl_->frame, impl_->animation_seconds, *impl_->camera, error);
    return { impl_->prepared.has_value(), std::move(error) };
}

void NativeBackend::activate_prepared()
{
    if (!impl_->prepared)
        return;
    if (impl_->backend.active)
    {
        const uint64_t used = impl_->backend.active->used_slots;
        impl_->backend.retired.push_back(
            { std::move(*impl_->backend.active), used });
    }
    impl_->backend.active = std::move(*impl_->prepared);
    impl_->prepared.reset();
}

void NativeBackend::retire_completed_slot(uint32_t frame_index)
{
    if (frame_index >= 64)
        return;
    const uint64_t slot = uint64_t{ 1 } << frame_index;
    for (auto& retired : impl_->backend.retired)
        retired.pending_slots &= ~slot;
    std::erase_if(impl_->backend.retired,
        [](const auto& retired) { return retired.pending_slots == 0; });
}

DraxulPluginRenderResultV2 NativeBackend::record(
    const DraxulPluginMetalFrameV2& bound_frame,
    const AudioTextureFrame* audio, bool paused)
{
    const auto* frame = &bound_frame;
    id<MTLTexture> target
        = (__bridge id<MTLTexture>)frame->drawable_texture;
    if (!impl_->backend.active
        || impl_->backend.active->format != target.pixelFormat)
        return render_result(true, DRAXUL_PLUGIN_NO_DEADLINE);

    if (audio)
    {
        for (auto& surface : impl_->backend.active->surfaces)
        {
            if (!surface.audio_analysis
                || surface.audio_generation == audio->generation
                || audio->rgba.empty())
                continue;
            const MTLRegion region = MTLRegionMake2D(0, 0,
                AudioTextureFrame::width, AudioTextureFrame::height);
            [surface.texture replaceRegion:region
                               mipmapLevel:0
                                 withBytes:audio->rgba.data()
                               bytesPerRow:AudioTextureFrame::width * 4 * sizeof(float)];
            surface.audio_generation = audio->generation;
        }
    }

    id<MTLCommandBuffer> command
        = (__bridge id<MTLCommandBuffer>)frame->command_buffer;
    MTLRenderPassDescriptor* pass
        = (__bridge MTLRenderPassDescriptor*)
              frame->continuation_render_pass_descriptor;
    const uint32_t uniform_slot = frame->frame_index
        % impl_->backend.active->buffered_frame_count;
    const size_t uniform_offset
        = impl_->backend.active->uniform_stride * uniform_slot;
    const auto uniform = make_common_uniforms(
        impl_->animation_seconds,
        impl_->backend.active->width,
        impl_->backend.active->height,
        frame->viewport.x, frame->viewport.y, *impl_->camera);
    std::memcpy(static_cast<uint8_t*>(
                    impl_->backend.active->uniform_buffer.contents)
            + uniform_offset,
        uniform.data(), sizeof(uniform));
    for (const auto& scene_pass : impl_->backend.active->passes)
    {
        if (scene_pass.ray_trace)
        {
            auto& model = impl_->backend.active->models[*scene_pass.model_index];
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
                = impl_->backend.active->surfaces[scene_pass.targets.front()].texture;
            id<MTLComputeCommandEncoder> encoder
                = [command computeCommandEncoder];
            [encoder setComputePipelineState:scene_pass.ray_pipeline];
            [encoder setTexture:ray_target atIndex:0];
            [encoder setAccelerationStructure:model.tlas atBufferIndex:0];
            [encoder setBuffer:impl_->backend.active->uniform_buffer
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
                    = impl_->backend.active->surfaces[surface_index];
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
        if (scene_pass.model_index && scene_pass.has_depth)
        {
            [encoder setDepthStencilState:
                         impl_->backend.active->model_depth_state];
        }
        if (scene_pass.model_index)
            [encoder setVertexBuffer:impl_->backend.active->models[*scene_pass.model_index].vertex_buffer
                              offset:0
                             atIndex:0];
        else
            [encoder setVertexBuffer:impl_->backend.vertex_buffer
                              offset:0
                             atIndex:0];
        [encoder setVertexBuffer:impl_->backend.active->uniform_buffer
                          offset:uniform_offset
                         atIndex:1];
        [encoder setFragmentBuffer:impl_->backend.active->uniform_buffer
                            offset:uniform_offset
                           atIndex:1];
        for (NSUInteger index = 0; index < scene_pass.samplers.size(); ++index)
        {
            const auto& surface = impl_->backend.active->surfaces[scene_pass.samplers[index]];
            [encoder setFragmentTexture:surface.texture atIndex:index];
            [encoder setFragmentSamplerState:surface.repeat
                         ? impl_->backend.active->repeat_sampler
                         : impl_->backend.active->clamp_sampler
                                     atIndex:index];
        }
        if (scene_pass.model_index)
        {
            const auto& model = impl_->backend.active->models[*scene_pass.model_index];
            [encoder setFragmentBuffer:model.material_buffer
                                offset:0
                               atIndex:2];
            [encoder setFragmentSamplerState:
                         impl_->backend.active->repeat_sampler
                                     atIndex:15];
            for (NSUInteger kind = 0; kind < model.textures.size(); ++kind)
            {
                for (NSUInteger index = 0;
                     index < rezonality::kMaxModelMaterials; ++index)
                {
                    const id<MTLTexture> texture = model.textures[kind][std::min(index, model.textures[kind].size() - 1)];
                    const NSUInteger binding = 16
                        + kind * rezonality::kMaxModelMaterials + index;
                    [encoder setFragmentTexture:texture atIndex:binding];
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
            const auto& model = impl_->backend.active->models[*scene_pass.model_index];
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
        impl_->backend.active->used_slots
            |= uint64_t{ 1 } << frame->frame_index;
    return render_result(true, paused ? DRAXUL_PLUGIN_NO_DEADLINE : draxul::plugin_support::kFrameDelayNs);
}

} // namespace rezonality
