#ifndef ENTITY_EXPORTER_H
#define ENTITY_EXPORTER_H

#include <functional>
#include <string>
#include "nbtutils.h"

namespace EntityExporter {
// 读取所选维度的 entities/r.*.*.mca，将区域内实体写为 entities.json。
// 返回实际导出的实体数量。
size_t Export(const std::string& outputPath = "entities.json");
// 遍历所选维度中与选区相交的区域文件里的全部实体(含 Passengers), 返回读取的区块数。
size_t VisitEntities(const std::function<void(const NbtTagPtr&)>& visit);
}

#endif
