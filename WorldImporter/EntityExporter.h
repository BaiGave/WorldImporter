#ifndef ENTITY_EXPORTER_H
#define ENTITY_EXPORTER_H

#include <string>

namespace EntityExporter {
// 读取所选维度的 entities/r.*.*.mca，将区域内实体写为 entities.json。
// 返回实际导出的实体数量。
size_t Export(const std::string& outputPath = "entities.json");
}

#endif
