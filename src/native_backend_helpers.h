#pragma once

#include "camera.h"
#include "model_loader.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace rezonality::detail
{

constexpr size_t kCommonUniformFloatCount = 192;
using CommonUniformBlock = std::array<float, kCommonUniformFloatCount>;

inline size_t align_up(size_t value, size_t alignment)
{
    return alignment == 0
        ? value
        : (value + alignment - 1) / alignment * alignment;
}

inline CommonUniformBlock make_common_uniforms(double elapsed_seconds,
    uint32_t width, uint32_t height, int32_t origin_x, int32_t origin_y,
    const Camera& camera)
{
    CommonUniformBlock uniform{};
    const float time = static_cast<float>(std::max(0.0, elapsed_seconds));
    uniform[0] = time;
    uniform[1] = time;
    uniform[2] = 1.0f / 60.0f;
    uniform[3] = time * 60.0f;
    uniform[4] = 60.0f;
    uniform[6] = 1.0f;
    const uint32_t vertex_size = sizeof(ModelVertex);
    std::memcpy(&uniform[7], &vertex_size, sizeof(vertex_size));
    uniform[8] = static_cast<float>(width);
    uniform[9] = static_cast<float>(height);
    uniform[10] = 1.0f;
    uniform[48] = static_cast<float>(origin_x);
    uniform[49] = static_cast<float>(origin_y);
    uniform[52] = camera.position.x;
    uniform[53] = camera.position.y;
    uniform[54] = camera.position.z;
    uniform[55] = 1.0f;
    const glm::mat4 model(1.0f);
    const glm::mat4 view = camera_view(camera);
    const glm::mat4 projection = camera_projection(camera, width, height);
    const glm::mat4 model_view_projection = projection * view * model;
    const glm::mat4 view_inverse = glm::inverse(view);
    const glm::mat4 projection_inverse = glm::inverse(projection);
    const auto copy_matrix = [&uniform](size_t offset,
                                 const glm::mat4& matrix) {
        std::memcpy(uniform.data() + offset, glm::value_ptr(matrix),
            sizeof(glm::mat4));
    };
    copy_matrix(56, model);
    copy_matrix(72, view);
    copy_matrix(88, projection);
    copy_matrix(104, model_view_projection);
    copy_matrix(120, view_inverse);
    copy_matrix(136, projection_inverse);
    return uniform;
}

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

} // namespace rezonality::detail
