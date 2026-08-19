#include "model_loader.h"

#include "image_loader.h"

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace rezonality
{

namespace
{

ModelTexture solid_texture(std::array<uint8_t, 4> rgba, bool srgb = false)
{
    ModelTexture texture;
    texture.srgb = srgb;
    texture.pixels.assign(rgba.begin(), rgba.end());
    return texture;
}

void flip_texture_rows(ModelTexture& texture)
{
    if (texture.height <= 1 || texture.width == 0 || texture.pixels.empty())
        return;
    const size_t row_bytes = static_cast<size_t>(texture.width) * 4;
    std::vector<uint8_t> row(row_bytes);
    for (uint32_t y = 0; y < texture.height / 2; ++y)
    {
        uint8_t* top = texture.pixels.data() + y * row_bytes;
        uint8_t* bottom = texture.pixels.data()
            + (texture.height - 1 - y) * row_bytes;
        std::memcpy(row.data(), top, row_bytes);
        std::memcpy(top, bottom, row_bytes);
        std::memcpy(bottom, row.data(), row_bytes);
    }
}

bool load_texture(const aiScene& scene, const aiMaterial& material,
    aiTextureType type, const std::filesystem::path& model_path,
    std::array<uint8_t, 4> fallback, bool srgb, bool flip_y,
    ModelTexture& result, std::string& error)
{
    aiString requested;
    if (material.GetTextureCount(type) == 0
        || material.GetTexture(type, 0, &requested) != aiReturn_SUCCESS)
    {
        result = solid_texture(fallback, srgb);
        return true;
    }

    result.srgb = srgb;
    if (const aiTexture* embedded = scene.GetEmbeddedTexture(requested.C_Str()))
    {
        if (embedded->mHeight == 0)
        {
            if (load_rgba8_image(
                    reinterpret_cast<const uint8_t*>(embedded->pcData),
                    embedded->mWidth, result.width, result.height,
                    result.pixels, error))
            {
                if (flip_y)
                    flip_texture_rows(result);
                return true;
            }
        }
        else
        {
            result.width = embedded->mWidth;
            result.height = embedded->mHeight;
            result.pixels.resize(static_cast<size_t>(result.width)
                * result.height * 4);
            for (size_t index = 0;
                 index < static_cast<size_t>(result.width) * result.height;
                 ++index)
            {
                const aiTexel& source = embedded->pcData[index];
                result.pixels[index * 4 + 0] = source.r;
                result.pixels[index * 4 + 1] = source.g;
                result.pixels[index * 4 + 2] = source.b;
                result.pixels[index * 4 + 3] = source.a;
            }
            if (flip_y)
                flip_texture_rows(result);
            return true;
        }
    }
    else
    {
        std::filesystem::path texture_path
            = std::filesystem::u8path(requested.C_Str());
        if (texture_path.is_relative())
            texture_path = model_path.parent_path() / texture_path;
        if (load_rgba8_image(texture_path, result.width, result.height,
                result.pixels, error))
        {
            if (flip_y)
                flip_texture_rows(result);
            return true;
        }
        error = "Could not load model texture '" + texture_path.string()
            + "': " + error;
        return false;
    }
    error = "Could not decode embedded model texture '"
        + std::string(requested.C_Str()) + "' in '" + model_path.string()
        + "': " + error;
    return false;
}

glm::vec3 converted(const aiVector3D& value)
{
    return { value.x, value.y, value.z };
}

} // namespace

bool load_model(const std::filesystem::path& path, const glm::vec3& scale,
    bool flip_texture_y, ModelData& model, std::string& error)
{
    Assimp::Importer importer;
    constexpr unsigned flags = aiProcess_Triangulate
        | aiProcess_PreTransformVertices
        | aiProcess_CalcTangentSpace | aiProcess_GenSmoothNormals;
    const aiScene* scene = importer.ReadFile(path.string(), flags);
    if (!scene || !scene->HasMeshes())
    {
        error = "Could not load model '" + path.string() + "': "
            + importer.GetErrorString();
        return false;
    }
    if (scene->mNumMaterials > kMaxModelMaterials)
    {
        error = "Model '" + path.string() + "' has "
            + std::to_string(scene->mNumMaterials) + " materials; maximum is "
            + std::to_string(kMaxModelMaterials);
        return false;
    }

    ModelData candidate;
    candidate.path = path;
    candidate.scale = scale;
    candidate.flip_texture_y = flip_texture_y;
    candidate.materials.reserve(std::max(1u, scene->mNumMaterials));
    for (unsigned index = 0; index < scene->mNumMaterials; ++index)
    {
        const aiMaterial& source = *scene->mMaterials[index];
        ModelMaterial material;
        material.name = source.GetName().C_Str();
        aiColor4D base(1.0f, 1.0f, 1.0f, 1.0f);
        if (aiGetMaterialColor(&source, AI_MATKEY_BASE_COLOR, &base)
            != aiReturn_SUCCESS)
        {
            aiColor3D diffuse(1.0f, 1.0f, 1.0f);
            source.Get(AI_MATKEY_COLOR_DIFFUSE, diffuse);
            base = { diffuse.r, diffuse.g, diffuse.b, 1.0f };
        }
        material.base_color_factor = { base.r, base.g, base.b, base.a };
        aiColor3D emissive(0.0f, 0.0f, 0.0f);
        source.Get(AI_MATKEY_COLOR_EMISSIVE, emissive);
        material.emissive_factor
            = { emissive.r, emissive.g, emissive.b, 1.0f };
        source.Get(AI_MATKEY_METALLIC_FACTOR, material.metallic_factor);
        source.Get(AI_MATKEY_ROUGHNESS_FACTOR, material.roughness_factor);
        if (!load_texture(*scene, source,
            source.GetTextureCount(aiTextureType_BASE_COLOR)
                ? aiTextureType_BASE_COLOR : aiTextureType_DIFFUSE,
            path, { 255, 255, 255, 255 }, true, flip_texture_y,
            material.base_color, error)
            || !load_texture(*scene, source,
            source.GetTextureCount(aiTextureType_NORMALS)
                ? aiTextureType_NORMALS : aiTextureType_NORMAL_CAMERA,
            path, { 128, 128, 255, 255 }, false, flip_texture_y,
            material.normal, error)
            || !load_texture(*scene, source,
            source.GetTextureCount(aiTextureType_METALNESS)
                ? aiTextureType_METALNESS : aiTextureType_DIFFUSE_ROUGHNESS,
            path, { 255, 255, 255, 255 }, false, flip_texture_y,
            material.metallic_roughness, error)
            || !load_texture(*scene, source,
            source.GetTextureCount(aiTextureType_EMISSIVE)
                ? aiTextureType_EMISSIVE : aiTextureType_EMISSION_COLOR,
            path, { 255, 255, 255, 255 }, true, flip_texture_y,
            material.emissive, error)
            || !load_texture(*scene, source,
            aiTextureType_AMBIENT_OCCLUSION, path,
            { 255, 255, 255, 255 }, false, flip_texture_y,
            material.occlusion, error))
            return false;
        candidate.materials.push_back(std::move(material));
    }
    if (candidate.materials.empty())
        candidate.materials.emplace_back();

    for (unsigned mesh_index = 0; mesh_index < scene->mNumMeshes;
         ++mesh_index)
    {
        const aiMesh& mesh = *scene->mMeshes[mesh_index];
        const uint32_t vertex_base
            = static_cast<uint32_t>(candidate.vertices.size());
        for (unsigned vertex_index = 0; vertex_index < mesh.mNumVertices;
             ++vertex_index)
        {
            ModelVertex vertex;
            vertex.position = glm::vec4(
                converted(mesh.mVertices[vertex_index]) * scale, 1.0f);
            if (mesh.HasTextureCoords(0))
                vertex.uv = { mesh.mTextureCoords[0][vertex_index].x,
                    mesh.mTextureCoords[0][vertex_index].y };
            if (mesh.HasNormals())
                vertex.normal = converted(mesh.mNormals[vertex_index]);
            if (mesh.HasTangentsAndBitangents())
            {
                vertex.tangent = converted(mesh.mTangents[vertex_index]);
                vertex.bitangent
                    = converted(mesh.mBitangents[vertex_index]);
            }
            if (mesh.HasVertexColors(0))
            {
                const aiColor4D& color = mesh.mColors[0][vertex_index];
                vertex.color = { color.r, color.g, color.b };
            }
            else if (mesh.mMaterialIndex < candidate.materials.size())
                vertex.color = glm::vec3(candidate.materials[
                    mesh.mMaterialIndex].base_color_factor);
            candidate.vertices.push_back(vertex);
        }
        ModelPart part;
        part.name = mesh.mName.C_Str();
        part.index_offset = static_cast<uint32_t>(candidate.indices.size());
        part.material_index = std::min<uint32_t>(mesh.mMaterialIndex,
            static_cast<uint32_t>(candidate.materials.size() - 1));
        for (unsigned face_index = 0; face_index < mesh.mNumFaces;
             ++face_index)
        {
            const aiFace& face = mesh.mFaces[face_index];
            if (face.mNumIndices != 3)
                continue;
            for (unsigned corner = 0; corner < 3; ++corner)
                candidate.indices.push_back(vertex_base
                    + face.mIndices[corner]);
            part.index_count += 3;
        }
        candidate.parts.push_back(std::move(part));
    }
    if (candidate.vertices.empty() || candidate.indices.empty())
    {
        error = "Model '" + path.string() + "' contains no triangles";
        return false;
    }
    model = std::move(candidate);
    return true;
}

} // namespace rezonality
