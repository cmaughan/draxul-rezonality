cmake_minimum_required(VERSION 3.25)

# Rezonality currently builds as a mounted Draxul product. Keeping this root
# deliberately small makes the ownership seam explicit while the VkLive core
# is imported in functional slices.
if(NOT TARGET Draxul::PluginSDK)
    message(FATAL_ERROR
        "Rezonality must currently be configured from a Draxul checkout")
endif()

if(NOT TARGET nlohmann_json::nlohmann_json)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

if(NOT TARGET assimp::assimp)
    include(FetchContent)
    set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_NO_EXPORT ON CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        rezonality_assimp
        GIT_REPOSITORY https://github.com/assimp/assimp.git
        GIT_TAG v5.4.3
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(rezonality_assimp)
    # Assimp sets a global debug postfix in its cache. Draxul deliberately
    # keeps one binary name across configurations because manifests name the
    # module explicitly.
    set(CMAKE_DEBUG_POSTFIX "" CACHE STRING "" FORCE)
endif()

if(APPLE)
    include(FetchContent)
    set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    # SPIRV-Cross's MSL backend shares compiler support with its GLSL target,
    # so GLSL must be enabled even though Rezonality only emits MSL on macOS.
    # This does not add a Vulkan SDK or runtime dependency.
    set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_MSL ON CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
    set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        rezonality_spirv_cross
        GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
        GIT_TAG 1a6169566c73d3da552748fc372fe2bbb856e46e
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(rezonality_spirv_cross)
elseif(NOT TARGET Vulkan::Vulkan)
    find_package(Vulkan REQUIRED)
endif()

set(_rezonality_sources
    src/camera.cpp
    src/camera.h
    src/image_loader.cpp
    src/image_loader.h
    src/live_project.cpp
    src/live_project.h
    src/model_loader.cpp
    src/model_loader.h
    src/rezonality_plugin.cpp)

add_library(draxul-rezonality-plugin MODULE ${_rezonality_sources})
target_link_libraries(draxul-rezonality-plugin PRIVATE
    Draxul::PluginSDK
    Draxul::PluginSupport::Adapter
    assimp::assimp
    glm::glm
    nlohmann_json::nlohmann_json)
target_compile_definitions(draxul-rezonality-plugin PRIVATE
    GLM_FORCE_DEPTH_ZERO_TO_ONE)
target_include_directories(draxul-rezonality-plugin PRIVATE ${stb_SOURCE_DIR})
if(APPLE)
    set_source_files_properties(src/rezonality_plugin.cpp
        PROPERTIES LANGUAGE OBJCXX)
    target_link_libraries(draxul-rezonality-plugin PRIVATE
        spirv-cross-msl
        "-framework Metal"
        "-framework Foundation")
else()
    target_link_libraries(draxul-rezonality-plugin PRIVATE
        Vulkan::Vulkan
        Draxul::PluginSupport::VulkanResources
        GPUOpen::VulkanMemoryAllocator)
endif()
set_target_properties(draxul-rezonality-plugin PROPERTIES
    PREFIX ""
    DEBUG_POSTFIX ""
    OUTPUT_NAME "draxul-rezonality")

if(COMMAND draxul_apply_sanitizers)
    draxul_apply_sanitizers(draxul-rezonality-plugin)
    draxul_apply_coverage(draxul-rezonality-plugin)
    draxul_apply_msvc_parallel_pdb_fix(draxul-rezonality-plugin)
endif()

file(GLOB _rezonality_test_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/rezonality_*_tests.cpp")

if(APPLE)
    set(_rezonality_compiler_payload
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/mac/glslangValidator=>tools/mac/glslangValidator")
else()
    set(_rezonality_compiler_payload
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/win/glslangValidator.exe=>tools/win/glslangValidator.exe")
endif()

draxul_register_bundled_plugin(
    ID dev.draxul.rezonality
    TARGET draxul-rezonality-plugin
    MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/plugin.toml"
    FEATURE DRAXUL_ENABLE_REZONALITY
    DEPENDENCY_MODE STRICT
    COPY_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/examples=>examples"
    COPY_FILES
        ${_rezonality_compiler_payload}
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glslang-NOTICE.txt=>third_party/glslang-NOTICE.txt"
    PRODUCT_TARGETS draxul-rezonality-plugin
    TEST_SOURCES ${_rezonality_test_sources}
    TEST_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Tests.cmake")


