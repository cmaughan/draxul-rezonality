set(_rezonality_root "${CMAKE_CURRENT_LIST_DIR}/..")
file(GLOB _rezonality_test_sources CONFIGURE_DEPENDS
    "${_rezonality_root}/tests/rezonality_*_tests.cpp")

draxul_add_test_target(
    draxul-test-rezonality rezonality 1
    ${_rezonality_test_sources}
    "${_rezonality_root}/src/camera.cpp"
    "${_rezonality_root}/src/image_loader.cpp"
    "${_rezonality_root}/src/live_project.cpp"
    "${_rezonality_root}/src/model_loader.cpp"
    "${_rezonality_root}/src/rezonality_plugin.cpp")
target_link_libraries(draxul-test-rezonality PRIVATE
    Draxul::PluginSDK
    Draxul::PluginSupport::Adapter
    assimp::assimp
    glm::glm
    nlohmann_json::nlohmann_json
    ${CMAKE_DL_LIBS})
target_compile_definitions(draxul-test-rezonality PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    DRAXUL_REZONALITY_MODULE_PATH="$<TARGET_FILE:draxul-rezonality-plugin>")
add_dependencies(draxul-test-rezonality draxul-rezonality-plugin)
set_tests_properties(draxul-test-rezonality-shard-0
    PROPERTIES RUN_SERIAL TRUE)
target_include_directories(draxul-test-rezonality PRIVATE
    "${_rezonality_root}/src"
    ${stb_SOURCE_DIR})
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
