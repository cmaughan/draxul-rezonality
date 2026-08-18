set(_rezonality_root "${CMAKE_CURRENT_LIST_DIR}/..")
file(GLOB _rezonality_test_sources CONFIGURE_DEPENDS
    "${_rezonality_root}/tests/rezonality_*_tests.cpp")

draxul_add_test_target(
    draxul-test-rezonality rezonality 1
    ${_rezonality_test_sources}
    "${_rezonality_root}/src/rezonality_plugin.cpp")
target_link_libraries(draxul-test-rezonality PRIVATE
    Draxul::PluginSDK
    Draxul::PluginSupport::Adapter)

