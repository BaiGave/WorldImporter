#include "CampfireItems.h"
#include "ItemRender.h"

#include <algorithm>
#include <cctype>
#include <string>

using namespace std;

namespace {

string Lower(string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// Block name may be "minecraft:campfire[facing=north,lit=true]" or without ns.
string BaseBlockId(const string& nameWithState) {
    string s = nameWithState;
    size_t bracket = s.find('[');
    if (bracket != string::npos) s = s.substr(0, bracket);
    if (s.rfind("minecraft:", 0) == 0) s = s.substr(10);
    return Lower(s);
}

string StateValue(const string& nameWithState, const string& key) {
    size_t bracket = nameWithState.find('[');
    if (bracket == string::npos) return {};
    size_t end = nameWithState.find(']', bracket);
    string body = nameWithState.substr(bracket + 1,
        end == string::npos ? string::npos : end - bracket - 1);
    string needle = key + "=";
    size_t p = 0;
    while (p < body.size()) {
        size_t comma = body.find(',', p);
        string part = body.substr(p, comma == string::npos ? string::npos : comma - p);
        if (part.rfind(needle, 0) == 0) return Lower(part.substr(needle.size()));
        if (comma == string::npos) break;
        p = comma + 1;
    }
    return {};
}

// Direction.get2DDataValue: S=0, W=1, N=2, E=3
int Facing2D(const string& facing) {
    if (facing == "south") return 0;
    if (facing == "west") return 1;
    if (facing == "east") return 3;
    return 2; // north default (CampfireBlock)
}

// Direction.toYRot for horizontal dirs
float DirToYRot(int data2d) {
    switch (data2d & 3) {
    case 0: return 0.0f;     // south
    case 1: return 90.0f;    // west
    case 2: return 180.0f;   // north
    default: return 270.0f;  // east
    }
}

int SlotOf(const NbtTagPtr& stack) {
    if (!stack || stack->type != TagType::COMPOUND) return -1;
    auto t = getChildByName(stack, "Slot");
    if (!t) return -1;
    if (t->type == TagType::BYTE) return bytesToByte(t->payload) & 0xFF;
    if (t->type == TagType::INT) return bytesToInt(t->payload);
    if (t->type == TagType::SHORT) return bytesToShort(t->payload);
    return -1;
}

void Emit(ModelData& model, ModelData mesh, const EntityAffine& pose) {
    ItemRender::Transform(mesh, pose);
    for (auto& face : mesh.faces) face.faceDirection = FaceType::DO_NOT_CULL;
    if (model.vertices.empty()) model = std::move(mesh);
    else MergeModelsDirectly(model, mesh);
}

}  // namespace

bool AppendCampfireItems(ModelData& blockModel, const NbtTagPtr& blockEntityNbt,
                         const std::string& blockNameWithState, int /*x*/, int /*y*/, int /*z*/) {
    if (!blockEntityNbt || blockEntityNbt->type != TagType::COMPOUND) return false;
    const string id = BaseBlockId(blockNameWithState);
    if (id != "campfire" && id != "soul_campfire") return false;

    auto items = getChildByName(blockEntityNbt, "Items");
    if (!items || items->type != TagType::LIST || items->children.empty()) return false;

    const int facing = Facing2D(StateValue(blockNameWithState, "facing"));
    bool any = false;

    for (const auto& stackTag : items->children) {
        if (!stackTag || stackTag->type != TagType::COMPOUND) continue;
        int slot = SlotOf(stackTag);
        if (slot < 0 || slot > 3) {
            // Some older dumps omit Slot and use list order; accept 0..3 by index fallback later.
            continue;
        }
        ItemStackInfo item = ItemRender::ParseItemStack(stackTag);
        if (item.empty()) continue;

        ModelData mesh;
        if (!ItemRender::Build(item, ItemDisplay::Fixed, false, mesh)) continue;

        // CampfireRenderer.render (1.21.1):
        //   translate(0.5, 0.44921875, 0.5)
        //   dir = from2DDataValue((slot + facing.get2DDataValue()) % 4)
        //   mulPose Y(-dir.toYRot()); mulPose X(90)
        //   translate(-0.3125, -0.3125, 0); scale(0.375)
        // ItemRender::Build already applied Fixed display + translate(-0.5).
        const int dir = (slot + facing) & 3;
        const float yRot = -DirToYRot(dir);
        EntityAffine pose =
            EntityAffine::T(0.5, 0.44921875, 0.5) *
            EntityAffine::RYDeg(yRot) *
            EntityAffine::RXDeg(90.0) *
            EntityAffine::T(-0.3125, -0.3125, 0.0) *
            EntityAffine::S(0.375);
        Emit(blockModel, mesh, pose);
        any = true;
    }

    // Fallback: list order if no Slot tags (should not happen with addcf.py).
    if (!any) {
        int i = 0;
        for (const auto& stackTag : items->children) {
            if (i > 3) break;
            ItemStackInfo item = ItemRender::ParseItemStack(stackTag);
            if (item.empty()) { ++i; continue; }
            ModelData mesh;
            if (!ItemRender::Build(item, ItemDisplay::Fixed, false, mesh)) { ++i; continue; }
            const int dir = (i + facing) & 3;
            EntityAffine pose =
                EntityAffine::T(0.5, 0.44921875, 0.5) *
                EntityAffine::RYDeg(-DirToYRot(dir)) *
                EntityAffine::RXDeg(90.0) *
                EntityAffine::T(-0.3125, -0.3125, 0.0) *
                EntityAffine::S(0.375);
            Emit(blockModel, mesh, pose);
            any = true;
            ++i;
        }
    }
    return any;
}
