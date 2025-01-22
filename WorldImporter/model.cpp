#include "model.h"
#include "fileutils.h"
#include <Windows.h>   
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>



//输出UV数据
void printUVInfo(const std::unordered_map<int, std::vector<int>>& uvToFaceId,
    const std::vector<std::vector<float>>& uvCoordinates) {
    // 输出 uvToFaceId 内容
    std::cout << "UV to Face ID Mapping:" << std::endl;
    for (const auto& uvFacePair : uvToFaceId) {
        std::cout << "Face ID: " << uvFacePair.first << " -> UV Indices: ";
        for (const int uvIndex : uvFacePair.second) {
            std::cout << uvIndex << " ";
        }
        std::cout << std::endl;
    }

    // 输出 uvCoordinates 内容
    std::cout << "\nUV Coordinates:" << std::endl;
    for (size_t i = 0; i < uvCoordinates.size(); ++i) {
        std::cout << "UV Index " << i << ": ("
            << uvCoordinates[i][0] << ", " << uvCoordinates[i][1] << ")" << std::endl;
    }
}

// 输出材质与面ID映射
void outputMaterialToFaceIds(const std::unordered_map<std::string, std::vector<int>>& materialToFaceIds) {
    std::cout << "Material to Face IDs:" << std::endl;
    for (const auto& entry : materialToFaceIds) {
        std::cout << "Material: " << entry.first << " -> Face IDs: [";
        for (size_t i = 0; i < entry.second.size(); ++i) {
            std::cout << entry.second[i];
            if (i < entry.second.size() - 1) {
                std::cout << ", ";
            }
        }
        std::cout << "]" << std::endl;
    }
}

// 获取可执行文件所在的目录路径
std::string getExecutableDir() {
    // 获取可执行文件路径
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);

    // 提取目录路径
    std::string exeDir(exePath);
    size_t lastSlash = exeDir.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        exeDir = exeDir.substr(0, lastSlash + 1);  // 包括最后的斜杠
    }
    return exeDir;
}

// 获取不带扩展名的文件名
std::string getObjectName(const std::string& objFilePath) {
    size_t dotPos = objFilePath.find_last_of(".");
    if (dotPos != std::string::npos) {
        return objFilePath.substr(0, dotPos);  // 去掉后缀
    }
    return objFilePath;  // 如果没有扩展名，直接返回路径
}

// 创建 .obj 文件并写入内容
void createObjFile(const std::string& objName, const std::string& objFilePath,
    const std::string& mtlFilePath, const std::vector<std::vector<float>>& vertices,
    const std::vector<std::vector<float>>& uvCoordinates,
    const std::unordered_map<int, std::vector<int>>& facesToVertices,
    const std::unordered_map<int, std::vector<int>>& uvToFaceId,
    const std::unordered_map<std::string, std::vector<int>>& materialToFaceIds) {
    std::ofstream objFile(objFilePath);
    if (objFile.is_open()) {
        // 写入 MTL 文件的引用
        objFile << "mtllib " << mtlFilePath << "\n";
        objFile << "o " + objName + "\n";

        // 写入顶点坐标
        for (const auto& vertex : vertices) {
            objFile << "v " << vertex[0] << " " << vertex[1] << " " << vertex[2] << "\n";
        }

        // 写入纹理坐标
        for (const auto& uv : uvCoordinates) {
            objFile << "vt " << uv[0] << " " << uv[1] << "\n";
        }

        // 写入面（需要将每个面与其材质对应）
        for (const auto& materialPair : materialToFaceIds) {
            const std::string& materialName = materialPair.first;
            const std::vector<int>& faceIds = materialPair.second;

            // 对于每个材质，使用 `usemtl` 来指定材质
            objFile << "usemtl " << materialName << "\n";

            // 写入对应材质的面数据
            for (int faceId : faceIds) {
                const auto& faceVertices = facesToVertices.at(faceId);
                const auto& faceUvIndices = uvToFaceId.at(faceId);

                objFile << "f";
                for (size_t i = 0; i < faceVertices.size(); ++i) {
                    objFile << " " << faceVertices[i] + 1 << "/" << faceUvIndices[i] + 1; // Obj格式从1开始计数
                }
                objFile << "\n";
            }
        }

        // 关闭文件
        objFile.close();
    }
    else {
        std::cerr << "Failed to create .obj file: " << objFilePath << std::endl;
    }
}


