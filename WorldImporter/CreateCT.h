// CreateCT.h
// ==================== Create 机械动力 连接纹理支持 ====================
//
// Create 的连接纹理不走 OptiFine CTM 协议，而是运行时把方块模型里
// 引用原始贴图(如 block/brass_casing)的 quad 的 UV 重映射到
// * 专用连接贴图(如 block/brass_casing_connected.png)的 sheet 格子上。
//
// 贴图 sheet 布局由 AllCTTypes 决定:
//   HORIZONTAL / VERTICAL    2x2 格子
//   ROOF / RECTANGLE / CROSS 4x4 格子
//   OMNIDIRECTIONAL          8x8 格子
//
// 本模块移植 Create 的 CTSpriteShiftEntry + AllCTTypes 算法:
//   - 按方块注册 CT 行为 (同 Create 的 ConnectedTextureBehaviour), 行为按面/原贴图选 sprite shift
//   - 根据世界邻居关系计算 CTContext (up/down/left/right + 4 角, connectsTo/isBeingBlocked)
//   - 调用 getTextureIndex 得到 sheet 格子编号
//   - 把面 UV 重映射到该格子 (u'=(u+col)/size, v'=(sheetSize-1-row+v)/size)
//   - 材质换成整张 connected 贴图
//
// 导出结果为普通 OBJ/MTL/PNG，Blender 端无需理解 Create 的机制。

#ifndef CREATECT_H
#define CREATECT_H

#include <string>
#include <vector>
#include "model.h"

// CT 类型（对应 Create 的 AllCTTypes 枚举）
enum class CreateCTType {
    HORIZONTAL,           // 2x2  水平连接
    HORIZONTAL_KRYPPERS,  // 2x2  水平连接（书架风格变体）
    VERTICAL,             // 2x2  垂直连接
    OMNIDIRECTIONAL,      // 8x8  全方向（机壳/玻璃）
    ROOF,                 // 4x4  屋顶/瓦片
    ROOF_STAIR,           // 4x4  屋顶楼梯
    CROSS,                // 4x4  十字
    RECTANGLE             // 4x4  矩形
};

// 对单个方块模型应用 Create CT。
// 只处理 sprite == shift.original 的面: 换 connected 材质 + 重映射 UV。
// 返回 true 表示至少有一个面应用了 CT。
bool ApplyCreateCTToBlockModel(ModelData& model, const std::string& ns,
    const std::string& blockName, int x, int y, int z);

// 初始化(解析注册表, 当前为静态数据, 仅做占位)
void InitializeCreateCT();

#endif // CREATECT_H
