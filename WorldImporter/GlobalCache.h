#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <mutex>
#include <shared_mutex>
#include "include/json.hpp"
#include "config.h"
#include "JarReader.h"

// 前向声明依赖类型
class JarReader;

// 外部声明
extern std::unordered_set<std::string> solidBlocks;
extern std::unordered_set<std::string> fluidBlocks;

// ========= 全局缓存声明 =========
namespace GlobalCache {
	// 纹理缓存 [modId:namespace:resource_path -> PNG数据]
	extern std::unordered_map<std::string, std::vector<unsigned char>> textures;

	//动态材质缓存 [modId:namespace:resource_path -> JSON]
	extern std::unordered_map<std::string, nlohmann::json> mcmetaCache;

	// 方块状态缓存 [modId:namespace:block_id -> JSON]
	extern std::unordered_map<std::string, nlohmann::json> blockstates;

	// 模型缓存 [modId:namespace:model_path -> JSON]
	extern std::unordered_map<std::string, nlohmann::json> models;

	// 生物群系缓存 [modId:namespace:biome_id -> JSON]
	extern std::unordered_map<std::string, nlohmann::json> biomes;

	// 色图缓存 [modId:namespace:colormap_name -> PNG数据]
	extern std::unordered_map<std::string, std::vector<unsigned char>> colormaps;

	// ========= 快速查找索引 =========
	// 直接查找键: "blockstates:<namespace>:<resourcePath>" -> 对应完整缓存键
	extern std::unordered_map<std::string, std::string> blockstateIndex;
	extern std::unordered_map<std::string, std::string> modelIndex;
	extern std::unordered_map<std::string, std::string> textureIndex;
	extern std::unordered_map<std::string, std::string> mcmetaIndex;
	extern std::unordered_map<std::string, std::string> biomeIndex;
	extern std::unordered_map<std::string, std::string> colormapIndex;

	// 同步原语
	extern std::once_flag initFlag;
	extern std::shared_mutex cacheMutex;
	extern std::vector<std::string> jarOrder;
}



// ========= 初始化方法 =========
void InitializeAllCaches();