// 创建 .mtl 文件，接收 textureToPath 作为参数
void createMtlFile(const std::string& mtlFilePath, const std::unordered_map<std::string, std::string>& textureToPath) {
    std::ofstream mtlFile(mtlFilePath);
    if (mtlFile.is_open()) {
        // 遍历每个材质并写入相应的内容
        for (const auto& texturePair : textureToPath) {
            const std::string& textureName = texturePair.first;  // 材质名称
            std::string texturePath = texturePair.second;  // 材质对应的文件路径

            // 确保路径以 ".png" 结尾，如果没有则加上
            if (texturePath.substr(texturePath.find_last_of(".") + 1) != "png") {
                texturePath += ".png";
            }

            // 写入材质名称
            mtlFile << "newmtl " << textureName << std::endl;

            // 下面是固定的材质属性，您可以根据需要修改
            mtlFile << "Ns 90.000000" << std::endl; // 光泽度
            mtlFile << "Ka 1.000000 1.000000 1.000000" << std::endl; // 环境光颜色
            mtlFile << "Ks 0.000000 0.000000 0.000000" << std::endl; // 镜面反射
            mtlFile << "Ke 0.000000 0.000000 0.000000" << std::endl; // 自发光
            mtlFile << "Ni 1.500000" << std::endl; // 折射率
            mtlFile << "illum 1" << std::endl; // 照明模型

            // 写入纹理信息，注意这里是相对路径
            mtlFile << "map_Kd " << texturePath << std::endl; // 颜色纹理
            mtlFile << "map_d " << texturePath << std::endl;  // 透明度纹理

            mtlFile << std::endl; // 添加空行，以便分隔不同材质
        }

        // 关闭文件
        mtlFile.close();
    }
    else {
        std::cerr << "Failed to create .mtl file: " << mtlFilePath << std::endl;
    }
}


void processTextures(const nlohmann::json& modelJson,
    std::unordered_map<std::string, std::vector<int>>& materialToFaceIds,
    std::unordered_map<std::string, std::string>& textureToPath,
    std::unordered_map<std::string, std::string>& textureKeyToMaterialName) {

    std::unordered_set<std::string> processedMaterials;

    if (modelJson.contains("textures")) {
        auto textures = modelJson["textures"];
        for (auto& texture : textures.items()) {
            std::string textureKey = texture.key();
            std::string textureValue = texture.value();

            // 解析命名空间和路径
            size_t colonPos = textureValue.find(':');
            std::string namespaceName = "minecraft";
            std::string pathPart = textureValue;
            if (colonPos != std::string::npos) {
                namespaceName = textureValue.substr(0, colonPos);
                pathPart = textureValue.substr(colonPos + 1);
            }

            // 提取基础材质名称（取路径最后一段）
            size_t lastSlash = pathPart.find_last_of("/\\");
            std::string materialName = (lastSlash != std::string::npos) ?
                pathPart.substr(lastSlash + 1) :
                pathPart;

            // 生成唯一材质标识
            std::string fullMaterialName = namespaceName + ":" + materialName;
            textureKeyToMaterialName[textureKey] = fullMaterialName;

            if (processedMaterials.find(fullMaterialName) == processedMaterials.end()) {
                processedMaterials.insert(fullMaterialName);

                // 保存纹理到 textures/ 目录（直接扁平化存储）
                std::string saveDir = "textures";
                SaveTextureToFile(namespaceName, pathPart, saveDir); // 假设函数自动处理路径

                // 记录材质路径（直接使用基础名称）
                textureToPath[fullMaterialName] = "textures/" + materialName + ".png";

                // 初始化材质面列表
                materialToFaceIds[fullMaterialName] = std::vector<int>();
            }
        }
    }
}


