#include "BookModel.h"
#include "EntityCube.h"
#include "GlobalCache.h"

#include <cmath>
#include <shared_mutex>

namespace {

std::string BaseBlockId(const std::string& blockId) {
    std::string id = blockId;
    const size_t colon = id.find(':');
    const size_t bracket = id.find('[');
    if (colon != std::string::npos && (bracket == std::string::npos || colon < bracket))
        id = id.substr(colon + 1);
    const size_t lb = id.find('[');
    if (lb != std::string::npos) id = id.substr(0, lb);
    return id;
}

std::string BlockProperty(const std::string& blockId, const std::string& key) {
    const size_t lb = blockId.find('[');
    if (lb == std::string::npos) return "";
    size_t i = lb + 1;
    while (i < blockId.size() && blockId[i] != ']') {
        size_t end = blockId.find(',', i);
        if (end == std::string::npos) end = blockId.size();
        const size_t rb = blockId.find(']', i);
        if (rb != std::string::npos && rb < end) end = rb;
        const size_t eq = blockId.find('=', i);
        if (eq != std::string::npos && eq < end && blockId.compare(i, eq - i, key) == 0)
            return blockId.substr(eq + 1, end - eq - 1);
        if (end >= blockId.size()) break;
        i = end + 1;
    }
    return "";
}

// Direction.getClockWise
std::string ClockWise(const std::string& facing) {
    if (facing == "north") return "east";
    if (facing == "east") return "south";
    if (facing == "south") return "west";
    if (facing == "west") return "north";
    return "east";
}

// Direction.toYRot: south 0, west 90, north 180, east 270
float ToYRot(const std::string& dir) {
    if (dir == "west") return 90.0f;
    if (dir == "north") return 180.0f;
    if (dir == "east") return 270.0f;
    return 0.0f;
}

bool TextureCached(const std::string& ns, const std::string& path) {
    const std::string indexKey = "textures:" + ns + ":" + path;
    std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
    const auto idx = GlobalCache::textureIndex.find(indexKey);
    if (idx != GlobalCache::textureIndex.end()) {
        const auto it = GlobalCache::textures.find(idx->second);
        if (it != GlobalCache::textures.end() && !it->second.empty()) return true;
    }
    for (const std::string& modId : GlobalCache::jarOrder) {
        const auto it = GlobalCache::textures.find(modId + ":" + ns + ":" + path);
        if (it != GlobalCache::textures.end() && !it->second.empty()) return true;
    }
    return false;
}

// BookModel.createBodyLayer + setupAnim(time, flipLeft, flipRight, open).
// ModelPart.x 是像素；yRot 是弧度。renderer 的 PoseStack 在 root 里，这里不再额外 S(1,-1,-1)。
void AddBookParts(ModelData& model, int mat, const entitycube::Aff& root,
                  float time, float flipLeft, float flipRight, float open) {
    using namespace entitycube;
    const float o = (sinf(time * 0.02f) * 0.1f + 1.25f) * open;
    const float pageX = sinf(o);
    const float texW = 64.0f;
    const float texH = 32.0f;

    AddCube(model, mat, root * Pose(0.0f, 0.0f, -1.0f, 0.0f, kPi + o, 0.0f),
            0, 0, -6.0f, -5.0f, -0.005f, 6.0f, 10.0f, 0.005f, 0.0f, texW, texH);
    AddCube(model, mat, root * Pose(0.0f, 0.0f, 1.0f, 0.0f, -o, 0.0f),
            16, 0, 0.0f, -5.0f, -0.005f, 6.0f, 10.0f, 0.005f, 0.0f, texW, texH);
    AddCube(model, mat, root * Pose(0.0f, 0.0f, 0.0f, 0.0f, kPi * 0.5f, 0.0f),
            12, 0, -1.0f, -5.0f, 0.0f, 2.0f, 10.0f, 0.005f, 0.0f, texW, texH);
    AddCube(model, mat, root * Pose(pageX, 0.0f, 0.0f, 0.0f, o, 0.0f),
            0, 10, 0.0f, -4.0f, -0.99f, 5.0f, 8.0f, 1.0f, 0.0f, texW, texH);
    AddCube(model, mat, root * Pose(pageX, 0.0f, 0.0f, 0.0f, -o, 0.0f),
            12, 10, 0.0f, -4.0f, -0.01f, 5.0f, 8.0f, 1.0f, 0.0f, texW, texH);
    AddCube(model, mat, root * Pose(pageX, 0.0f, 0.0f, 0.0f, o - o * 2.0f * flipLeft, 0.0f),
            24, 10, 0.0f, -4.0f, 0.0f, 5.0f, 8.0f, 0.005f, 0.0f, texW, texH);
    AddCube(model, mat, root * Pose(pageX, 0.0f, 0.0f, 0.0f, o - o * 2.0f * flipRight, 0.0f),
            24, 10, 0.0f, -4.0f, 0.0f, 5.0f, 8.0f, 0.005f, 0.0f, texW, texH);
}

}  // namespace

void AppendBlockEntityBook(const std::string& ns, const std::string& blockId, ModelData& model) {
    using namespace entitycube;
    if (ns != "minecraft") return;

    const std::string base = BaseBlockId(blockId);
    Aff root;
    float time = 0.0f;
    float flipLeft = 0.0f;
    float flipRight = 0.0f;
    float open = 0.0f;

    if (base == "lectern") {
        if (BlockProperty(blockId, "has_book") != "true") return;
        std::string facing = BlockProperty(blockId, "facing");
        if (facing.empty()) facing = "north";
        // LecternRenderer: T(0.5,1.0625,0.5) * RY(-clockWise.toYRot) * RZ(67.5) * T(0,-0.125,0)
        // setupAnim(0, 0.1, 0.9, 1.2) -> o = 1.5
        const float yRot = ToYRot(ClockWise(facing));
        root = T(0.5f, 1.0625f, 0.5f) * RY(Deg(-yRot)) * RZ(Deg(67.5f)) * T(0.0f, -0.125f, 0.0f);
        flipLeft = 0.1f;
        flipRight = 0.9f;
        open = 1.2f;
    } else if (base == "enchanting_table") {
        // 静止方块实体: time=0, rot=0, open=0, flip=0
        // T(0.5,0.75,0.5) * T(0, 0.1+sin(time*0.1)*0.01, 0) * RY(0) * RZ(80)
        // setupAnim(0, clamp(frac(0.25)*1.6-0.3)=0.1, clamp(frac(0.75)*1.6-0.3)=0.9, 0) -> o = 0
        const float bob = sinf(time * 0.1f) * 0.01f;
        root = T(0.5f, 0.75f, 0.5f) * T(0.0f, 0.1f + bob, 0.0f) * RY(0.0f) * RZ(Deg(80.0f));
        flipLeft = 0.1f;
        flipRight = 0.9f;
        open = 0.0f;
    } else {
        return;
    }

    const char* paths[] = {
        "entity/enchantment/enchanting_table_book",
        "entity/enchanting_table_book",
    };
    ModelData book;
    int mat = -1;
    const std::string matName = ns + ":" + base + "#book";
    for (const char* path : paths) {
        if (!TextureCached(ns, path)) continue;
        mat = AddMaterial(book, ns, matName, path);
        if (mat >= 0) break;
    }
    if (mat < 0) return;

    AddBookParts(book, mat, root, time, flipLeft, flipRight, open);
    MergeModelsDirectly(model, book);
}
