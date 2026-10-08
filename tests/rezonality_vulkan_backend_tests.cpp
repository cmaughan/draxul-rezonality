#include <catch2/catch_test_macros.hpp>

#include "camera.h"
#include "live_project.h"
#include "native_backend.h"
#include "scoped_env_var.h"

#include <draxul/vulkan/vk_plugin_allocator.h>
#include <draxul/vulkan/vk_resource_helpers.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{

namespace resources = draxul::vkresources;

void require_vulkan_prerequisite(bool available, const char* reason)
{
    if (available)
        return;
    const char* required = std::getenv("DRAXUL_REZONALITY_REQUIRE_VULKAN");
    if (required && std::strcmp(required, "1") == 0)
        FAIL(reason);
    SKIP(reason);
}

struct VulkanTestFrame
{
    draxul::tests::ScopedEnvVar synchronization_setting{
        "VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true" };
    draxul::tests::ScopedEnvVar submit_validation_setting{
        "VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION", "true" };
    draxul::tests::ScopedEnvVar load_store_validation_setting{
        "VK_KHRONOS_VALIDATION_SYNCVAL_LOAD_OP_AFTER_STORE_OP_VALIDATION", "true" };
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    resources::AttachmentResource target;
    resources::BufferResource readback;
    DraxulPluginVulkanFrameV2 frame{};
    VkPhysicalDeviceProperties properties{};
    std::mutex messages_mutex;
    std::vector<std::string> validation_errors;
    bool submitted = false;
    static constexpr uint32_t extent = 128;

    static VKAPI_ATTR VkBool32 VKAPI_CALL validation_callback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
    {
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        {
            auto& owner = *static_cast<VulkanTestFrame*>(user);
            std::lock_guard lock(owner.messages_mutex);
            if (owner.validation_errors.size() < 64)
                owner.validation_errors.emplace_back(data->pMessage);
        }
        return VK_FALSE;
    }

