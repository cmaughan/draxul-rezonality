set(_rezonality_root "${CMAKE_CURRENT_LIST_DIR}/..")
if(APPLE)
    set(_rezonality_test_native_backend_source
        "${_rezonality_root}/src/native_backend_metal.mm")
else()
    set(_rezonality_test_native_backend_source
        "${_rezonality_root}/src/native_backend_vulkan.cpp")
endif()
draxul_add_test_target(
    draxul-test-rezonality rezonality 1
    "${_rezonality_root}/tests/rezonality_plugin_contract_tests.cpp"
    "${_rezonality_root}/src/rezonality_plugin.cpp"
    "${_rezonality_test_native_backend_source}")
target_link_libraries(draxul-test-rezonality PRIVATE
    Draxul::PluginSDK
    Draxul::PluginSupport::Adapter
    draxul-rezonality-project
    draxul-rezonality-runtime
    draxul-rezonality-audio
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
            --draxul "$<TARGET_FILE:draxul>"
            --installer "${_rezonality_root}/tools/install_neovim.py"
            --script "${_rezonality_root}/tests/rezonality_neovim_test.lua")
    set_tests_properties(draxul-rezonality-neovim PROPERTIES
        LABELS "rezonality;integration;nvim"
        TIMEOUT 30)
endif()
add_dependencies(draxul-test-rezonality draxul)
if(APPLE)
    # Unlike the product module, this test is an executable and therefore owns
    # the SDL implementation that its audio-analysis cases call.
    target_link_libraries(draxul-test-rezonality PRIVATE SDL3::SDL3)
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

draxul_add_test_target(
    draxul-test-rezonality-project rezonality 1
    "${_rezonality_root}/tests/rezonality_project_tests.cpp")
target_link_libraries(draxul-test-rezonality-project PRIVATE
    draxul-rezonality-project
    Draxul::PluginSupport::Types)
target_compile_definitions(draxul-test-rezonality-project PRIVATE
    "DRAXUL_REZONALITY_TEST_ROOT=\"${_rezonality_root}\"")

draxul_add_test_target(
    draxul-test-rezonality-runtime rezonality 1
    "${_rezonality_root}/tests/rezonality_runtime_tests.cpp")
target_link_libraries(draxul-test-rezonality-runtime PRIVATE
    draxul-rezonality-runtime
    Draxul::PluginSupport::Types)

draxul_add_test_target(
    draxul-test-rezonality-audio rezonality 1
    "${_rezonality_root}/tests/rezonality_audio_tests.cpp")
target_link_libraries(draxul-test-rezonality-audio PRIVATE
    draxul-rezonality-audio
    Draxul::PluginSupport::Types)
if(APPLE)
    # Product modules resolve SDL from the host. The focused executable owns
    # the implementation for the audio capture code it links statically.
    target_link_libraries(draxul-test-rezonality-audio PRIVATE SDL3::SDL3)
endif()
