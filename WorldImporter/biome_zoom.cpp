#include "biome.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

// Vanilla biome zoom and swamp grass noise.
//
// Checked against 1.21.1 (obfuscated, Mojang map m1211.txt) and 26.3 (named):
//   BiomeManager            ddy     getBiome / getFiddledDistance / getFiddle / obfuscateSeed
//   LinearCongruentialGenerator ayj next(long, long)
//   ClientLevel.calculateBlockTint fzf.b   integer mean over (2r+1)^2, same Y
//   GrassColorModifier.SWAMP       Biome.BIOME_INFO_NOISE (ddw.e)
//   PerlinSimplexNoise eoj, SimplexNoise eok, LegacyRandomSource dyz,
//   WorldgenRandom dzx, BitRandomSource dyn, Mth.floor ayo.a
//
// obfuscateSeed = Guava Hashing.sha256().hashLong(seed).asLong():
// putLong writes the long little-endian. BytesHashCode.padToLong (Guava 32.1.2,
// what HashCode.asLong calls) shifts digest byte i by 8*i, so byte 0 is the
// low byte of the returned long.
// BiomeManager's constructor stores that already-hashed long (the server hashes
// before constructing it). getFiddle uses Math.floorMod(rval >> 24, 1024).

namespace {

uint32_t Rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

void Sha256(const uint8_t* msg, size_t len, uint8_t out[32]) {
    static const uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };

    uint32_t h0 = 0x6a09e667u, h1 = 0xbb67ae85u, h2 = 0x3c6ef372u, h3 = 0xa54ff53au;
    uint32_t h4 = 0x510e527fu, h5 = 0x9b05688cu, h6 = 0x1f83d9abu, h7 = 0x5be0cd19u;

    const uint64_t bitLen = static_cast<uint64_t>(len) * 8u;
    size_t padLen = len + 1;
    while ((padLen % 64) != 56) ++padLen;
    std::vector<uint8_t> buf(padLen + 8, 0);
    if (len) std::memcpy(buf.data(), msg, len);
    buf[len] = 0x80;
    for (int i = 0; i < 8; ++i) {
        buf[padLen + static_cast<size_t>(i)] = static_cast<uint8_t>(bitLen >> (56 - 8 * i));
    }

