#include "camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace rezonality
{

namespace
{

glm::quat quat_from_vectors(glm::vec3 from, glm::vec3 to)
{
    const float norms = std::sqrt(glm::dot(from, from) * glm::dot(to, to));
    float real = norms + glm::dot(from, to);
    glm::vec3 axis;
    if (real < 1.e-6f * norms)
    {
        real = 0.0f;
        axis = std::abs(from.x) > std::abs(from.z)
            ? glm::vec3(-from.y, from.x, 0.0f)
            : glm::vec3(0.0f, -from.z, from.y);
    }
    else
        axis = glm::cross(from, to);
    return glm::normalize(glm::quat(real, axis.x, axis.y, axis.z));
}

void update_axes(Camera& camera)
{
    camera.right = glm::normalize(
        glm::vec3(1.0f, 0.0f, 0.0f) * camera.orientation);
    camera.up = glm::normalize(
        glm::vec3(0.0f, 1.0f, 0.0f) * camera.orientation);
}

} // namespace

void camera_set_pos_lookat(Camera& camera, const glm::vec3& position,
    const glm::vec3& focal_point)
{
    camera.position = position;
    camera.focal_point = focal_point;
    camera.view_direction = glm::normalize(focal_point - position);
    camera.orientation = quat_from_vectors(
        camera.view_direction, glm::vec3(0.0f, 0.0f, -1.0f));
    update_axes(camera);
}

void camera_orbit(Camera& camera, const glm::vec2& degrees)
{
    const glm::quat vertical = glm::angleAxis(
        glm::radians(degrees.y), camera.right);
    const glm::quat horizontal = glm::angleAxis(
        glm::radians(degrees.x), glm::vec3(0.0f, 1.0f, 0.0f));
    camera.orientation = glm::normalize(
        camera.orientation * vertical * horizontal);
    const float distance = glm::length(
        camera.focal_point - camera.position);
    camera.view_direction = glm::normalize(
        glm::vec3(0.0f, 0.0f, -1.0f) * camera.orientation);
    camera.position = camera.focal_point
        - camera.view_direction * distance;
    update_axes(camera);
}

void camera_dolly(Camera& camera, float distance)
{
    const float current = glm::length(
        camera.focal_point - camera.position);
    const float requested = std::max(0.05f, current - distance);
    camera.position = camera.focal_point
        - camera.view_direction * requested;
}

glm::mat4 camera_view(const Camera& camera)
{
    return glm::lookAt(camera.position, camera.focal_point, camera.up);
}

glm::mat4 camera_projection(const Camera& camera, uint32_t width,
    uint32_t height)
{
    return glm::perspectiveFov(glm::radians(camera.field_of_view),
        static_cast<float>(std::max(1u, width)),
        static_cast<float>(std::max(1u, height)),
        camera.near_far.x, camera.near_far.y);
}

} // namespace rezonality
