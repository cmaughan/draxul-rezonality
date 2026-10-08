#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
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
    bool flip_texture_y = true;
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<ModelPart> parts;
    std::vector<ModelMaterial> materials;
};

// One immutable decoded model shared by every candidate build that uses it
// and by the project's decoded-asset cache, so reusing an import never copies
// its vertices or texture pixels. Converts to const ModelData& so renderers
// read it exactly like an owned model.
class SharedModel
{
public:
    SharedModel()
        : data_(std::make_shared<const ModelData>())
    {
    }
    explicit SharedModel(ModelData model)
        : data_(std::make_shared<const ModelData>(std::move(model)))
    {
    }
    explicit SharedModel(std::shared_ptr<const ModelData> data)
        : data_(data ? std::move(data) : std::make_shared<const ModelData>())
    {
    }

    [[nodiscard]] const ModelData& get() const
    {
        return *data_;
    }
    operator const ModelData&() const
    {
        return *data_;
    }
    const ModelData* operator->() const
    {
        return data_.get();
    }
    [[nodiscard]] const std::shared_ptr<const ModelData>& shared() const
    {
        return data_;
    }

private:
    std::shared_ptr<const ModelData> data_;
};

// When dependencies is non-null, every file the import opened or probed
// (including the model itself, external buffers, material libraries, and
// texture files) is appended so callers can detect changed inputs.
bool load_model(const std::filesystem::path& path, const glm::vec3& scale,
    bool flip_texture_y, ModelData& model, std::string& error,
    std::vector<std::filesystem::path>* dependencies = nullptr);

} // namespace rezonality
