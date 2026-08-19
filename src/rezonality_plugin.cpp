#include "live_project.h"

#include <draxul/plugin_adapter.h>
#include <draxul/plugin_api.h>

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
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using draxul::plugin_support::render_result;
using draxul::plugin_support::tick_result;
using rezonality::BuildResult;
using rezonality::LiveProject;
using rezonality::ShaderBuild;

constexpr const char* kPluginId = "dev.draxul.rezonality";
constexpr size_t kCommonUniformFloatCount = 192;
using CommonUniformBlock = std::array<float, kCommonUniformFloatCount>;

size_t align_up(size_t value, size_t alignment)
{
    return alignment == 0
        ? value
        : (value + alignment - 1) / alignment * alignment;
}

CommonUniformBlock make_common_uniforms(double elapsed_seconds,
    uint32_t width, uint32_t height)
{
    CommonUniformBlock uniform{};
    const float time = static_cast<float>(std::max(0.0, elapsed_seconds));
    uniform[0] = time;
    uniform[1] = time;
    uniform[2] = 1.0f / 60.0f;
    uniform[3] = time * 60.0f;
    uniform[4] = 60.0f;
    uniform[6] = 1.0f;
    uniform[8] = static_cast<float>(width);
    uniform[9] = static_cast<float>(height);
    uniform[10] = 1.0f;
    for (size_t matrix : { size_t{ 56 }, size_t{ 72 }, size_t{ 88 },
             size_t{ 104 }, size_t{ 120 }, size_t{ 136 } })
        for (size_t diagonal = 0; diagonal < 4; ++diagonal)
            uniform[matrix + diagonal * 5] = 1.0f;
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
    };
    struct Pass
    {
        id<MTLRenderPipelineState> pipeline = nil;
        std::vector<size_t> targets;
        std::vector<size_t> samplers;
        bool direct = false;
        bool has_clear = false;
        float clear[4] = { 0, 0, 0, 1 };
    };
    std::vector<Surface> surfaces;
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

std::optional<MetalGeneration> create_generation(BackendState& backend,
    const ShaderBuild& build, const DraxulPluginMetalFrameV2& frame,
    double animation_seconds, std::string& error)
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

    MetalGeneration generation;
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
        generation.width, generation.height);
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
        surface.repeat = !source.image_pixels.empty();
        MTLTextureDescriptor* texture = [[MTLTextureDescriptor alloc] init];
        texture.textureType = MTLTextureType2D;
        texture.width = source.image_width != 0 ? source.image_width
            : std::max<NSUInteger>(1, static_cast<NSUInteger>(
                  generation.width * std::max(0.01f, source.scale_x)));
        texture.height = source.image_height != 0 ? source.image_height
            : std::max<NSUInteger>(1, static_cast<NSUInteger>(
                  generation.height * std::max(0.01f, source.scale_y)));
        texture.storageMode = source.image_pixels.empty()
            ? MTLStorageModePrivate : MTLStorageModeManaged;
        texture.usage = surface.depth
            ? MTLTextureUsageRenderTarget
            : MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
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
        if (!source.image_pixels.empty())
        {
            const MTLRegion region = MTLRegionMake2D(
                0, 0, source.image_width, source.image_height);
            [surface.texture replaceRegion:region mipmapLevel:0
                withBytes:source.image_pixels.data()
                bytesPerRow:source.image_width * 4];
        }
        generation.surfaces.push_back(surface);
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
            options:nil error:&compile_error];
        id<MTLLibrary> fragment_library = [device
            newLibraryWithSource:[NSString stringWithUTF8String:fragment_source.c_str()]
            options:nil error:&compile_error];
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
        descriptor.vertexDescriptor = vertices;
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
            newRenderPipelineStateWithDescriptor:descriptor error:&compile_error];
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
    VkSampler sampler = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0;
    uint32_t height = 0;
    bool initialized = false;
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
    std::vector<size_t> target_surfaces;
    bool direct = false;
    bool has_clear = false;
    float clear[4] = { 0, 0, 0, 1 };
};

