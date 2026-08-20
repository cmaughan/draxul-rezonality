set(_rezonality_root "${CMAKE_CURRENT_LIST_DIR}/..")
file(GLOB _rezonality_test_sources CONFIGURE_DEPENDS
    "${_rezonality_root}/tests/rezonality_*_tests.cpp")

draxul_add_test_target(
    draxul-test-rezonality rezonality 1
    ${_rezonality_test_sources}
    "${_rezonality_root}/src/audio_analysis.cpp"
    "${_rezonality_root}/src/camera.cpp"
    "${_rezonality_root}/src/diagnostics.cpp"
    "${_rezonality_root}/src/image_loader.cpp"
    "${_rezonality_root}/src/live_project.cpp"
    "${_rezonality_root}/src/model_loader.cpp"
    "${_rezonality_root}/src/rezonality_plugin.cpp")
if(APPLE)
    # The shared plugin entry point contains Metal and Foundation syntax, so
    # compile it with the same Objective-C++ language used by the product.
    set_source_files_properties(
        "${_rezonality_root}/src/rezonality_plugin.cpp"
        PROPERTIES LANGUAGE OBJCXX)
    target_sources(draxul-test-rezonality PRIVATE
        "${_rezonality_root}/src/mic_permission.mm")
else()
    target_sources(draxul-test-rezonality PRIVATE
        "${_rezonality_root}/src/mic_permission_stub.cpp")
endif()
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
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME draxul-rezonality-agent-layout
    COMMAND ${Python3_EXECUTABLE}
        "${_rezonality_root}/tests/rezonality_layout_integration.py"
        --draxul "$<TARGET_FILE:draxul>"
        --python "${Python3_EXECUTABLE}"
        --generator "${_rezonality_root}/tools/rezonality_layout.py"
        --project "${_rezonality_root}/examples/simple")
set_tests_properties(draxul-rezonality-agent-layout PROPERTIES
    LABELS "rezonality;integration"
    RUN_SERIAL TRUE
    TIMEOUT 60)
find_program(REZONALITY_NVIM_EXECUTABLE nvim)
if(REZONALITY_NVIM_EXECUTABLE)
    add_test(NAME draxul-rezonality-neovim
        COMMAND ${Python3_EXECUTABLE}
            "${_rezonality_root}/tests/rezonality_neovim_integration.py"
            --nvim "${REZONALITY_NVIM_EXECUTABLE}"
            --installer "${_rezonality_root}/tools/install_neovim.py"
            --script "${_rezonality_root}/tests/rezonality_neovim_test.lua")
    set_tests_properties(draxul-rezonality-neovim PROPERTIES
        LABELS "rezonality;integration;nvim"
        TIMEOUT 30)
endif()
add_dependencies(draxul-test-rezonality draxul)
target_include_directories(draxul-test-rezonality PRIVATE
    "${_rezonality_root}/src"
    ${stb_SOURCE_DIR})
if(APPLE)
    # Unlike the product module, this test is an executable and therefore owns
    # the SDL implementation that its audio-analysis cases call.
    target_link_libraries(draxul-test-rezonality PRIVATE SDL3::SDL3)
    set_source_files_properties("${_rezonality_root}/src/mic_permission.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(draxul-test-rezonality PRIVATE
        spirv-cross-msl
        "-framework Metal"
        "-framework Foundation"
        ${REZONALITY_AVFOUNDATION_FRAMEWORK})
else()
    target_link_libraries(draxul-test-rezonality PRIVATE
        SDL3::SDL3
        Vulkan::Vulkan
        Draxul::PluginSupport::VulkanResources
        GPUOpen::VulkanMemoryAllocator)
endif()
