#include "live_project.h"

#include <draxul/plugin_adapter.h>
#include <draxul/plugin_api.h>

#if defined(__APPLE__)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <spirv_msl.hpp>
#else
#include <vulkan/vulkan.h>
#endif

#include <algorithm>
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
    id<MTLRenderPipelineState> pipeline = nil;
    MTLPixelFormat format = MTLPixelFormatInvalid;
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

    std::string vertex_source;
    std::string fragment_source;
    if (!convert_to_msl(build.vertex_spirv, spv::ExecutionModelVertex,
            "rezonality_vertex", vertex_source, error)
        || !convert_to_msl(build.fragment_spirv,
            spv::ExecutionModelFragment, "rezonality_fragment",
            fragment_source, error))
        return std::nullopt;

    NSError* compile_error = nil;
    id<MTLLibrary> vertex_library = [device
        newLibraryWithSource:[NSString
                                 stringWithUTF8String:vertex_source.c_str()]
        options:nil error:&compile_error];
    if (!vertex_library)
    {
        error = "Metal vertex compilation failed: "
            + ns_error(compile_error);
        return std::nullopt;
    }
    id<MTLLibrary> fragment_library = [device
        newLibraryWithSource:[NSString
                                 stringWithUTF8String:fragment_source.c_str()]
        options:nil error:&compile_error];
    if (!fragment_library)
    {
        error = "Metal fragment compilation failed: "
            + ns_error(compile_error);
        return std::nullopt;
    }

    id<MTLFunction> vertex
        = [vertex_library newFunctionWithName:@"rezonality_vertex"];
    id<MTLFunction> fragment
        = [fragment_library newFunctionWithName:@"rezonality_fragment"];
    if (!vertex || !fragment)
    {
        error = "Translated Metal entry points are missing";
        return std::nullopt;
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

    MTLRenderPipelineDescriptor* descriptor
        = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.vertexDescriptor = vertices;
    descriptor.colorAttachments[0].pixelFormat = target.pixelFormat;
    id<MTLRenderPipelineState> pipeline = [device
        newRenderPipelineStateWithDescriptor:descriptor
                                       error:&compile_error];
    if (!pipeline)
    {
        error = "Rezonality Metal pipeline failed: "
            + ns_error(compile_error);
        return std::nullopt;
    }

    MetalGeneration generation;
    generation.pipeline = pipeline;
    generation.format = target.pixelFormat;
    generation.source_generation = build.generation;
    return generation;
}

#else

struct VulkanGeneration
{
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
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
    std::optional<VulkanGeneration> active;
    std::vector<RetiredGeneration> retired;
};

