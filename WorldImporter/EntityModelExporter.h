#ifndef ENTITY_MODEL_EXPORTER_H
#define ENTITY_MODEL_EXPORTER_H

#include <string>

namespace EntityModelExporter {
// 把选区内的生物导出为 <name>.obj / <name>.mtl (每个实体一个 OBJ 对象), 贴图写入 textures/,
// 村民职业/类型/等级等叠加层登记到 overlay.json。返回导出的实体数。
size_t Export(const std::string& name = "entities");
}

#endif
