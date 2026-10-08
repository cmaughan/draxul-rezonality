set(_rezonality_root "${CMAKE_CURRENT_LIST_DIR}/..")

# Test kinds and their link closures:
# - draxul-test-rezonality-project/-runtime/-audio: pure CPU, policy, and
#   capture cases against one product library each; no plugin ABI or GPU.
# - draxul-test-rezonality-native: in-process ABI export, live-edit, and
#   example-inventory cases (plus Metal preparation limits on macOS). It links
#   the module's compiled objects (draxul-rezonality-native) instead of
#   recompiling them and owns SDL3::SDL3 because it is an executable.
# - draxul-test-rezonality: loads the built module through the exported ABI.
#   It compiles no Rezonality implementation. It is the only serial suite:
#   its real-module edit/reload sequences are timing-sensitive file watching
#   and shader compilation under the dynamically loaded module.
# Add new cases to the source list of the matching target.
set(_rezonality_native_test_sources
    "${_rezonality_root}/tests/rezonality_native_contract_tests.cpp")
if(APPLE)
    # Exercises real Metal preparation limits against a system device.
    list(APPEND _rezonality_native_test_sources
        "${_rezonality_root}/tests/rezonality_metal_backend_tests.mm")
else()
    list(APPEND _rezonality_native_test_sources
        "${_rezonality_root}/tests/rezonality_vulkan_backend_tests.cpp")
endif()
draxul_add_test_target(
    draxul-test-rezonality-native rezonality 1
    ${_rezonality_native_test_sources})
target_link_libraries(draxul-test-rezonality-native PRIVATE
    draxul-rezonality-native
    nlohmann_json::nlohmann_json
    SDL3::SDL3)
target_compile_definitions(draxul-test-rezonality-native PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE)
if(APPLE)
    target_link_libraries(draxul-test-rezonality-native PRIVATE
        ${REZONALITY_AVFOUNDATION_FRAMEWORK})
endif()

draxul_add_test_target(
    draxul-test-rezonality rezonality 1
    "${_rezonality_root}/tests/rezonality_plugin_contract_tests.cpp")
target_link_libraries(draxul-test-rezonality PRIVATE
    Draxul::PluginSDK
    nlohmann_json::nlohmann_json
    ${CMAKE_DL_LIBS})
target_compile_definitions(draxul-test-rezonality PRIVATE
    DRAXUL_REZONALITY_MODULE_PATH="$<TARGET_FILE:draxul-rezonality-plugin>")
if(APPLE)
    # The module resolves SDL from its host executable at dlopen
    # (RTLD_NOW). This loader owns no SDL calls itself, so force the complete
    # static SDL ABI into the executable as the Draxul host does.
    target_link_options(draxul-test-rezonality PRIVATE
        "LINKER:-force_load,$<TARGET_FILE:SDL3::SDL3>")
    target_link_libraries(draxul-test-rezonality PRIVATE SDL3::SDL3)
endif()
add_dependencies(draxul-test-rezonality draxul-rezonality-plugin draxul)
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

draxul_add_test_target(
    draxul-test-rezonality-project rezonality 1
    "${_rezonality_root}/tests/rezonality_project_tests.cpp"
    "${_rezonality_root}/tests/rezonality_project_support_tests.cpp")
target_link_libraries(draxul-test-rezonality-project PRIVATE
    draxul-rezonality-project
    Draxul::PluginSupport::Types
    nlohmann_json::nlohmann_json)
target_compile_definitions(draxul-test-rezonality-project PRIVATE
    "DRAXUL_REZONALITY_TEST_ROOT=\"${_rezonality_root}\"")

draxul_add_test_target(
    draxul-test-rezonality-runtime rezonality 1
    "${_rezonality_root}/tests/rezonality_runtime_tests.cpp"
    "${_rezonality_root}/tests/rezonality_backend_policy_tests.cpp")
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
