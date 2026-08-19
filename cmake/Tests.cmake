set(_rezonality_root "${CMAKE_CURRENT_LIST_DIR}/..")
file(GLOB _rezonality_test_sources CONFIGURE_DEPENDS
    "${_rezonality_root}/tests/rezonality_*_tests.cpp")

draxul_add_test_target(
    draxul-test-rezonality rezonality 1
    ${_rezonality_test_sources}
    "${_rezonality_root}/src/image_loader.cpp"
    "${_rezonality_root}/src/live_project.cpp"
    "${_rezonality_root}/src/rezonality_plugin.cpp")
target_link_libraries(draxul-test-rezonality PRIVATE
    Draxul::PluginSDK
    Draxul::PluginSupport::Adapter
    nlohmann_json::nlohmann_json)
target_include_directories(draxul-test-rezonality PRIVATE ${stb_SOURCE_DIR})
if(APPLE)
    target_link_libraries(draxul-test-rezonality PRIVATE
        spirv-cross-msl
        "-framework Metal"
        "-framework Foundation")
else()
    target_link_libraries(draxul-test-rezonality PRIVATE
        Vulkan::Vulkan
        Draxul::PluginSupport::VulkanResources
        GPUOpen::VulkanMemoryAllocator)
endif()
