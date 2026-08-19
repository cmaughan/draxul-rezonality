#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>

namespace rezonality
{

// Retains VkLive's per-camera values, but leaves timing and input ownership to
// the pane instance instead of the former application window.
struct Camera
{
    std::string name = "default_camera";
    glm::vec3 position{ 0.0f, 0.0f, 4.0f };
    glm::vec3 focal_point{ 0.0f };
    glm::vec2 near_far{ 0.1f, 256.0f };
    float field_of_view = 60.0f;
    glm::quat orientation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 view_direction{ 0.0f, 0.0f, -1.0f };
    glm::vec3 right{ 1.0f, 0.0f, 0.0f };
    glm::vec3 up{ 0.0f, 1.0f, 0.0f };
};

void camera_set_pos_lookat(Camera& camera, const glm::vec3& position,
    const glm::vec3& focal_point);
void camera_orbit(Camera& camera, const glm::vec2& degrees);
void camera_dolly(Camera& camera, float distance);
glm::mat4 camera_view(const Camera& camera);
glm::mat4 camera_projection(const Camera& camera, uint32_t width,
    uint32_t height);

} // namespace rezonality