    for (size_t off = 0; off < buf.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const size_t p = off + static_cast<size_t>(i) * 4;
            w[i] = (uint32_t(buf[p]) << 24) | (uint32_t(buf[p + 1]) << 16) |
                   (uint32_t(buf[p + 2]) << 8) | uint32_t(buf[p + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t t1 = h + S1 + ch + K[i] + w[i];
            const uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e; h5 += f; h6 += g; h7 += h;
    }

    const uint32_t hs[8] = {h0, h1, h2, h3, h4, h5, h6, h7};
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<uint8_t>(hs[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(hs[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(hs[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(hs[i]);
    }
}

double Fiddle(int64_t rval) {
    // Math.floorMod(rval >> 24, 1024) / 1024.0, then (u - 0.5) * 0.9.
    const int64_t shifted = rval >> 24;
    int64_t mod = shifted % 1024;
    if (mod < 0) mod += 1024;
    const double uniform = static_cast<double>(mod) / 1024.0;
    return (uniform - 0.5) * 0.9;
}

// BitRandomSource.next / nextInt / nextDouble on top of LegacyRandomSource.
// WorldgenRandom(LegacyRandomSource) does not reseed the delegate (setSeed
// returns while the delegate field is still null) and next(bits) only
// forwards, so a direct LegacyRandomSource(2345) matches BIOME_INFO_NOISE.
struct LegacyRng {
    uint64_t seed;
    explicit LegacyRng(uint64_t setSeed) {
        seed = (setSeed ^ 0x5DEECE66DULL) & 0xFFFFFFFFFFFFULL;
    }
    int next(int bits) {
        seed = (seed * 25214903917ULL + 11ULL) & 0xFFFFFFFFFFFFULL;
        return static_cast<int>(seed >> (48 - bits));
    }
    double nextDouble() {
        const int hi = next(26);
        const int lo = next(27);
        const int64_t combined = (static_cast<int64_t>(hi) << 27) + static_cast<int64_t>(lo);
        // 1.110223E-16f is a float constant widened to double (dyn.c), not 2^-53.
        return static_cast<double>(combined) * static_cast<double>(1.110223E-16f);
    }
    int nextInt(int bound) {
        if ((bound & (bound - 1)) == 0) {
            return static_cast<int>((static_cast<int64_t>(bound) * static_cast<int64_t>(next(31))) >> 31);
        }
        for (;;) {
            const int bits = next(31);
            const int val = bits % bound;
            // Java int wrap of (bits - val + (bound - 1)) < 0.
            const uint32_t wrapped = static_cast<uint32_t>(bits) - static_cast<uint32_t>(val) +
                                     static_cast<uint32_t>(bound - 1);
            if (static_cast<int32_t>(wrapped) >= 0) return val;
        }
    }
};

int FloorToInt(double value) {
    // Mth.floor: (int) truncates toward 0, then subtract 1 when value < (double)truncated.
    const int truncated = static_cast<int>(value);
    return value < static_cast<double>(truncated) ? truncated - 1 : truncated;
}

// SimplexNoise 2D (eok.a(double, double)). Octave list {0} makes
// PerlinSimplexNoise.getValue(x, z, false) this single sample.
struct Simplex2D {
    static constexpr int kGrad[12][3] = {
        {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0},
        {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
        {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}
    };
    int perm[256];

    Simplex2D() {
        LegacyRng rng(2345);
        (void)rng.nextDouble();
        (void)rng.nextDouble();
        (void)rng.nextDouble();
        for (int i = 0; i < 256; ++i) perm[i] = i;
        for (int i = 0; i < 256; ++i) {
            const int j = rng.nextInt(256 - i);
            std::swap(perm[i], perm[i + j]);
        }
    }

    int at(int i) const { return perm[i & 255]; }

    static double Contrib(int gi, double x, double y) {
        double t = 0.5 - x * x - y * y;
        if (t < 0.0) return 0.0;
        t *= t;
        const double dot = static_cast<double>(kGrad[gi][0]) * x + static_cast<double>(kGrad[gi][1]) * y;
        return t * t * dot;
    }

    double sample(double x, double y) const {
        const double sqrt3 = std::sqrt(3.0);
        const double f2 = 0.5 * (sqrt3 - 1.0);
        const double g2 = (3.0 - sqrt3) / 6.0;
        const double skew = (x + y) * f2;
        const int i = FloorToInt(x + skew);
        const int j = FloorToInt(y + skew);
        const double t = static_cast<double>(i + j) * g2;
        const double x0 = x - (static_cast<double>(i) - t);
        const double y0 = y - (static_cast<double>(j) - t);
        const int i1 = (x0 > y0) ? 1 : 0;
        const int j1 = (x0 > y0) ? 0 : 1;
        const double x1 = x0 - static_cast<double>(i1) + g2;
        const double y1 = y0 - static_cast<double>(j1) + g2;
        const double x2 = x0 - 1.0 + 2.0 * g2;
        const double y2 = y0 - 1.0 + 2.0 * g2;
        const int ii = i & 255;
        const int jj = j & 255;
        const int gi0 = at(ii + at(jj)) % 12;
        const int gi1 = at(ii + i1 + at(jj + j1)) % 12;
        const int gi2 = at(ii + 1 + at(jj + 1)) % 12;
        const double n0 = Contrib(gi0, x0, y0);
        const double n1 = Contrib(gi1, x1, y1);
        const double n2 = Contrib(gi2, x2, y2);
        return 70.0 * (n0 + n1 + n2);
    }
};

const Simplex2D& SwampSimplex() {
    static const Simplex2D noise;
    return noise;
}

}  // namespace

int64_t BiomeObfuscateSeed(int64_t worldSeed) {
    uint8_t msg[8];
    const uint64_t bits = static_cast<uint64_t>(worldSeed);
    for (int i = 0; i < 8; ++i) msg[i] = static_cast<uint8_t>(bits >> (8 * i));
    uint8_t dig[32];
    Sha256(msg, 8, dig);
    uint64_t out = 0;
    for (int i = 0; i < 8; ++i) out |= static_cast<uint64_t>(dig[i]) << (8 * i);
    return static_cast<int64_t>(out);
}

int64_t BiomeLcgNext(int64_t seed, int64_t salt) {
    uint64_t mixed = static_cast<uint64_t>(seed);
    mixed *= mixed * 6364136223846793005ULL + 1442695040888963407ULL;
    mixed += static_cast<uint64_t>(salt);
    return static_cast<int64_t>(mixed);
}

double BiomeFiddledDistance(int64_t zoomSeed, int qx, int qy, int qz, double dx, double dy, double dz) {
    int64_t rval = zoomSeed;
    rval = BiomeLcgNext(rval, qx);
    rval = BiomeLcgNext(rval, qy);
    rval = BiomeLcgNext(rval, qz);
    rval = BiomeLcgNext(rval, qx);
    rval = BiomeLcgNext(rval, qy);
    rval = BiomeLcgNext(rval, qz);
    const double fiddleX = Fiddle(rval);
    rval = BiomeLcgNext(rval, zoomSeed);
    const double fiddleY = Fiddle(rval);
    rval = BiomeLcgNext(rval, zoomSeed);
    const double fiddleZ = Fiddle(rval);
    const double sx = dx + fiddleX;
    const double sy = dy + fiddleY;
    const double sz = dz + fiddleZ;
    return sz * sz + sy * sy + sx * sx;
}

void BiomeChosenQuart(int64_t zoomSeed, int x, int y, int z, int& qx, int& qy, int& qz) {
    const int absX = x - 2;
    const int absY = y - 2;
    const int absZ = z - 2;
    const int parentX = absX >> 2;
    const int parentY = absY >> 2;
    const int parentZ = absZ >> 2;
    const double fractX = static_cast<double>(absX & 3) / 4.0;
    const double fractY = static_cast<double>(absY & 3) / 4.0;
    const double fractZ = static_cast<double>(absZ & 3) / 4.0;
    int best = 0;
    double bestDist = std::numeric_limits<double>::infinity();
    for (int i = 0; i < 8; ++i) {
        const bool xEven = (i & 4) == 0;
        const bool yEven = (i & 2) == 0;
        const bool zEven = (i & 1) == 0;
        const int cornerX = xEven ? parentX : parentX + 1;
        const int cornerY = yEven ? parentY : parentY + 1;
        const int cornerZ = zEven ? parentZ : parentZ + 1;
        const double dist = BiomeFiddledDistance(
            zoomSeed, cornerX, cornerY, cornerZ,
            xEven ? fractX : fractX - 1.0,
            yEven ? fractY : fractY - 1.0,
            zEven ? fractZ : fractZ - 1.0);
        // Strict >: an equal distance keeps the lower corner index.
        if (bestDist > dist) {
            best = i;
            bestDist = dist;
        }
    }
    qx = (best & 4) == 0 ? parentX : parentX + 1;
    qy = (best & 2) == 0 ? parentY : parentY + 1;
    qz = (best & 1) == 0 ? parentZ : parentZ + 1;
}

double BiomeSwampNoise(int blockX, int blockZ) {
    return SwampSimplex().sample(static_cast<double>(blockX) * 0.0225, static_cast<double>(blockZ) * 0.0225);
}

int BiomeSwampGrassColor(int blockX, int blockZ) {
    // Strict <, same as GrassColorModifier.SWAMP. 5011004 = 0x4C763C, 6975545 = 0x6A7039.
    return BiomeSwampNoise(blockX, blockZ) < -0.1 ? 5011004 : 6975545;
}

int BiomeBlendTint(const int* colors, int count) {
    if (!colors || count <= 0) return 0;
    int red = 0, green = 0, blue = 0;
    for (int i = 0; i < count; ++i) {
        const int color = colors[i];
        red += (color >> 16) & 0xFF;
        green += (color >> 8) & 0xFF;
        blue += color & 0xFF;
    }
    // Toward-zero division by the full sample count, then the low 8 bits, matching
    // ClientLevel.calculateBlockTint (Cursor3D walks X then Z; the sum is commutative).
    return ((red / count) & 0xFF) << 16 | ((green / count) & 0xFF) << 8 | ((blue / count) & 0xFF);
}
