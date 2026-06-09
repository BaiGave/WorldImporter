#!/usr/bin/env bash
set -euo pipefail

# ============================================================
# WorldImporter 编译脚本 (MinGW-w64)
# 依赖: libzip, zlib (预编译库)
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$SCRIPT_DIR/WorldImporter"
OUT_DIR="$SRC_DIR/build"

# 预编译库路径 (libzip + zlib)
LIBZIP_BASE="/c/Users/Administrator/AppData/Local/Temp/libzip-install"
ZLIB_BASE="/c/Users/Administrator/AppData/Local/Temp/zlib-install"
MINGW_BASE="/c/Users/Administrator/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64"

# 源码列表 (对应 .vcxproj 中的 ClCompile 项)
SOURCES=(
    biome.cpp
    block.cpp
    blockstate.cpp
    chunk.cpp
    ChunkGenerator.cpp
    ChunkGroupAllocator.cpp
    ChunkLoader.cpp
    config.cpp
    decompressor.cpp
    EntityBlock.cpp
    fileutils.cpp
    Fluid.cpp
    GlobalCache.cpp
    init.cpp
    JarReader.cpp
    locutil.cpp
    LODManager.cpp
    main.cpp
    MemoryMonitor.cpp
    model.cpp
    ModelDeduplicator.cpp
    nbtutils.cpp
    ObjExporter.cpp
    RegionCache.cpp
    RegionModelExporter.cpp
    SpecialBlock.cpp
    TaskMonitor.cpp
    texture.cpp
)

# 拼接源码绝对路径
SRC_FILES=()
for src in "${SOURCES[@]}"; do
    SRC_FILES+=("$SRC_DIR/$src")
done

# 创建输出目录
mkdir -p "$OUT_DIR"

# 设置 MinGW 到 PATH
export PATH="$MINGW_BASE/bin:$PATH"

echo "=== WorldImporter Build ==="
echo "Compiler: $(g++ --version | head -1)"
echo "Target:   $OUT_DIR/WorldImporter.exe"
echo ""

# 编译并链接
g++ -std=c++20 \
    -I"$SRC_DIR" \
    -I"$SRC_DIR/include" \
    -I"$LIBZIP_BASE/include" \
    -I"$ZLIB_BASE/include" \
    -L"$LIBZIP_BASE/lib" \
    -L"$ZLIB_BASE/lib" \
    "${SRC_FILES[@]}" \
    -lzip -lzlib \
    -static-libgcc -static-libstdc++ \
    -lpthread \
    -O2 \
    -o "$OUT_DIR/WorldImporter.exe"

echo ""
echo "=== Build OK: $OUT_DIR/WorldImporter.exe ==="
ls -lh "$OUT_DIR/WorldImporter.exe"
