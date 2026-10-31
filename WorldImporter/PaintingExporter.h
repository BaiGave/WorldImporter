#ifndef PAINTING_EXPORTER_H
#define PAINTING_EXPORTER_H

#include <string>
#include <vector>

namespace PaintingExporter {

// One quad in the same world space as region_models.obj (1 block = 1 unit,
// no extra origin shift). vt uses the block-face V flip: texture top is v=1.
struct PaintingQuad {
    float x[4];
    float y[4];
    float z[4];
    float u[4];
    float v[4];
    bool front; // true: painting artwork; false: painting/back.png (back or edge)
};

// w x h x (1/16) box centred on Pos. facing is the 2D value
// 0=south, 1=west, 2=north, 3=east (direction the painting faces).
// Empty when facing or size is unusable.
std::vector<PaintingQuad> BuildPaintingMesh(double posX, double posY, double posZ,
    int facing, int width, int height);

struct PaintingSize {
    int width = 0;
    int height = 0;
    bool known = false;
};

// Built-in sizes for <=1.20.x (no data/<ns>/painting_variant). Accepts
// "kebab" or "minecraft:kebab".
PaintingSize BuiltinPaintingSize(const std::string& variantId);

// Always-on export of minecraft:painting entities whose Pos lies in the
// configured block box and dimension. Writes paintings.obj/.mtl next to
// region_models.obj, or deletes a stale pair when nothing is in range.
// Returns the number of paintings written.
size_t Export();

}

#endif
