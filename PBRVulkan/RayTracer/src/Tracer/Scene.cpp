#include "Scene.h"

#include <iostream>
#include <future>
#include <utility>

#include "Camera.h"
#include "TextureImage.h"

#include "../Vulkan/Device.h"
#include "../Vulkan/CommandPool.h"
#include "../Vulkan/Buffer.h"

#include "../Geometry/Vertex.h"

#include "../Assets/Material.h"
#include "../Assets/Light.h"
#include "../Assets/Texture.h"
#include "../Assets/Mesh.h"

#include "../Loader/Loader.h"
#include "../path.h"

namespace Tracer
{
	Scene::Scene(
		std::string config,
		const Vulkan::Device& device,
		const Vulkan::CommandPool& commandPool)
		: config(std::move(config)), device(device), commandPool(commandPool)
	{
		const auto start = std::chrono::high_resolution_clock::now();

		root = Path::Root({ "PBRVulkan", "Assets", "PBRScenes" });

		Load();
		LoadEmptyBuffers();
		Wait();
		CreateBuffers();
		Print();

		const auto stop = std::chrono::high_resolution_clock::now();
		const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);

		std::cout << "[SCENE] Loading time: " << duration.count() << " milliseconds" << std::endl;
	}

	void Scene::Wait()
	{
		for (auto& texture : textures)
		{
			texture->Wait();
			textureImages.emplace_back(new TextureImage(device, commandPool, *texture));
		}

		for (const auto& mesh : meshes)
			mesh->Wait();

		if (hdrLoader.valid())
			hdrLoader.get();

		std::cout << "[SCENE] All assets have been loaded!" << std::endl;
	}

	void Scene::Print() const
	{
		std::cout << "[SCENE] Scene has been loaded!" << std::endl;
		std::cout << "	# meshes:    " << meshes.size() << std::endl;
		std::cout << "	# instances: " << meshInstances.size() << std::endl;
		std::cout << "	# textures:  " << textures.size() << std::endl;
		std::cout << "	# lights:    " << lights.size() << std::endl;
		std::cout << "	# materials: " << materials.size() << std::endl;
	}

	bool Scene::Load()
	{
		if (!LoadSceneFromFile(config, *this, options))
		{
			std::cout << "[ERROR] " + config + " does not exist" << std::endl;
			std::cout << "[INFO] Download the scenes repository! " << std::endl;
			exit(-1);
		}

		return true;
	}

	void Scene::LoadEmptyBuffers()
	{
		// Add a dummy texture for texture samplers in Vulkan
		if (textures.empty())
		{
			textures.emplace_back(std::make_unique<Assets::Texture>());
		}

		if (lights.empty())
		{
			Assets::Light light;
			light.type = -1;
			lights.emplace_back(light);
		}
	}

	void Scene::LoadHDR(HDRData* hdr)
	{
		VkFormat format = VK_FORMAT_R32G32B32_SFLOAT;
		VkImageTiling tiling = VK_IMAGE_TILING_LINEAR;
		VkImageType imageType = VK_IMAGE_TYPE_2D;

		auto columns = std::make_unique<Assets::Texture>(hdr->width, hdr->height, 12, hdr->cols);
		hdrImages.emplace_back(new TextureImage(device, commandPool, *columns, format, tiling, imageType));

		auto conditional = std::make_unique<Assets::Texture>(hdr->width, hdr->height, 12, hdr->conditionalDistData);
		hdrImages.emplace_back(new TextureImage(device, commandPool, *conditional, format, tiling, imageType));

		auto marginal = std::make_unique<Assets::Texture>(hdr->width, hdr->height, 12, hdr->marginalDistData);
		hdrImages.emplace_back(new TextureImage(device, commandPool, *marginal, format, tiling, imageType));
	}

	void Scene::CreateBuffers()
	{
		std::vector<uint32_t> indices;
		std::vector<Geometry::Vertex> vertices;
		std::vector<glm::uvec2> offsets;

		for (const auto& meshInstance : meshInstances)
		{
			auto& mesh = meshes[meshInstance.meshId];
			glm::mat4 modelMatrix = meshInstance.modelTransform;
			glm::mat4 modelTransInvMatrix = transpose(inverse(modelMatrix));

			for (auto& vertex : mesh->GetVertices())
			{
				vertex.position = modelMatrix * glm::vec4(vertex.position, 1.f);
				vertex.normal = normalize(modelTransInvMatrix * glm::vec4(vertex.normal, 1.f));
				vertex.materialId = meshInstance.materialId;
			}

			const auto indexOffset = static_cast<uint32_t>(indices.size());
			const auto vertexOffset = static_cast<uint32_t>(vertices.size());

			offsets.emplace_back(indexOffset, vertexOffset);

			vertices.insert(vertices.end(), mesh->GetVertices().begin(), mesh->GetVertices().end());
			indices.insert(indices.end(), mesh->GetIndecies().begin(), mesh->GetIndecies().end());
		}

		// =============== VERTEX BUFFER ===============

		auto usage = static_cast<VkBufferUsageFlagBits>(
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
		auto size = sizeof(meshes[0]->GetVertices()[0]) * vertices.size();
		std::cout << "[SCENE] Vertex buffer size = " << static_cast<double>(size) / 1000000.0 << " MB" << std::endl;
		Fill(vertexBuffer, vertices.data(), size, usage,
		     VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT);

		// =============== INDEX BUFFER ===============

		usage = static_cast<VkBufferUsageFlagBits>(
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
		size = sizeof(indices[0]) * indices.size();
		Fill(indexBuffer, indices.data(), size, usage,
		     VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT);

		// =============== MATERIAL BUFFER ===============

		size = sizeof(materials[0]) * materials.size();
		Fill(materialBuffer, materials.data(), size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0);

		// =============== OFFSET BUFFER ===============

		size = sizeof(offsets[0]) * offsets.size();
		Fill(offsetBuffer, offsets.data(), size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0);

		// =============== LIGHTS BUFFER ===============

		size = sizeof(lights[0]) * lights.size();
		Fill(lightsBuffer, lights.data(), size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0);
	}

	bool Scene::IsValid(const std::string config) const
	{
		const auto exists = std::filesystem::exists(config);

		if (!exists)
		{
			std::cout << "[ERROR] " + config + " does not exist" << std::endl;
			std::cout << "[INFO] Download https://github.com/Zielon/PBRScenes! " << std::endl;
		}

		return exists;
	}

	void Scene::Fill(
		std::unique_ptr<class Vulkan::Buffer>& buffer,
		void* data,
		size_t size,
		VkBufferUsageFlagBits usage,
		VkMemoryAllocateFlags allocateFlags) const
	{
		const std::unique_ptr<Vulkan::Buffer> buffer_staging(
			new Vulkan::Buffer(
				device, size,
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));

		buffer_staging->Fill(data);

		buffer.reset(
			new Vulkan::Buffer(
				device, size,
				static_cast<VkBufferUsageFlagBits>(VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage),
				allocateFlags,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));

		buffer->Copy(commandPool, *buffer_staging);
	}

	void Scene::AddCamera(glm::vec3 pos, glm::vec3 lookAt, float fov, float aspect)
	{
		camera.reset(new Camera(pos, lookAt, fov, aspect));
	}

	void Scene::AddHDR(const std::string& path)
	{
		hdrLoader = std::async(std::launch::async, [this, path]()
		{
			auto fs = std::filesystem::path(path).make_preferred();
			const auto file = (root / fs).string();
			auto* hdr = HDRLoader::load(file.c_str());

			if (hdr == nullptr)
				std::cerr << "[ERROR] Unable to load HDR!" << std::endl;
			else
			{
				std::cout << "[HDR TEXTURE] " + file + " has been loaded!" << std::endl;
				LoadHDR(hdr);
				hdrResolution = hdr->width * hdr->height;
				delete hdr;
			}
		});
	}

	int Scene::AddMeshInstance(Assets::MeshInstance meshInstance)
	{
		int id = meshInstances.size();
		meshInstances.push_back(meshInstance);
		return id;
	}

	int Scene::AddMesh(const std::string& path)
	{
		int id;

		if (meshMap.find(path) != meshMap.end())
		{
			id = meshMap[path];
		}
		else
		{
			auto fs = std::filesystem::path(path).make_preferred();
			const auto file = (root / fs).string();
			id = meshes.size();
			meshes.emplace_back(new Assets::Mesh(file));
			meshMap[path] = id;
			std::cout << "[MESH] " + file + " has been requested!" << std::endl;
		}

		return id;
	}

	int Scene::AddTexture(const std::string& path)
	{
		int id;

		if (textureMap.find(path) != textureMap.end())
		{
			id = meshMap[path];
		}
		else
		{
			auto fs = std::filesystem::path(path).make_preferred();
			const auto file = (root / fs).string();
			id = textures.size();
			textures.emplace_back(new Assets::Texture(file));
			textureMap[path] = id;
			std::cout << "[TEXTURE] " + file + " has been requested!" << std::endl;
		}

		return id;
	}

	int Scene::AddMaterial(Assets::Material material)
	{
		int id = materials.size();
		materials.push_back(material);
		return id;
	}

	int Scene::AddLight(Assets::Light light)
	{
		int id = lights.size();
		lights.push_back(light);
		return id;
	}

	Scene::~Scene()
	{
		std::cout << "[SCENE] Scene " << config << " has been unloaded." << std::endl;
	}
}