void processElements(const nlohmann::json& modelJson,
    std::vector<std::vector<float>>& vertices,
    std::unordered_map<std::string, std::vector<int>>& materialToFaceIds,
    std::unordered_map<int, std::vector<int>>& facesToVertices,
    std::unordered_map<int, std::vector<int>>& uvToFaceId,
    std::vector<std::vector<float>>& uvCoordinates,
    const std::unordered_map<std::string, std::string>& textureKeyToMaterialName) {

    std::unordered_map<std::string, int> vertexCache;
    std::unordered_map<std::string, int> uvCache;
    int faceId = 0;

    if (modelJson.contains("elements")) {
        auto elements = modelJson["elements"];

        for (const auto& element : elements) {
            if (element.contains("from") && element.contains("to") && element.contains("faces")) {
                auto from = element["from"];
                auto to = element["to"];
                auto faces = element["faces"];

                // 转换原始坐标为 OBJ 坐标系（/16）
                float x1 = from[0].get<float>() / 16.0f;
                float y1 = from[1].get<float>() / 16.0f;
                float z1 = from[2].get<float>() / 16.0f;
                float x2 = to[0].get<float>() / 16.0f;
                float y2 = to[1].get<float>() / 16.0f;
                float z2 = to[2].get<float>() / 16.0f;

                // 生成基础顶点数据
                std::unordered_map<std::string, std::vector<std::vector<float>>> elementVertices = {
                    {"down",  {{x2, y1, z2}, {x2, y1, z1}, {x1, y1, z1}, {x1, y1, z2}}},
                    {"up",    {{x1, y2, z1}, {x1, y2, z2}, {x2, y2, z2}, {x2, y2, z1}}},
                    {"west",  {{x1, y1, z2}, {x1, y2, z2}, {x1, y2, z1}, {x1, y1, z1}}},
                    {"east",  {{x2, y1, z1}, {x2, y2, z1}, {x2, y2, z2}, {x2, y1, z2}}},
                    {"north", {{x1, y1, z1}, {x1, y2, z1}, {x2, y2, z1}, {x2, y1, z1}}},
                    {"south", {{x2, y1, z2}, {x2, y2, z2}, {x1, y2, z2}, {x1, y1, z2}}}
                };

                // 处理元素旋转
                if (element.contains("rotation")) {
                    auto rotation = element["rotation"];
                    std::string axis = rotation["axis"].get<std::string>();
                    float angle_deg = rotation["angle"].get<float>();
                    auto origin = rotation["origin"];

                    // 转换旋转中心到 OBJ 坐标系
                    float ox = origin[0].get<float>() / 16.0f;
                    float oy = origin[1].get<float>() / 16.0f;
                    float oz = origin[2].get<float>() / 16.0f;
                    float angle_rad = angle_deg * (M_PI / 180.0f); // 转换为弧度

                    // 对每个面的顶点应用旋转
                    for (auto& faceEntry : elementVertices) {
                        auto& vertices = faceEntry.second;
                        for (auto& vertex : vertices) {
                            float& vx = vertex[0];
                            float& vy = vertex[1];
                            float& vz = vertex[2];

                            // 平移至旋转中心相对坐标
                            float tx = vx - ox;
                            float ty = vy - oy;
                            float tz = vz - oz;

                            // 根据轴类型进行旋转
                            if (axis == "x") {
                                // 绕X轴旋转
                                float new_y = ty * cos(angle_rad) - tz * sin(angle_rad);
                                float new_z = ty * sin(angle_rad) + tz * cos(angle_rad);
                                ty = new_y;
                                tz = new_z;
                            }
                            else if (axis == "y") {
                                // 绕Y轴旋转
                                float new_x = tx * cos(angle_rad) + tz * sin(angle_rad);
                                float new_z = -tx * sin(angle_rad) + tz * cos(angle_rad);
                                tx = new_x;
                                tz = new_z;
                            }
                            else if (axis == "z") {
                                // 绕Z轴旋转
                                float new_x = tx * cos(angle_rad) - ty * sin(angle_rad);
                                float new_y = tx * sin(angle_rad) + ty * cos(angle_rad);
                                tx = new_x;
                                ty = new_y;
                            }

                            // 平移回原坐标系
                            vx = tx + ox;
                            vy = ty + oy;
                            vz = tz + oz;
                        }
                    }
                }

                // 遍历每个面的数据，判断面是否存在，如果存在则处理
                for (auto& face : faces.items()) {
                    std::string faceName = face.key();  // 面的名称 (down, up, east, etc.)
                    if (elementVertices.find(faceName) != elementVertices.end()) {
                        auto faceVertices = elementVertices[faceName];

                        // 对于每个面，添加四个顶点
                        std::vector<int> currentFaceVertices;
                        for (const auto& vertex : faceVertices) {
                            std::string vertexKey = std::to_string(vertex[0]) + "," + std::to_string(vertex[1]) + "," + std::to_string(vertex[2]);
                            // 如果这个顶点不存在，添加它并缓存
                            if (vertexCache.find(vertexKey) == vertexCache.end()) {
                                vertexCache[vertexKey] = vertices.size();
                                vertices.push_back(vertex);
                            }
                            // 将顶点的索引添加到当前面的顶点列表中
                            currentFaceVertices.push_back(vertexCache[vertexKey]);
                        }

                        // 将面ID和对应的顶点列表存储在 facesToVertices 中
                        facesToVertices[faceId] = currentFaceVertices;

                        // 在处理每个面的材质时
                        if (face.value().contains("texture")) {
                            std::string texture = face.value()["texture"];
                            if (texture.front() == '#') texture.erase(0, 1);

                            // 通过映射获取正确材质名称
                            auto it = textureKeyToMaterialName.find(texture);
                            if (it != textureKeyToMaterialName.end()) {
                                const std::string& materialName = it->second;
                                materialToFaceIds[materialName].push_back(faceId);
                            }


                            // 处理 UV 区域，如果没有指定，自动根据当前面的坐标设置
                            std::vector<int> uvRegion = { 0, 0, 16, 16 }; // 默认 UV 区域

                            if (face.value().contains("uv")) {
                                auto uv = face.value()["uv"];
                                uvRegion = { uv[0].get<int>(), uv[1].get<int>(), uv[2].get<int>(), uv[3].get<int>() };
                            }

                            // 获取旋转角度，默认为0
                            int rotation = face.value().value("rotation", 0);


                            // 计算四个 UV 坐标点
                            std::vector<std::vector<float>> uvCoords = {
                                {uvRegion[2] / 16.0f, 1 - uvRegion[3] / 16.0f},   
                                {uvRegion[2] / 16.0f, 1 - uvRegion[1] / 16.0f},
                                {uvRegion[0] / 16.0f, 1 - uvRegion[1] / 16.0f},
                                {uvRegion[0] / 16.0f, 1 - uvRegion[3] / 16.0f}
                                
                            };

                            // 根据旋转角度调整 UV 坐标
                            switch (rotation) {
                            case 90:
                                std::swap(uvCoords[0], uvCoords[3]);
                                std::swap(uvCoords[0], uvCoords[2]);
                                std::swap(uvCoords[0], uvCoords[1]);
                                break;
                            case 180:  
                                std::swap(uvCoords[0], uvCoords[2]);
                                std::swap(uvCoords[1], uvCoords[3]);
                                break;
                            case 270:
                                std::swap(uvCoords[0], uvCoords[3]);
                                std::swap(uvCoords[1], uvCoords[3]);
                                std::swap(uvCoords[2], uvCoords[3]);
                                break;
                            default:
                                break;
                            }

                            // 将 UV 坐标存入缓存，避免重复
                            std::vector<int> uvIndices;
                            for (const auto& uv : uvCoords) {
                                std::string uvKey = std::to_string(uv[0]) + "," + std::to_string(uv[1]);
                                if (uvCache.find(uvKey) == uvCache.end()) {
                                    uvCache[uvKey] = uvCoordinates.size();
                                    uvCoordinates.push_back(uv);
                                }
                                uvIndices.push_back(uvCache[uvKey]);
                            }
                            // 将每个面的 UV 坐标索引（四个顶点）与面 ID 关联
                            uvToFaceId[faceId] = uvIndices;
                        }

                        // 增加面ID
                        faceId++;
                    }
                }
            }
        }
    }
}




