#ifndef CTM_H
#define CTM_H

// ==================== OptiFine CTM 连接材质支持 ====================
//
// 本模块实现 OptiFine 风格的连接纹理(Connected Texture Maps)。
//
// 资源来源:
//   assets/<namespace>/optifine/ctm/**/*.properties   CTM 规则
//   assets/<namespace>/optifine/ctm/**/*.png          CTM tile 贴图
//
// 支持的 method(第一阶段):
//   ctm_compact  5 张 tile,按象限合成连接贴图(玻璃/矿物块等主体)
//   horizontal   按左右连接选 tile(书架等)
//   vertical     按上下连接选 tile
//   ctm          完整 47 tile 标准连接(制图台等)
//   repeat       按世界坐标 + 面方向循环平铺 tile(网格纹理/条木等)
//
// 烘焙时机:
//   在区块导出阶段,逐方块逐面根据世界邻居关系选择/合成 CTM 贴图,
//   输出为普通 OBJ/MTL/PNG,Blender 端无需理解 CTM。

#include <string>
#include <vector>
#include <unordered_map>

// 前向声明:模型数据
struct ModelData;
struct Face;

// CTM 方法枚举
enum class CtmMethod {
    None,               // 无 CTM
    CtmCompact,         // 5 tile 紧凑连接(按象限合成)
    Ctm,                // 完整 47 tile 标准连接
    Horizontal,         // 水平连接(左右)
    Vertical,           // 垂直连接(上下)
    Random,             // 按坐标确定性随机
    Fixed,              // 固定替换贴图
    Top,                // 与主轴正方向邻居连接时替换侧面
    Overlay,            // 标准 17 tile 叠加层
    OverlayHorizontal,  // 叠加水平
    Repeat,             // 重复平铺
    Unknown             // 未知方法,不处理
};

// 一条 CTM 规则(对应一个 .properties 文件)
struct CtmRule {
    std::string ns;                     // 命名空间,例如 minecraft
    std::string baseDir;                // tile 所在目录(相对 namespace),例如 optifine/ctm/glass/glass
    std::vector<std::string> matchBlocks;  // matchBlocks 的完整 ns:block 名
    std::vector<std::string> matchTiles;   // matchTiles 值
    std::vector<std::string> matchTileNamespaces; // 每个 matchTiles 对应的纹理命名空间
    CtmMethod method = CtmMethod::None;
    std::vector<int> tiles;             // tile 编号列表
    std::vector<std::string> faces;     // faces 过滤(sides/all/north,...)
    std::string connect;                // connect=block / 空
    int width = 0;                      // repeat 专用: tile 网格宽
    int height = 0;                     // repeat 专用: tile 网格高
    std::string orient;                 // orient 模式(texture / none / state-axis 等)
    std::string symmetry = "none";      // random: none / opposite / all
    std::vector<int> weights;           // random: 可选权重
    int randomLoops = 0;                // random: 随机循环扰动
    bool linked = false;                // random: 同列方块共用随机值
    bool innerSeams = false;            // 连接时是否保留内接缝
    std::vector<std::string> connectBlocks; // overlay 邻居方块过滤
    std::vector<std::string> connectTiles;  // overlay 邻居贴图过滤
    std::string layer;                  // overlay 图层
    int tintIndex = -1;                 // tintIndex 数字值(命名值见 tintIndexName)
    std::string tintIndexName;          // tintIndex 非数字时的原名, 如 grass / stone
    std::string tintBlock;              // tintBlock 完整方块 id(含命名空间), 空=用当前方块
    bool hasTint = false;               // properties 是否显式声明了 tintIndex / tintBlock
    std::unordered_map<int, int> ctmOverrides; // ctm.N 自定义替换
    std::string resourceCondition;      // Continuity 资源条件
    std::string propertiesPath;         // properties 文件相对路径,用于调试
};

namespace GlobalCache {
    // CTM 资源缓存
    // ctmProperties: key = "namespace:properties相对路径" -> properties 原始文本
    extern std::unordered_map<std::string, std::string> ctmProperties;
    // ctmTextures: key = "namespace:ctm贴图相对路径(含optifine/ctm)" -> PNG 数据
    extern std::unordered_map<std::string, std::vector<unsigned char>> ctmTextures;
    // ctmPropertiesIndex / ctmTexturesIndex: 快速查找索引
    extern std::unordered_map<std::string, std::string> ctmPropertiesIndex;
    extern std::unordered_map<std::string, std::string> ctmTexturesIndex;
}

// ========= 初始化/解析 =========

// 解析所有已缓存的 ctmProperties,构建规则索引。在所有资源合并完成后调用一次。
void InitializeCtmRules();

// ========= 查询 =========

// 按命名空间+方块名+贴图名查找匹配的 CTM 规则。返回 nullptr 表示无匹配。
// textureName 为不含扩展名和 namespace 的贴图路径,例如 "block/glass"。
const CtmRule* FindCtmRule(const std::string& blockNs,
                           const std::string& blockName,
                           const std::string& textureNs,
                           const std::string& textureName);

// 判断给定面方向是否在规则的 faces 过滤内。faceName 为 down/up/north/south/west/east。
bool CtmRuleMatchesFace(const CtmRule& rule, const std::string& faceName);

// ========= 烘焙 =========

// 对一个方块实例的模型应用 CTM。
// 会根据世界邻居关系修改 model 中每个面的材质,必要时合成/保存 CTM 贴图。
// 参数:
//   model      方块模型(拷贝,可被修改)
//   ns         命名空间
//   blockName  不含命名空间的方块名,例如 "glass"
//   x,y,z      方块世界坐标,用于查询邻居
void ApplyCtmToBlockModel(ModelData& model,
                          const std::string& ns,
                          const std::string& blockName,
                          int x, int y, int z);

// 是否有任何 CTM 规则被加载(用于快速跳过 CTM 处理)
bool HasCtmRules();

#endif // CTM_H
