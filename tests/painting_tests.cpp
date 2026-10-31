#include "PaintingExporter.h"
#include "config.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

// WorldImporterCore references the global defined in main.cpp.
Config config;

namespace {

int gFailures = 0;

void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    ++gFailures;
}

bool near(float actual, double expected, double eps = 1e-4) {
    return std::fabs(static_cast<double>(actual) - expected) <= eps;
}

struct Vec {
    float x, y, z;
};

Vec at(const PaintingExporter::PaintingQuad& q, int i) {
    return {q.x[i], q.y[i], q.z[i]};
}

Vec sub(Vec a, Vec b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec cross(Vec a, Vec b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

void expectVec(Vec actual, double x, double y, double z, const std::string& label) {
    if (!near(actual.x, x) || !near(actual.y, y) || !near(actual.z, z)) {
        fail(label + " got (" + std::to_string(actual.x) + ", " + std::to_string(actual.y) +
             ", " + std::to_string(actual.z) + ")");
    }
}

void expectUv(const PaintingExporter::PaintingQuad& q, int i, double u, double v, const std::string& label) {
    if (!near(q.u[i], u) || !near(q.v[i], v)) {
        fail(label + " uv[" + std::to_string(i) + "] got (" + std::to_string(q.u[i]) + ", " +
             std::to_string(q.v[i]) + ")");
    }
}

void expectNormal(const PaintingExporter::PaintingQuad& q, double nx, double ny, double nz, const std::string& label) {
    const Vec n = cross(sub(at(q, 1), at(q, 0)), sub(at(q, 2), at(q, 0)));
    const double dot = n.x * nx + n.y * ny + n.z * nz;
    if (dot <= 1e-4) fail(label + " normal does not face outward");
    // The other two axes of a unit axis-aligned normal must be ~0.
    if (std::fabs(nx) < 0.5 && std::fabs(n.x) > 1e-4) fail(label + " normal has unexpected X");
    if (std::fabs(ny) < 0.5 && std::fabs(n.y) > 1e-4) fail(label + " normal has unexpected Y");
    if (std::fabs(nz) < 0.5 && std::fabs(n.z) > 1e-4) fail(label + " normal has unexpected Z");
}

void expectSize(const std::string& id, int width, int height) {
    const auto size = PaintingExporter::BuiltinPaintingSize(id);
    if (!size.known || size.width != width || size.height != height) {
        fail(id + " size");
    }
}

// Cell (i, j) starts after the single front quad. Order per cell: back, top, bottom, -U, +U.
size_t cellBase(int i, int j, int height) {
    return 1 + static_cast<size_t>((i * height + j) * 5);
}

void testBuiltinSizes() {
    expectSize("kebab", 1, 1);
    expectSize("minecraft:kebab", 1, 1);
    expectSize("painting/kebab", 1, 1);
    expectSize("minecraft:painting/kebab", 1, 1);
    expectSize("aztec", 1, 1);
    expectSize("alban", 1, 1);
    expectSize("aztec2", 1, 1);
    expectSize("bomb", 1, 1);
    expectSize("plant", 1, 1);
    expectSize("wasteland", 1, 1);
    expectSize("pool", 2, 1);
    expectSize("courbet", 2, 1);
    expectSize("sea", 2, 1);
    expectSize("sunset", 2, 1);
    expectSize("creebet", 2, 1);
    expectSize("wanderer", 1, 2);
    expectSize("graham", 1, 2);
    expectSize("match", 2, 2);
    expectSize("bust", 2, 2);
    expectSize("stage", 2, 2);
    expectSize("void", 2, 2);
    expectSize("skull_and_roses", 2, 2);
    expectSize("wither", 2, 2);
    expectSize("earth", 2, 2);
    expectSize("wind", 2, 2);
    expectSize("water", 2, 2);
    expectSize("fire", 2, 2);
    expectSize("fighters", 4, 2);
    expectSize("pointer", 4, 4);
    expectSize("pigscene", 4, 4);
    expectSize("burning_skull", 4, 4);
    expectSize("skeleton", 4, 3);
    expectSize("donkey_kong", 4, 3);
    if (PaintingExporter::BuiltinPaintingSize("backyard").known) fail("1.21-only backyard must not be built-in");
    if (PaintingExporter::BuiltinPaintingSize("").known) fail("empty id");
    if (PaintingExporter::BuiltinPaintingSize("minecraft:nope").known) fail("unknown id");
}

void testRejected() {
    if (!PaintingExporter::BuildPaintingMesh(0, 0, 0, -1, 1, 1).empty()) fail("facing -1");
    if (!PaintingExporter::BuildPaintingMesh(0, 0, 0, 4, 1, 1).empty()) fail("facing 4");
    if (!PaintingExporter::BuildPaintingMesh(0, 0, 0, 0, 0, 1).empty()) fail("width 0");
    if (!PaintingExporter::BuildPaintingMesh(0, 0, 0, 0, 1, -1).empty()) fail("height -1");
}

// South 1x1: viewer looks north, so u=0 is west (-X) and obj v=1 is the top.
void testSouth1x1() {
    const double cx = 2.5, cy = 152.5, cz = 11.03125;
    const double t = 1.0 / 32.0;
    const auto mesh = PaintingExporter::BuildPaintingMesh(cx, cy, cz, 0, 1, 1);
    if (mesh.size() != 6) {
        fail("south 1x1 quad count " + std::to_string(mesh.size()));
        return;
    }
    const auto& front = mesh[0];
    if (!front.front) fail("quad 0 is not the front");
    expectVec(at(front, 0), cx - 0.5, cy - 0.5, cz + t, "south front p0");
    expectVec(at(front, 1), cx + 0.5, cy - 0.5, cz + t, "south front p1");
    expectVec(at(front, 2), cx + 0.5, cy + 0.5, cz + t, "south front p2");
    expectVec(at(front, 3), cx - 0.5, cy + 0.5, cz + t, "south front p3");
    expectUv(front, 0, 0, 0, "south front");
    expectUv(front, 1, 1, 0, "south front");
    expectUv(front, 2, 1, 1, "south front");
    expectUv(front, 3, 0, 1, "south front");
    expectNormal(front, 0, 0, 1, "south front");

    const auto& back = mesh[1];
    if (back.front) fail("back marked front");
    expectVec(at(back, 0), cx - 0.5, cy - 0.5, cz - t, "south back p0");
    expectVec(at(back, 1), cx - 0.5, cy + 0.5, cz - t, "south back p1");
    expectVec(at(back, 2), cx + 0.5, cy + 0.5, cz - t, "south back p2");
    expectVec(at(back, 3), cx + 0.5, cy - 0.5, cz - t, "south back p3");
    expectUv(back, 0, 1, 0, "south back");
    expectUv(back, 1, 1, 1, "south back");
    expectUv(back, 2, 0, 1, "south back");
    expectUv(back, 3, 0, 0, "south back");
    expectNormal(back, 0, 0, -1, "south back");

    const auto& top = mesh[2];
    expectVec(at(top, 0), cx - 0.5, cy + 0.5, cz + t, "south top front-west");
    expectVec(at(top, 2), cx + 0.5, cy + 0.5, cz - t, "south top back-east");
    expectUv(top, 0, 0, 1, "south top");
    expectUv(top, 1, 1, 1, "south top");
    expectUv(top, 2, 1, 15.0 / 16.0, "south top");
    expectUv(top, 3, 0, 15.0 / 16.0, "south top");
    expectNormal(top, 0, 1, 0, "south top");

    const auto& bottom = mesh[3];
    expectUv(bottom, 0, 0, 1, "south bottom");
    expectUv(bottom, 2, 1, 15.0 / 16.0, "south bottom");
    expectNormal(bottom, 0, -1, 0, "south bottom");

    const auto& west = mesh[4];
    expectUv(west, 0, 0, 0, "south west edge");
    expectUv(west, 1, 0, 1, "south west edge");
    expectUv(west, 2, 1.0 / 16.0, 1, "south west edge");
    expectUv(west, 3, 1.0 / 16.0, 0, "south west edge");
    expectNormal(west, -1, 0, 0, "south west edge");

    const auto& east = mesh[5];
    expectUv(east, 0, 1.0 / 16.0, 0, "south east edge");
    expectUv(east, 1, 0, 0, "south east edge");
    expectUv(east, 2, 0, 1, "south east edge");
    expectUv(east, 3, 1.0 / 16.0, 1, "south east edge");
    expectNormal(east, 1, 0, 0, "south east edge");

    int fronts = 0;
    for (const auto& quad : mesh) if (quad.front) ++fronts;
    if (fronts != 1) fail("south 1x1 front count");
}

void expectFrontFrame(int facing, double cx, double cy, double cz, int width, int height,
    double ux, double uy, double uz, double nx, double ny, double nz, const std::string& label) {
    const auto mesh = PaintingExporter::BuildPaintingMesh(cx, cy, cz, facing, width, height);
    const size_t cells = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (mesh.size() != 1 + cells * 5) {
        fail(label + " quad count " + std::to_string(mesh.size()));
        return;
    }
    const auto& front = mesh[0];
    if (!front.front) fail(label + " missing front flag");
    const double t = 1.0 / 32.0;
    const double hw = width * 0.5;
    const double hh = height * 0.5;
    // p0 is u=0 (viewer's left, -U) at the bottom of the front plane.
    expectVec(at(front, 0),
        cx + nx * t - ux * hw, cy - hh, cz + nz * t - uz * hw, label + " p0");
    expectVec(at(front, 1),
        cx + nx * t + ux * hw, cy - hh, cz + nz * t + uz * hw, label + " p1");
    expectVec(at(front, 2),
        cx + nx * t + ux * hw, cy + hh, cz + nz * t + uz * hw, label + " p2");
    expectVec(at(front, 3),
        cx + nx * t - ux * hw, cy + hh, cz + nz * t - uz * hw, label + " p3");
    expectUv(front, 0, 0, 0, label);
    expectUv(front, 3, 0, 1, label);
    expectNormal(front, nx, ny, nz, label + " front");

    // u=0 sits on the viewer's left, which is the -U end.
    if (!near(front.u[0], 0) || !near(front.u[3], 0)) fail(label + " u=0 not on -U");
    if (!near(front.v[2], 1) || !near(front.v[3], 1)) fail(label + " obj v=1 not on top");

    int backs = 0;
    for (int i = 0; i < width; ++i) {
        for (int j = 0; j < height; ++j) {
            const auto& back = mesh[cellBase(i, j, height)];
            if (back.front) fail(label + " back flagged front");
            const double u0 = -hw + i;
            const double y0 = -hh + j;
            expectVec(at(back, 0),
                cx - nx * t + ux * u0, cy + y0, cz - nz * t + uz * u0, label + " back cell");
            expectUv(back, 0, 1, 0, label + " back");
            expectUv(back, 2, 0, 1, label + " back");
            expectNormal(back, -nx, -ny, -nz, label + " back");
            const auto& side = mesh[cellBase(i, j, height) + 3];
            expectUv(side, 0, 0, 0, label + " -U");
            expectUv(side, 2, 1.0 / 16.0, 1, label + " -U");
            ++backs;
        }
    }
    if (backs != static_cast<int>(cells)) fail(label + " back count");
}

void testFacings() {
    // u axis is the viewer's right. u=0 is therefore the opposite end.
    // south +Z, west -X, north -Z, east +X. Rotated around Y from the south frame.
    testSouth1x1();
    expectFrontFrame(1, 139.96875, 152.5, 2.5, 1, 1, 0, 0, 1, -1, 0, 0, "west");
    expectFrontFrame(2, 2.5, 152.5, 9.96875, 1, 1, -1, 0, 0, 0, 0, -1, "north");
    expectFrontFrame(3, 141.03125, 152.5, 2.5, 1, 1, 0, 0, -1, 1, 0, 0, "east");
    // 2x2 centred on Pos, four back cells, each using the full back texture.
    expectFrontFrame(0, 3.0, 158.0, 11.03125, 2, 2, 1, 0, 0, 0, 0, 1, "south 2x2");
    expectFrontFrame(0, 8.0, 158.0, 11.03125, 4, 4, 1, 0, 0, 0, 0, 1, "south 4x4");
    expectFrontFrame(2, 58.0, 157.5, 9.96875, 4, 3, -1, 0, 0, 0, 0, -1, "north 4x3");
}

} // namespace

int main() {
    testBuiltinSizes();
    testRejected();
    testFacings();
    if (gFailures != 0) {
        std::cerr << gFailures << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS: painting placement, facing, size and UVs\n";
    return 0;
}
