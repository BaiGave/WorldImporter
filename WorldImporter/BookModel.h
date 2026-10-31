#pragma once

#include "model.h"
#include <string>

// 讲台(has_book)和附魔台的书是方块实体模型。贴图缺失时不修改 model。
void AppendBlockEntityBook(const std::string& ns, const std::string& blockId, ModelData& model);