void destroy_generation(VulkanGeneration& generation)
{
    if (generation.pipeline)
        vkDestroyPipeline(generation.device, generation.pipeline, nullptr);
    if (generation.pipeline_layout)
        vkDestroyPipelineLayout(
            generation.device, generation.pipeline_layout, nullptr);
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

std::optional<VulkanGeneration> create_generation(BackendState& backend,
    const ShaderBuild& build, const DraxulPluginVulkanFrameV2& frame,
    std::string& error)
{
    if (!ensure_vertex_buffer(backend, frame, error))
        return std::nullopt;
    VulkanGeneration generation;
    generation.device = static_cast<VkDevice>(frame.device);
    generation.render_pass = reinterpret_cast<VkRenderPass>(
        static_cast<uintptr_t>(frame.continuation_render_pass));
    generation.target_generation = frame.target_generation;
    generation.source_generation = build.generation;

    const VkShaderModule vertex
        = create_shader(generation.device, build.vertex_spirv);
    const VkShaderModule fragment
        = create_shader(generation.device, build.fragment_spirv);
    if (!vertex || !fragment)
    {
        if (vertex)
            vkDestroyShaderModule(generation.device, vertex, nullptr);
        if (fragment)
            vkDestroyShaderModule(generation.device, fragment, nullptr);
        error = "Rezonality could not create Vulkan shader modules";
        return std::nullopt;
    }

    VkPipelineLayoutCreateInfo layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    if (vkCreatePipelineLayout(generation.device, &layout_info, nullptr,
            &generation.pipeline_layout)
        != VK_SUCCESS)
    {
        vkDestroyShaderModule(generation.device, vertex, nullptr);
        vkDestroyShaderModule(generation.device, fragment, nullptr);
        error = "Rezonality could not create a Vulkan pipeline layout";
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
    VkPipelineColorBlendAttachmentState color_attachment{};
    color_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT
        | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
        | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO
    };
    blend.attachmentCount = 1;
    blend.pAttachments = &color_attachment;
    const VkDynamicState states[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO
    };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
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
    pipeline_info.layout = generation.pipeline_layout;
    pipeline_info.renderPass = generation.render_pass;
    pipeline_info.subpass = 0;
    const VkResult pipeline_result = vkCreateGraphicsPipelines(
        generation.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
        &generation.pipeline);
    vkDestroyShaderModule(generation.device, vertex, nullptr);
    vkDestroyShaderModule(generation.device, fragment, nullptr);
    if (pipeline_result != VK_SUCCESS)
    {
        destroy_generation(generation);
        error = "Rezonality could not create a Vulkan graphics pipeline";
        return std::nullopt;
    }
    return generation;
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
    bool quiesced = false;
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

int32_t handle_input(void*, const DraxulPluginInputEventV2*)
{
    return 0;
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
    retire_completed_slot(instance->backend, frame->frame_index);

    id<MTLTexture> target
        = (__bridge id<MTLTexture>)frame->drawable_texture;
    const ShaderBuild* desired = nullptr;
    if (instance->pending_build)
        desired = &*instance->pending_build;
    else if (instance->active_build
        && (!instance->backend.active
            || instance->backend.active->format != target.pixelFormat))
        desired = &*instance->active_build;
    if (desired)
    {
        static thread_local std::string error;
        error.clear();
        const uint64_t desired_generation = desired->generation;
        auto candidate = create_generation(
            instance->backend, *desired, *frame, error);
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
                + std::to_string(instance->active_generation);
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
    id<MTLRenderCommandEncoder> encoder
        = [command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:instance->backend.active->pipeline];
    [encoder setVertexBuffer:instance->backend.vertex_buffer
                      offset:0 atIndex:0];
    const MTLViewport viewport{
        static_cast<double>(frame->viewport.x),
        static_cast<double>(frame->viewport.y),
        static_cast<double>(frame->viewport.width),
        static_cast<double>(frame->viewport.height), 0.0, 1.0
    };
    [encoder setViewport:viewport];
    const MTLScissorRect scissor{
        static_cast<NSUInteger>(std::max(0, frame->viewport.x)),
        static_cast<NSUInteger>(std::max(0, frame->viewport.y)),
        static_cast<NSUInteger>(std::max(0, frame->viewport.width)),
        static_cast<NSUInteger>(std::max(0, frame->viewport.height))
    };
    [encoder setScissorRect:scissor];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                 vertexStart:0 vertexCount:6];
    [encoder endEncoding];
    if (frame->frame_index < 64)
        instance->backend.active->used_slots
            |= uint64_t{ 1 } << frame->frame_index;
    return render_result(true, DRAXUL_PLUGIN_NO_DEADLINE);
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
            || instance->backend.active->render_pass != render_pass))
        desired = &*instance->active_build;
    if (desired)
    {
        static thread_local std::string error;
        error.clear();
        const uint64_t desired_generation = desired->generation;
        auto candidate = create_generation(
            instance->backend, *desired, *frame, error);
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
                + std::to_string(instance->active_generation);
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
    VkRenderPassBeginInfo begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    begin.renderPass = render_pass;
    begin.framebuffer = reinterpret_cast<VkFramebuffer>(
        static_cast<uintptr_t>(frame->continuation_framebuffer));
    begin.renderArea.extent = {
        static_cast<uint32_t>(frame->framebuffer_width),
        static_cast<uint32_t>(frame->framebuffer_height)
    };
    vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{};
    viewport.x = static_cast<float>(frame->viewport.x);
    viewport.y = static_cast<float>(frame->viewport.y);
    viewport.width = static_cast<float>(frame->viewport.width);
    viewport.height = static_cast<float>(frame->viewport.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{
        { std::max(0, frame->viewport.x),
            std::max(0, frame->viewport.y) },
        { static_cast<uint32_t>(std::max(0, frame->viewport.width)),
            static_cast<uint32_t>(std::max(0, frame->viewport.height)) }
    };
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
        instance->backend.active->pipeline);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(command, 0, 1,
        &instance->backend.vertex_buffer, &offset);
    vkCmdDraw(command, 6, 1, 0, 0);
    vkCmdEndRenderPass(command);
    if (frame->frame_index < 64)
        instance->backend.active->used_slots
            |= uint64_t{ 1 } << frame->frame_index;
    return render_result(true, DRAXUL_PLUGIN_NO_DEADLINE);
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
    { kPluginId, "Rezonality", "0.2.0",
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