    void initialize()
    {
#ifndef VK_EXT_layer_settings
        require_vulkan_prerequisite(false,
            "Vulkan headers lack VK_EXT_layer_settings; synchronization/load-store validation cannot run");
#endif
        for (const char* name : {
                 "VK_KHRONOS_VALIDATION_VALIDATE_SYNC",
                 "VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION",
                 "VK_KHRONOS_VALIDATION_SYNCVAL_LOAD_OP_AFTER_STORE_OP_VALIDATION" })
        {
            const char* value = std::getenv(name);
            REQUIRE(value != nullptr);
            REQUIRE(std::string(value) == "true");
        }
        uint32_t layer_count = 0;
        REQUIRE(vkEnumerateInstanceLayerProperties(&layer_count, nullptr) == VK_SUCCESS);
        std::vector<VkLayerProperties> layers(layer_count);
        REQUIRE(vkEnumerateInstanceLayerProperties(&layer_count, layers.data()) == VK_SUCCESS);
        const auto validation_layer = std::find_if(layers.begin(), layers.end(), [](const auto& layer) {
            return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        require_vulkan_prerequisite(validation_layer != layers.end(),
            "VK_LAYER_KHRONOS_validation is unavailable; native Vulkan validation was not executed");
        require_vulkan_prerequisite(validation_layer->specVersion >= VK_MAKE_API_VERSION(0, 1, 4, 350),
            "Khronos layer 1.4.350 or newer is required for this verified load/store-validation fixture");

        const char* layer = "VK_LAYER_KHRONOS_validation";
        uint32_t extension_count = 0;
        REQUIRE(vkEnumerateInstanceExtensionProperties(layer, &extension_count, nullptr) == VK_SUCCESS);
        std::vector<VkExtensionProperties> layer_extensions(extension_count);
        REQUIRE(vkEnumerateInstanceExtensionProperties(layer, &extension_count, layer_extensions.data()) == VK_SUCCESS);
        const char* extensions[] = {
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
            "VK_EXT_layer_settings",
        };
        for (const char* required_extension : extensions)
        {
            INFO(required_extension);
            require_vulkan_prerequisite(std::any_of(layer_extensions.begin(), layer_extensions.end(),
                [required_extension](const auto& extension) {
                    return std::strcmp(extension.extensionName, required_extension) == 0;
                }), "Required Vulkan validation extension is unavailable; validation was not executed");
        }
        VkApplicationInfo application{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        application.pApplicationName = "Rezonality native Vulkan regression";
        application.apiVersion = VK_API_VERSION_1_2;
        VkDebugUtilsMessengerCreateInfoEXT debug{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = validation_callback;
        debug.pUserData = this;
        const void* validation_chain = &debug;
#ifdef VK_EXT_layer_settings
        const VkBool32 enabled = VK_TRUE;
        const VkLayerSettingEXT settings[] = {
            { layer, "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &enabled },
            { layer, "syncval_submit_time_validation", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &enabled },
            { layer, "syncval_load_op_after_store_op_validation", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &enabled },
        };
        VkLayerSettingsCreateInfoEXT validation{ VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT };
        validation.settingCount = 3;
        validation.pSettings = settings;
        validation.pNext = &debug;
        validation_chain = &validation;
#endif
        VkInstanceCreateInfo instance_info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        instance_info.pApplicationInfo = &application;
        instance_info.enabledLayerCount = 1;
        instance_info.ppEnabledLayerNames = &layer;
        instance_info.enabledExtensionCount = 2;
        instance_info.ppEnabledExtensionNames = extensions;
        instance_info.pNext = validation_chain;
        const auto instance_result = vkCreateInstance(&instance_info, nullptr, &instance);
        INFO("vkCreateInstance result: " << instance_result);
        require_vulkan_prerequisite(instance_result != VK_ERROR_INCOMPATIBLE_DRIVER
                && instance_result != VK_ERROR_LAYER_NOT_PRESENT
                && instance_result != VK_ERROR_EXTENSION_NOT_PRESENT,
            "Vulkan 1.2 driver or required validation layer/extensions unavailable; validation was not executed");
        REQUIRE(instance_result == VK_SUCCESS);
        const auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        REQUIRE(create_messenger != nullptr);
        REQUIRE(create_messenger(instance, &debug, nullptr, &messenger) == VK_SUCCESS);

        uint32_t device_count = 0;
        REQUIRE(vkEnumeratePhysicalDevices(instance, &device_count, nullptr) == VK_SUCCESS);
        std::vector<VkPhysicalDevice> devices(device_count);
        REQUIRE(vkEnumeratePhysicalDevices(instance, &device_count, devices.data()) == VK_SUCCESS);
        uint32_t family_index = 0;
        for (const auto candidate : devices)
        {
            VkPhysicalDeviceProperties candidate_properties{};
            vkGetPhysicalDeviceProperties(candidate, &candidate_properties);
            if (candidate_properties.apiVersion < VK_API_VERSION_1_2
                || candidate_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
                continue;
            uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
            for (uint32_t index = 0; index < family_count; ++index)
                if (families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                {
                    physical_device = candidate;
                    properties = candidate_properties;
                    family_index = index;
                    break;
                }
            if (physical_device)
                break;
        }
        require_vulkan_prerequisite(physical_device != VK_NULL_HANDLE,
            "No non-CPU Vulkan 1.2 graphics device is available; native GPU validation was not executed");
        std::cout << "Rezonality Vulkan device: " << properties.deviceName
                  << "; validate_sync=true; syncval_submit_time_validation=true; "
                     "syncval_load_op_after_store_op_validation=true "
                     "(VkLayerSettingsCreateInfoEXT and scoped environment)\n";
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queue_info.queueFamilyIndex = family_index;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        VkPhysicalDeviceBufferDeviceAddressFeatures address{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
        VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        features.pNext = &address;
        vkGetPhysicalDeviceFeatures2(physical_device, &features);
        VkDeviceCreateInfo device_info{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        device_info.pNext = &features;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        REQUIRE(vkCreateDevice(physical_device, &device_info, nullptr, &device) == VK_SUCCESS);
        vkGetDeviceQueue(device, family_index, 0, &queue);

        frame.struct_size = sizeof(frame);
        frame.instance = instance;
        frame.physical_device = physical_device;
        frame.device = device;
        frame.graphics_queue = queue;
        frame.graphics_queue_family = family_index;
        frame.buffered_frame_count = 2;
        frame.target_generation = 1;
        frame.target_format = VK_FORMAT_R8G8B8A8_UNORM;
        frame.depth_format = VK_FORMAT_D32_SFLOAT;
        frame.framebuffer_width = extent;
        frame.framebuffer_height = extent;
        frame.viewport = { sizeof(DraxulPluginViewportV2), 0, 0, 64, 64, 1.0f, 96.0f };
        std::string error;
        REQUIRE(draxul::plugin_support::ensure_allocator(allocator, frame, "Vulkan test", error));
        REQUIRE(resources::create_attachment(device, allocator,
            { extent, extent, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT, 0,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, resources::LifetimeScope::Persistent, "rezonality-test-target" },
            target, error));
        resources::ScopedBuffer pending_readback;
        REQUIRE(resources::create_buffer(device, allocator,
            { extent * extent * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                resources::MemoryPolicy::HostRandomAccess, "rezonality-test-readback", resources::LifetimeScope::Persistent },
            pending_readback, error));
        readback = pending_readback.release();
        REQUIRE(readback.mapped != nullptr);

        VkAttachmentDescription attachment{};
        attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;
        VkRenderPassCreateInfo pass_info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        pass_info.attachmentCount = 1;
        pass_info.pAttachments = &attachment;
        pass_info.subpassCount = 1;
        pass_info.pSubpasses = &subpass;
        REQUIRE(vkCreateRenderPass(device, &pass_info, nullptr, &render_pass) == VK_SUCCESS);
        VkFramebufferCreateInfo framebuffer_info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebuffer_info.renderPass = render_pass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &target.view;
        framebuffer_info.width = extent;
        framebuffer_info.height = extent;
        framebuffer_info.layers = 1;
        REQUIRE(vkCreateFramebuffer(device, &framebuffer_info, nullptr, &framebuffer) == VK_SUCCESS);
        frame.target_image = reinterpret_cast<uintptr_t>(target.image);
        frame.target_image_view = reinterpret_cast<uintptr_t>(target.view);
        frame.continuation_render_pass = reinterpret_cast<uintptr_t>(render_pass);
        frame.continuation_framebuffer = reinterpret_cast<uintptr_t>(framebuffer);

        VkCommandPoolCreateInfo pool_info{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = family_index;
        REQUIRE(vkCreateCommandPool(device, &pool_info, nullptr, &pool) == VK_SUCCESS);
        VkCommandBufferAllocateInfo allocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        REQUIRE(vkAllocateCommandBuffers(device, &allocate, &command) == VK_SUCCESS);
        frame.command_buffer = command;
        VkFenceCreateInfo fence_info{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        REQUIRE(vkCreateFence(device, &fence_info, nullptr, &fence) == VK_SUCCESS);
    }

    void record(rezonality::NativeBackend& backend, uint32_t slot)
    {
        REQUIRE_FALSE(submitted);
        REQUIRE(vkResetCommandBuffer(command, 0) == VK_SUCCESS);
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        REQUIRE(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
        resources::transition_image(command, { target.image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } });
        const VkClearColorValue clear{ { 0.0f, 0.0f, 0.0f, 1.0f } };
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(command, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        resources::transition_image(command, { target.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, range });
        frame.frame_index = slot;
        const auto recorded = backend.record(frame, nullptr, true);
        INFO((recorded.error_message ? recorded.error_message : "no backend error"));
        REQUIRE(recorded.ok);
        resources::transition_image(command, { target.image,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, range });
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { extent, extent, 1 };
        vkCmdCopyImageToBuffer(command, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
        VkMemoryBarrier host{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &host, 0, nullptr, 0, nullptr);
        REQUIRE(vkEndCommandBuffer(command) == VK_SUCCESS);
    }

    void submit()
    {
        REQUIRE(vkResetFences(device, 1, &fence) == VK_SUCCESS);
        VkSubmitInfo submit_info{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command;
        REQUIRE(vkQueueSubmit(queue, 1, &submit_info, fence) == VK_SUCCESS);
        submitted = true;
    }

    std::vector<uint8_t> wait()
    {
        REQUIRE(submitted);
        REQUIRE(vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000ULL) == VK_SUCCESS);
        submitted = false;
        REQUIRE(vmaInvalidateAllocation(allocator, readback.allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
        const auto* bytes = static_cast<const uint8_t*>(readback.mapped);
        return { bytes, bytes + extent * extent * 4 };
    }

    void check_validation()
    {
        std::lock_guard lock(messages_mutex);
        for (const auto& error : validation_errors)
            UNSCOPED_INFO(error);
        CHECK(validation_errors.empty());
    }

    ~VulkanTestFrame()
    {
        if (device)
        {
            vkDeviceWaitIdle(device);
            vkDestroyFence(device, fence, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyFramebuffer(device, framebuffer, nullptr);
            vkDestroyRenderPass(device, render_pass, nullptr);
            resources::destroy_buffer(allocator, readback);
            resources::destroy_attachment(device, allocator, target);
            draxul::plugin_support::destroy_allocator(allocator);
            vkDestroyDevice(device, nullptr);
        }
        if (messenger)
        {
            const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            destroy(instance, messenger, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }
};

struct SubmissionDrain
{
    VulkanTestFrame& gpu;

    ~SubmissionDrain()
    {
        if (gpu.submitted)
            vkDeviceWaitIdle(gpu.device);
    }
};

rezonality::ShaderBuild robot_build()
{
    const auto root = std::filesystem::path(DRAXUL_PROJECT_ROOT) / "plugins" / "rezonality";
    rezonality::ProjectOptions options;
    options.project_path = root / "examples" / "pbr_robot";
    const rezonality::ProjectPipeline pipeline(root, options);
    auto built = pipeline.build(1);
    INFO(built.error);
    REQUIRE(built.build);
    return std::move(*built.build);
}

double prepare(rezonality::NativeBackend& backend, const rezonality::ShaderBuild& build)
{
    const auto start = std::chrono::steady_clock::now();
    const auto result = backend.prepare(build);
    if (result.ready)
        backend.activate_prepared();
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    INFO(result.error);
    REQUIRE(result.ready);
    REQUIRE(backend.active_compatible());
    return elapsed_ms;
}

void report_stats(const char* phase, const rezonality::BackendResourceStats& stats)
{
    std::cout << "Rezonality Vulkan " << phase
              << ": generations=" << stats.generations_prepared
              << " models_created=" << stats.models_created
              << " programs_created=" << stats.programs_created
              << " surfaces_created=" << stats.surfaces_created
              << " asset_upload_bytes=" << stats.asset_upload_bytes
              << " retained_upload_staging_bytes=" << stats.retained_upload_staging_bytes << '\n';
}

void measure_resize_preparation(VulkanTestFrame& gpu,
    rezonality::ShaderBuild build, bool force_rebuild)
{
    constexpr uint32_t sample_count = 10;
    rezonality::Camera camera;
    rezonality::NativeBackend backend;
    SubmissionDrain drain{ gpu };
    gpu.frame.viewport.width = 64;
    gpu.frame.viewport.height = 64;
    backend.bind_frame(gpu.frame, 0.0, camera);
    prepare(backend, build);
    const auto initial = backend.resource_stats();
    uint64_t staging_peak = initial.retained_upload_staging_bytes;
    gpu.record(backend, 0);
    gpu.submit();
    gpu.wait();

    std::vector<double> samples;
    samples.reserve(sample_count);
    for (uint32_t index = 0; index < sample_count; ++index)
    {
        const uint32_t slot = (index + 1) % gpu.frame.buffered_frame_count;
        backend.retire_completed_slot(slot);
        gpu.frame.viewport.width = index % 2 == 0 ? 96 : 64;
        gpu.frame.viewport.height = index % 2 == 0 ? 80 : 64;
        if (force_rebuild)
            ++build.generation;
        samples.push_back(prepare(backend, build));
        const auto stats = backend.resource_stats();
        staging_peak = std::max(staging_peak, stats.retained_upload_staging_bytes);
        CHECK(stats.last_reuse == (force_rebuild ? rezonality::GenerationReuse::Rebuild
                                              : rezonality::GenerationReuse::ResizeTargets));
        CHECK(stats.asset_upload_bytes == initial.asset_upload_bytes * (force_rebuild ? index + 2 : 1));
        gpu.record(backend, slot);
        gpu.submit();
        gpu.wait();
    }
    for (uint32_t slot = 0; slot < gpu.frame.buffered_frame_count; ++slot)
        backend.retire_completed_slot(slot);
    CHECK(backend.resource_stats().retained_upload_staging_bytes == 0);

    const char* phase = force_rebuild ? "forced-full-regeneration" : "size-only-reuse";
    std::cout << "Rezonality Vulkan prepare+activate " << phase << " samples_ms=";
    for (const double sample : samples)
        std::cout << sample << ',';
    std::sort(samples.begin(), samples.end());
    const double median = (samples[samples.size() / 2 - 1] + samples[samples.size() / 2]) * 0.5;
    const size_t p95_index = (samples.size() * 95 + 99) / 100 - 1;
    std::cout << " median_ms=" << median << " p95_ms=" << samples[p95_index]
              << " sampled_retained_upload_staging_peak_bytes=" << staging_peak << '\n';
    report_stats(phase, backend.resource_stats());
}

}

TEST_CASE("Rezonality Vulkan resize reuses assets and retires submitted staging",
    "[rezonality][vulkan][resize][staging]")
{
    VulkanTestFrame gpu;
    gpu.initialize();
    auto build = robot_build();
    REQUIRE(build.models.size() == 1);
    size_t static_images = 0;
    for (size_t index = 0; index < build.surfaces.size(); ++index)
        static_images += rezonality::surface_is_viewport_independent(build, index) ? 1 : 0;
    REQUIRE(static_images == 1);
    const auto viewport_surfaces = build.surfaces.size() - static_images;
    {
        rezonality::Camera camera;
        rezonality::NativeBackend backend;
        SubmissionDrain drain{ gpu };
        backend.bind_frame(gpu.frame, 0.0, camera);
        prepare(backend, build);
        const auto initial = backend.resource_stats();
        report_stats("initial", initial);
        CHECK(initial.last_reuse == rezonality::GenerationReuse::Rebuild);
        CHECK(initial.generations_prepared == 1);
        CHECK(initial.models_created == 1);
        CHECK(initial.programs_created == build.passes.size());
        CHECK(initial.surfaces_created == build.surfaces.size());
        CHECK(initial.asset_upload_bytes > 0);
        REQUIRE(initial.retained_upload_staging_bytes > 0);
        gpu.record(backend, 0);
        CHECK(backend.resource_stats().retained_upload_staging_bytes == initial.retained_upload_staging_bytes);
        gpu.submit();
        backend.retire_completed_slot(1);
        CHECK(backend.resource_stats().retained_upload_staging_bytes == initial.retained_upload_staging_bytes);
        const auto reference = gpu.wait();
        CHECK(std::any_of(reference.begin(), reference.end(), [](uint8_t value) { return value != 0 && value != 255; }));
        CHECK(backend.resource_stats().retained_upload_staging_bytes == initial.retained_upload_staging_bytes);
        backend.retire_completed_slot(0);
        CHECK(backend.resource_stats().retained_upload_staging_bytes == 0);
        report_stats("upload-completed", backend.resource_stats());
        gpu.record(backend, 1);
        gpu.submit();
        CHECK(gpu.wait() == reference);
        backend.retire_completed_slot(1);

        for (uint32_t step = 0; step < 8; ++step)
        {
            gpu.frame.viewport.width = step % 2 == 0 ? 96 : 64;
            gpu.frame.viewport.height = step % 2 == 0 ? 80 : 64;
            CHECK_FALSE(backend.active_compatible());
            prepare(backend, build);
            const auto resized = backend.resource_stats();
            CHECK(resized.last_reuse == rezonality::GenerationReuse::ResizeTargets);
            CHECK(resized.models_created == initial.models_created);
            CHECK(resized.programs_created == initial.programs_created);
            CHECK(resized.asset_upload_bytes == initial.asset_upload_bytes);
            CHECK(resized.retained_upload_staging_bytes == 0);
            CHECK(resized.models_reused == step + 1);
            CHECK(resized.programs_reused == (step + 1) * build.passes.size());
            CHECK(resized.surfaces_created == initial.surfaces_created + (step + 1) * viewport_surfaces);
            CHECK(resized.surfaces_reused == (step + 1) * static_images);
            gpu.record(backend, step % 2);
            gpu.submit();
            const auto pixels = gpu.wait();
            if (step % 2 == 1)
                CHECK(pixels == reference);
            backend.retire_completed_slot(step % 2);
        }
        report_stats("eight-resizes", backend.resource_stats());

        gpu.frame.viewport.width = static_cast<int32_t>(gpu.properties.limits.maxImageDimension2D + 1);
        const auto rejected = backend.prepare(build);
        INFO(rejected.error);
        CHECK_FALSE(rejected.ready);
        CHECK(rejected.error.find("exceeds the device 2D texture limit") != std::string::npos);
        gpu.frame.viewport.width = 64;
        REQUIRE(backend.active_compatible());
        gpu.record(backend, 0);
        gpu.submit();
        CHECK(gpu.wait() == reference);
        backend.retire_completed_slot(0);

        ++gpu.frame.target_generation;
        prepare(backend, build);
        const auto retargeted = backend.resource_stats();
        CHECK(retargeted.last_reuse == rezonality::GenerationReuse::ReuseAssets);
        CHECK(retargeted.models_created == initial.models_created);
        CHECK(retargeted.programs_created == 2 * initial.programs_created);
        CHECK(retargeted.asset_upload_bytes == initial.asset_upload_bytes);
        CHECK(retargeted.retained_upload_staging_bytes == 0);
        gpu.record(backend, 1);
        gpu.submit();
        CHECK(gpu.wait() == reference);
        backend.retire_completed_slot(1);
        report_stats("new-target-generation", backend.resource_stats());

        ++build.generation;
        prepare(backend, build);
        const auto reloaded = backend.resource_stats();
        CHECK(reloaded.last_reuse == rezonality::GenerationReuse::Rebuild);
        CHECK(reloaded.models_created == 2 * initial.models_created);
        CHECK(reloaded.asset_upload_bytes == 2 * initial.asset_upload_bytes);
        CHECK(reloaded.retained_upload_staging_bytes == initial.retained_upload_staging_bytes);
        report_stats("source-reload", reloaded);
        gpu.record(backend, 0);
        gpu.submit();
        backend.retire_completed_slot(1);
        CHECK(backend.resource_stats().retained_upload_staging_bytes == initial.retained_upload_staging_bytes);
        CHECK(gpu.wait() == reference);
        backend.retire_completed_slot(0);
        CHECK(backend.resource_stats().retained_upload_staging_bytes == 0);
        report_stats("reload-completed", backend.resource_stats());
        gpu.record(backend, 1);
        gpu.submit();
        CHECK(gpu.wait() == reference);
        backend.retire_completed_slot(1);
    }
    gpu.check_validation();
}

TEST_CASE("Rezonality Vulkan discarded first upload preserves texture initialization",
    "[.][rezonality][vulkan][discarded-upload]")
{
    VulkanTestFrame gpu;
    gpu.initialize();
    const auto build = robot_build();
    {
        rezonality::Camera camera;
        rezonality::NativeBackend backend;
        SubmissionDrain drain{ gpu };
        backend.bind_frame(gpu.frame, 0.0, camera);
        prepare(backend, build);
        gpu.record(backend, 0);
        REQUIRE(vkResetCommandBuffer(gpu.command, 0) == VK_SUCCESS);
        backend.retire_completed_slot(0);
        ++gpu.frame.target_generation;
        prepare(backend, build);
        gpu.record(backend, 0);
        gpu.submit();
        gpu.wait();
        backend.retire_completed_slot(0);
    }
    gpu.check_validation();
}

TEST_CASE("Rezonality Vulkan measures forced rebuild versus size-only preparation",
    "[.][rezonality][vulkan][resize-timing]")
{
    VulkanTestFrame gpu;
    gpu.initialize();
    const auto build = robot_build();
    measure_resize_preparation(gpu, build, true);
    measure_resize_preparation(gpu, build, false);
    gpu.check_validation();
}
