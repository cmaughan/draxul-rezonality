#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rezonality
{

constexpr uint32_t kMaxModelMaterials = 16;

struct ModelVertex
{
    glm::vec4 position{ 0.0f, 0.0f, 0.0f, 1.0f };
    glm::vec2 uv{ 0.0f };
    glm::vec3 color{ 1.0f };
    glm::vec3 normal{ 0.0f, 0.0f, 1.0f };
    glm::vec3 tangent{ 1.0f, 0.0f, 0.0f };
    glm::vec3 bitangent{ 0.0f, 1.0f, 0.0f };
};

struct ModelTexture
{
    uint32_t width = 1;
    uint32_t height = 1;
    bool srgb = false;
    std::vector<uint8_t> pixels;
};

struct ModelMaterial
{
    std::string name;
    glm::vec4 base_color_factor{ 1.0f };
    glm::vec4 emissive_factor{ 0.0f };
    float metallic_factor = 1.0f;
    float roughness_factor = 1.0f;
    float occlusion_strength = 1.0f;
    ModelTexture base_color;
    ModelTexture normal;
    ModelTexture metallic_roughness;
    ModelTexture emissive;
    ModelTexture occlusion;
};

struct ModelPart
{
    std::string name;
    uint32_t index_offset = 0;
    uint32_t index_count = 0;
    uint32_t material_index = 0;
};

struct ModelData
{
    std::filesystem::path path;
    glm::vec3 scale{ 1.0f };
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<ModelPart> parts;
    std::vector<ModelMaterial> materials;
};

bool load_model(const std::filesystem::path& path, const glm::vec3& scale,
    ModelData& model, std::string& error);

} // namespace rezonality