struct VulkanGeneration
{
    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    draxul::vkresources::BufferResource uniform_buffer;
    size_t uniform_stride = 0;
    uint32_t buffered_frame_count = 1;
    std::vector<VulkanSurfaceResource> surfaces;
    std::vector<VulkanPassResource> passes;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t target_generation = 0;
    uint64_t source_generation = 0;
    uint64_t used_slots = 0;
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
    }
    for (auto& surface : generation.surfaces)
    {
        if (surface.sampler)
            vkDestroySampler(generation.device, surface.sampler, nullptr);
        draxul::vkresources::destroy_attachment(
            generation.device, generation.allocator, surface.attachment);
        draxul::vkresources::destroy_buffer(
            generation.allocator, surface.upload_buffer);
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
    surface.format = surface_format(source.format);
    surface.aspect = source.format == ShaderBuild::SurfaceFormat::Depth32
        ? VK_IMAGE_ASPECT_DEPTH_BIT
        : VK_IMAGE_ASPECT_COLOR_BIT;
    surface.width = source.image_width != 0 ? source.image_width
        : std::max(1u, static_cast<uint32_t>(
              pane_width * std::max(0.01f, source.scale_x)));
    surface.height = source.image_height != 0 ? source.image_height
        : std::max(1u, static_cast<uint32_t>(
              pane_height * std::max(0.01f, source.scale_y)));
    const bool depth = surface.aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
    const VkImageUsageFlags usage = depth
        ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
        : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
            | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
            = source.image_pixels.empty()
            ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
            : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler.addressModeU = address_mode;
        sampler.addressModeV = address_mode;
        sampler.addressModeW = address_mode;
        sampler.maxLod = 1.0f;
        if (vkCreateSampler(generation.device, &sampler, nullptr,
                &surface.sampler) != VK_SUCCESS)
        {
            draxul::vkresources::destroy_attachment(generation.device,
                generation.allocator, surface.attachment);
            error = "Rezonality could not create sampler for surface '"
                + source.name + "'";
            return false;
        }
    }
    if (!source.image_pixels.empty())
    {
        draxul::vkresources::ScopedBuffer upload;
        const draxul::vkresources::BufferRequest upload_request(
            source.image_pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
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
        std::memcpy(surface.upload_buffer.mapped,
            source.image_pixels.data(), source.image_pixels.size());
        vmaFlushAllocation(generation.allocator,
            surface.upload_buffer.allocation, 0, source.image_pixels.size());
    }
    generation.surfaces.push_back(std::move(surface));
    return true;
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
            &pass.render_pass) != VK_SUCCESS)
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
            &pass.framebuffer) != VK_SUCCESS)
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
            nullptr, &pass.uniform_layout) != VK_SUCCESS)
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
            nullptr, &pass.sampler_layout) != VK_SUCCESS)
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
            &pass.descriptor_pool) != VK_SUCCESS)
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
    const VkDescriptorSetLayout set_layouts[]
        = { pass.uniform_layout, pass.sampler_layout };
    VkPipelineLayoutCreateInfo layout{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    layout.setLayoutCount = 2;
    layout.pSetLayouts = set_layouts;
    if (vkCreatePipelineLayout(generation.device, &layout, nullptr,
            &pass.layout) != VK_SUCCESS)
    {
        error = "Rezonality could not create descriptors for pass '"
            + source.name + "'";
        return false;
    }
    return true;
}

std::optional<VulkanGeneration> create_generation(BackendState& backend,
    const ShaderBuild& build, const DraxulPluginVulkanFrameV2& frame,
    double animation_seconds, std::string& error)
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
    generation.width = static_cast<uint32_t>(std::max(1, frame.viewport.width));
    generation.height = static_cast<uint32_t>(std::max(1, frame.viewport.height));
    generation.buffered_frame_count
        = std::max(1u, frame.buffered_frame_count);
    for (const auto& surface : build.surfaces)
        if (!create_surface(generation, surface, generation.width,
                generation.height, error))
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
        generation.width, generation.height);
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
        depth_stencil.depthTestEnable = has_depth ? VK_TRUE : VK_FALSE;
        depth_stencil.depthWriteEnable = has_depth ? VK_TRUE : VK_FALSE;
        depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkGraphicsPipelineCreateInfo pipeline_info{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO
        };
        pipeline_info.stageCount = 2;
        pipeline_info.pStages = stages;
        pipeline_info.pVertexInputState = &vertex_input;
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
    DraxulPluginViewportV2 viewport{};
    std::unique_ptr<LiveProject> project;
    std::optional<ShaderBuild> pending_build;
    std::optional<ShaderBuild> active_build;
    BackendState backend;
    uint64_t attempted_generation = 0;
    uint64_t active_generation = 0;
    bool visible = true;
    bool focused = false;
    bool paused = false;
    bool quiesced = false;
    double animation_elapsed_seconds = 0.0;
    double last_animation_seconds = -1.0;
    std::string status = "building g1";
    std::string presentation_status;
};