// 处理模型并生成文件路径
void processModelToObj(const nlohmann::json& modelJson, const std::string& objFilePath, const std::string& mtlFilePath) {
    // 获取可执行文件目录路径
    std::string exeDir = getExecutableDir();

    // 组合完整的obj文件路径
    std::string fullObjPath = exeDir + objFilePath;
    std::string fullMtlPath = exeDir + mtlFilePath;

    // 获取不带扩展名的文件名
    std::string objName = getObjectName(objFilePath);

    std::vector<std::vector<float>> vertices;  // 存储顶点信息
    std::vector<std::vector<float>> uvCoordinates;// 新增 uv 坐标列表
    std::unordered_map<std::string, std::vector<int>> materialToFaceIds;  // 存储材质与面ID映射
    std::unordered_map<int, std::vector<int>> facesToVertices;  // 存储面与其顶点索引的映射
    std::unordered_map<std::string, std::string> textureToPath;  // 存储材质名称与文件路径的对照表
    std::unordered_map<int, std::vector<int>> uvToFaceId; // 新增 uv 对照表
    std::unordered_map<std::string, std::string> textureKeyToMaterialName; // 新增映射表

    // 处理纹理（调用纹理处理函数）
    processTextures(modelJson, materialToFaceIds, textureToPath, textureKeyToMaterialName);
    processElements(modelJson, vertices, materialToFaceIds, facesToVertices, uvToFaceId, uvCoordinates, textureKeyToMaterialName);

    // 创建 .obj 和 .mtl 文件
    createObjFile(objName, fullObjPath, mtlFilePath, vertices, uvCoordinates, facesToVertices, uvToFaceId, materialToFaceIds);
    createMtlFile(fullMtlPath,textureToPath);
}



