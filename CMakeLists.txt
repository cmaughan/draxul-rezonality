cmake_minimum_required(VERSION 3.25)

# Rezonality currently builds as a mounted Draxul product. Keeping this root
# deliberately small makes the ownership seam explicit while the VkLive core
# is imported in functional slices.
if(NOT TARGET Draxul::PluginSDK)
    message(FATAL_ERROR
        "Rezonality must currently be configured from a Draxul checkout")
endif()

add_library(draxul-rezonality-plugin MODULE
    src/rezonality_plugin.cpp)
target_link_libraries(draxul-rezonality-plugin PRIVATE
    Draxul::PluginSDK
    Draxul::PluginSupport::Adapter)
set_target_properties(draxul-rezonality-plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "draxul-rezonality")

if(COMMAND draxul_apply_sanitizers)
    draxul_apply_sanitizers(draxul-rezonality-plugin)
    draxul_apply_coverage(draxul-rezonality-plugin)
    draxul_apply_msvc_parallel_pdb_fix(draxul-rezonality-plugin)
endif()

file(GLOB _rezonality_test_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/rezonality_*_tests.cpp")

draxul_register_bundled_plugin(
    ID dev.draxul.rezonality
    TARGET draxul-rezonality-plugin
    MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/plugin.toml"
    FEATURE DRAXUL_ENABLE_REZONALITY
    DEPENDENCY_MODE STRICT
    PRODUCT_TARGETS draxul-rezonality-plugin
    TEST_SOURCES ${_rezonality_test_sources}
    TEST_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Tests.cmake")