void log(RezonalityInstance* instance, uint32_t level,
    const std::string& message)
{
    if (instance && instance->host && instance->host->log)
        instance->host->log(instance->host->host_context,
            level, message.data(), message.size());
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

std::string format_error(const BuildResult& result,
    uint64_t active_generation)
{
    std::string status = "error g" + std::to_string(result.generation);
    if (!result.diagnostic_path.empty())
    {
        status += ": " + result.diagnostic_path.filename().string();
        if (result.diagnostic_line > 0)
            status += ":" + std::to_string(result.diagnostic_line);
    }
    if (!result.error.empty())
        status += " " + result.error;
    if (active_generation != 0)
        status += " | live g" + std::to_string(active_generation);
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
    auto* raw = instance.get();
    instance->project = std::make_unique<LiveProject>(
        instance->plugin_directory, std::move(*options), [raw] {
            request_tick(raw);
        });
    instance->paused = instance->project->options().paused;
    instance->project->start();
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
        || event->struct_size < sizeof(DraxulPluginInputEventV2)
        || event->kind != DRAXUL_PLUGIN_INPUT_KEY
        || !event->pressed || event->logical_key != 32)
        return 0;
    instance->paused = !instance->paused;
    if (!instance->paused)
        request_redraw(instance);
    notify_presentation(instance);
    return 1;
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
            if (instance->visible)
                request_redraw(instance);
        }
        else
        {
            instance->status = format_error(
                *result, instance->active_generation);
            log(instance, DRAXUL_PLUGIN_LOG_ERROR, instance->status);
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
        static thread_local std::string error;
        error.clear();
        const uint64_t desired_generation = desired->generation;
        auto candidate = create_generation(
            instance->backend, *desired, *frame,
            animation_seconds, error);
        if (candidate)
        {
            if (instance->backend.active)
            {
                const uint64_t used
                    = instance->backend.active->used_slots;
                instance->backend.retired.push_back({
                    std::move(*instance->backend.active), used });
            }
            instance->backend.active = std::move(*candidate);
            instance->active_build = *desired;
            instance->active_generation = desired_generation;
            instance->pending_build.reset();
            instance->status = "live g"
                + std::to_string(instance->active_generation) + " | "
                + std::to_string(desired->passes.size()) + " passes | "
                + std::to_string(desired->surfaces.size()) + " surfaces";
            notify_presentation(instance);
        }
        else
        {
            instance->pending_build.reset();
            instance->status = "error g"
                + std::to_string(desired_generation) + ": " + error;
            if (instance->active_generation)
                instance->status += " | live g"
                    + std::to_string(instance->active_generation);
            log(instance, DRAXUL_PLUGIN_LOG_ERROR, instance->status);
            notify_presentation(instance);
        }
    }
    if (!instance->backend.active
        || instance->backend.active->format != target.pixelFormat)
        return render_result(true, DRAXUL_PLUGIN_NO_DEADLINE);

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
        instance->backend.active->height);
    std::memcpy(static_cast<uint8_t*>(
                    instance->backend.active->uniform_buffer.contents)
            + uniform_offset,
        uniform.data(), sizeof(uniform));
    for (const auto& scene_pass : instance->backend.active->passes)
    {
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
                        = MTLLoadActionClear;
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
                        ? MTLLoadActionClear : MTLLoadActionLoad;
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
        [encoder setVertexBuffer:instance->backend.vertex_buffer
                          offset:0 atIndex:0];
        [encoder setVertexBuffer:instance->backend.active->uniform_buffer
                          offset:uniform_offset atIndex:1];
        [encoder setFragmentBuffer:instance->backend.active->uniform_buffer
                            offset:uniform_offset atIndex:1];
        for (NSUInteger index = 0; index < scene_pass.samplers.size(); ++index)
        {
            const auto& surface = instance->backend.active->surfaces[
                scene_pass.samplers[index]];
            [encoder setFragmentTexture:surface.texture atIndex:index];
            [encoder setFragmentSamplerState:surface.repeat
                    ? instance->backend.active->repeat_sampler
                    : instance->backend.active->clamp_sampler
                                     atIndex:index];
        }
        const MTLViewport viewport{ static_cast<double>(origin_x),
            static_cast<double>(origin_y), static_cast<double>(width),
            static_cast<double>(height), 0.0, 1.0 };
        const MTLScissorRect scissor{ origin_x, origin_y, width, height };
        [encoder setViewport:viewport];
        [encoder setScissorRect:scissor];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                     vertexStart:0 vertexCount:6];
        [encoder endEncoding];
    }
    if (frame->frame_index < 64)
        instance->backend.active->used_slots
            |= uint64_t{ 1 } << frame->frame_index;
    return render_result(true, instance->paused
            ? DRAXUL_PLUGIN_NO_DEADLINE
            : draxul::plugin_support::kFrameDelayNs);
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
        static thread_local std::string error;
        error.clear();
        const uint64_t desired_generation = desired->generation;
        auto candidate = create_generation(
            instance->backend, *desired, *frame,
            animation_seconds, error);
        if (candidate)
        {
            if (instance->backend.active)
            {
                const uint64_t used
                    = instance->backend.active->used_slots;
                instance->backend.retired.push_back({
                    std::move(*instance->backend.active), used });
            }
            instance->backend.active = std::move(*candidate);
            instance->active_build = *desired;
            instance->active_generation = desired_generation;
            instance->pending_build.reset();
            instance->status = "live g"
                + std::to_string(instance->active_generation) + " | "
                + std::to_string(desired->passes.size()) + " passes | "
                + std::to_string(desired->surfaces.size()) + " surfaces";
            notify_presentation(instance);
        }
        else
        {
            instance->pending_build.reset();
            instance->status = "error g"
                + std::to_string(desired_generation) + ": " + error;
            if (instance->active_generation)
                instance->status += " | live g"
                    + std::to_string(instance->active_generation);
            log(instance, DRAXUL_PLUGIN_LOG_ERROR, instance->status);
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
    const uint32_t uniform_slot = frame->frame_index
        % instance->backend.active->buffered_frame_count;
    const size_t uniform_offset
        = instance->backend.active->uniform_stride * uniform_slot;
    const auto uniform = make_common_uniforms(
        animation_seconds,
        instance->backend.active->width,
        instance->backend.active->height);
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
            const auto& target = instance->backend.active->surfaces[
                pass.target_surfaces.front()];
            target_width = target.width;
            target_height = target.height;
        }
        std::vector<VkClearValue> clears(pass.target_surfaces.size());
        for (size_t index = 0; index < pass.target_surfaces.size(); ++index)
        {
            const auto& target = instance->backend.active->surfaces[
                pass.target_surfaces[index]];
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
        const VkDescriptorSet sets[]
            = { pass.uniform_set, pass.sampler_set };
        const uint32_t dynamic_offset
            = static_cast<uint32_t>(uniform_offset);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pass.layout, 0, 2, sets, 1, &dynamic_offset);
        vkCmdBindVertexBuffers(command, 0, 1,
            &instance->backend.vertex_buffer, &offset);
        vkCmdDraw(command, 6, 1, 0, 0);
        vkCmdEndRenderPass(command);
    }
    if (frame->frame_index < 64)
        instance->backend.active->used_slots
            |= uint64_t{ 1 } << frame->frame_index;
    return render_result(true, instance->paused
            ? DRAXUL_PLUGIN_NO_DEADLINE
            : draxul::plugin_support::kFrameDelayNs);
}

#endif

int32_t get_presentation_state(void* opaque,
    DraxulPluginPresentationStateV2* state)
{
    auto* instance = static_cast<RezonalityInstance*>(opaque);
    if (!instance || !state
        || state->struct_size < sizeof(DraxulPluginPresentationStateV2))
        return 0;
    instance->presentation_status = instance->status;
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

const DraxulPluginApiV2 kApi = draxul::plugin_support::make_plugin_api(
    { kPluginId, "Rezonality", "0.3.0",
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
        .query_extension = &Presentation::query_extension,
    });

} // namespace

extern "C" DRAXUL_PLUGIN_EXPORT const DraxulPluginApiV2*
draxul_plugin_query_v2(uint32_t requested_abi)
{
    return requested_abi == DRAXUL_PLUGIN_ABI_VERSION ? &kApi : nullptr;
}