// 递归加载父模型，直到没有父模型为止
nlohmann::json LoadParentModel(const std::string& namespaceName, const std::string& blockId, nlohmann::json& currentModelJson) {
    // 获取当前模型的 parent
    if (currentModelJson.contains("parent")) {
        std::string parentModelId = currentModelJson["parent"];
        std::cout << "Loading parent model: " << parentModelId << std::endl;

        // 判断 parentModelId 是否包含冒号（即是否包含命名空间）
        size_t colonPos = parentModelId.find(':');
        std::string parentNamespace = namespaceName;  // 默认使用当前的 namespaceName

        if (colonPos != std::string::npos) {
            parentNamespace = parentModelId.substr(0, colonPos);  // 提取冒号前的部分作为父模型的命名空间
            parentModelId = parentModelId.substr(colonPos + 1);  // 提取冒号后的部分作为父模型的 ID
        }

        // 获取父模型的 JSON
        nlohmann::json parentModelJson = GetModelJson(parentNamespace, parentModelId);

        // 如果父模型存在，递归加载父模型并合并属性
        if (!parentModelJson.is_null()) {
            // 递归合并父模型的属性到当前模型中
            currentModelJson = MergeModelJson(parentModelJson, currentModelJson);

            // 如果父模型没有 parent 属性，停止递归
            if (!parentModelJson.contains("parent")) {
                return currentModelJson;  // 直接返回当前合并后的模型
            }

            // 递归加载父模型的父模型
            currentModelJson = LoadParentModel(parentNamespace, parentModelId, currentModelJson);
        }
    }
    return currentModelJson;
}

// 合并父模型和当前模型的属性
nlohmann::json MergeModelJson(const nlohmann::json& parentModelJson, const nlohmann::json& currentModelJson) {
    nlohmann::json mergedModelJson = currentModelJson;
    std::map<std::string, std::string> textureMap;

    // 保存子级的 textures
    if (currentModelJson.contains("textures")) {
        for (const auto& item : currentModelJson["textures"].items()) {
            textureMap[item.key()] = item.value();
        }
    }

    // 父模型的 parent 属性覆盖子模型的 parent 属性
    if (parentModelJson.contains("parent")) {
        mergedModelJson["parent"] = parentModelJson["parent"];
    }

    // 合并 "textures"
    if (parentModelJson.contains("textures")) {
        if (!mergedModelJson.contains("textures")) {
            mergedModelJson["textures"] = nlohmann::json::object();
        }
        for (const auto& item : parentModelJson["textures"].items()) {
            std::string textureValue = item.value();
            if (!textureValue.empty() && textureValue[0] == '#') {
                std::string varName = textureValue.substr(1);
                if (textureMap.find(varName) != textureMap.end()) {
                    textureValue = textureMap[varName];
                }
            }
            mergedModelJson["textures"][item.key()] = textureValue;
        }
    }

    // 合并 "elements"
    if (parentModelJson.contains("elements") && !currentModelJson.contains("elements")) {
        mergedModelJson["elements"] = parentModelJson["elements"];
    }

    // 合并 "display"
    if (parentModelJson.contains("display") && !currentModelJson.contains("display")) {
        mergedModelJson["display"] = parentModelJson["display"];
    }

    // 合并其他需要继承的属性
    if (parentModelJson.contains("ambientocclusion") && !currentModelJson.contains("ambientocclusion")) {
        mergedModelJson["ambientocclusion"] = parentModelJson["ambientocclusion"];
    }

    return mergedModelJson;
}

// 获取目标路径对应的模型文件的内容
nlohmann::json GetModelJson(const std::string& namespaceName, const std::string& blockId) {
    // 获取当前整合包的版本
    std::string currentVersion = currentSelectedGameVersion;

    // 在 modListCache 中查找对应的模组列表
    if (modListCache.find(currentVersion) == modListCache.end()) {
        std::cerr << "Mod list for version " << currentVersion << " not found!" << std::endl;
        return nlohmann::json();  // 返回空的 JSON
    }

    // 获取该版本的游戏文件夹路径
    std::string gameFolderPath = config.versionConfigs[currentVersion].gameFolderPath;


    // 遍历 modListCache 中该版本的模组列表，查找对应的 .json 文件
    for (const auto& folderData : modListCache[currentVersion]) {
        // 获取当前模组的命名空间
        std::string modNamespace = folderData.namespaceName;

        // 如果是 Vanilla (minecraft)
        if (modNamespace == "vanilla") {
            // 从 VersionCache 中获取 minecraft 的路径
            if (VersionCache.find(currentVersion) != VersionCache.end()) {
                for (const auto& folderData : VersionCache[currentVersion]) {
                    // 获取 minecraft 文件夹路径
                    std::wstring minecraftPath = string_to_wstring(folderData.path);
                    JarReader minecraftReader(minecraftPath);

                    // 构造 model 文件的路径
                    std::string modelFilePath = "assets/" + namespaceName + "/models/" + blockId + ".json";
                    std::string fileContent = minecraftReader.getFileContent(modelFilePath);

                    // 如果找到了文件，解析 JSON 内容并返回
                    if (!fileContent.empty()) {
                        try {
                            nlohmann::json modelJson = nlohmann::json::parse(fileContent);
                            return modelJson;
                        }
                        catch (const std::exception& e) {
                            std::cerr << "Error parsing JSON: " << e.what() << std::endl;
                            return nlohmann::json();  // 返回空的 JSON
                        }
                    }
                }
            }
            else {
                std::cerr << "Minecraft version not found in VersionCache!" << std::endl;
                return nlohmann::json();  // 返回空的 JSON
            }
        }
        // 如果是 ResourcePack
        else if (modNamespace == "resourcePack") {
            // 遍历 resourcePacksCache 查找对应的 resourcePack 文件
            for (const auto& folderData : resourcePacksCache[currentVersion]) {
                std::wstring resourcePackPath = string_to_wstring(folderData.path);
                JarReader resourcePackReader(resourcePackPath);

                // 构造 model 文件的路径
                std::string modelFilePath = "assets/" + namespaceName + "/models/" + blockId + ".json";
                std::string fileContent = resourcePackReader.getFileContent(modelFilePath);

                // 如果找到了文件，解析 JSON 内容并返回
                if (!fileContent.empty()) {
                    try {
                        nlohmann::json modelJson = nlohmann::json::parse(fileContent);
                        return modelJson;
                    }
                    catch (const std::exception& e) {
                        std::cerr << "Error parsing JSON: " << e.what() << std::endl;
                        return nlohmann::json();  // 返回空的 JSON
                    }
                }
            }
        }
        // 如果是普通 Mod 模组，继续按原逻辑处理
        else {
            // 构造 model 文件的路径
            std::string modelFilePath = "assets/" + namespaceName + "/models/" + blockId + ".json";
            std::wstring jarFilePath = string_to_wstring(folderData.path);
            JarReader jarReader(jarFilePath);

            // 如果找到了文件，解析 JSON 内容并返回
            std::string fileContent = jarReader.getFileContent(modelFilePath);
            if (!fileContent.empty()) {
                try {
                    nlohmann::json modelJson = nlohmann::json::parse(fileContent);
                    return modelJson;
                }
                catch (const std::exception& e) {
                    std::cerr << "Error parsing JSON: " << e.what() << std::endl;
                    return nlohmann::json();  // 返回空的 JSON
                }
            }
        }
    }

    // 如果没有找到文件，返回空 JSON
    std::cerr << "Model file not found for blockId: " << blockId << std::endl;
    return nlohmann::json();  // 返回空的 JSON
}


// 处理模型 JSON 的方法
nlohmann::json ProcessModelJson(const std::string& namespaceName, const std::string& blockId) {
    nlohmann::json modelJson = GetModelJson(namespaceName, blockId);

    if (modelJson.is_null()) {
        return nlohmann::json();
    }

    // 递归加载父模型并合并属性
    modelJson = LoadParentModel(namespaceName, blockId, modelJson);

    // 处理 blockId，去掉路径部分，保留最后的文件名
    size_t lastSlashPos = blockId.find_last_of("/\\");
    std::string fileName = (lastSlashPos == std::string::npos) ? blockId : blockId.substr(lastSlashPos + 1);

    // 构建 OBJ 文件路径
    std::string objFilePath = fileName + ".obj";

    // 构建 OBJ 文件路径
    std::string mtlFilePath = fileName + ".mtl";

    // 处理模型数据并保存为 OBJ 文件
    processModelToObj(modelJson, objFilePath, mtlFilePath);

    return modelJson;
}