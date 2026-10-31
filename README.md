[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/BaiGave/WorldImporter)
# WorldImporter
 C++软件，可以导入MC地图

## 使用 CMake 构建
```bash
cmake -S. -Bbuild --preset <预设> [更多参数]
cmake --build build
```
预设：`<构建类型>[-vcpkg]`

构建类型：`release`或`debug`

`-vcpkg`：使用 vcpkg 获取依赖

如 `release-vcpkg` 为使用 vcpkg 的发布构建

## 测试
`tests/` 是可选目录；本地存在该目录时，`BUILD_TESTING=ON` 会一并构建测试：
```bash
cmake -S. -Bbuild -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
- 用例：`chunk_io`、`import_geometry`、`slab_regression`、`foliage_model`，以及 CTM 烘焙回归（`ctest -L ctm`）。
- 不要测试用 `-DBUILD_TESTING=OFF`；`tests/` 缺失时构建会自动跳过测试目标，不影响主程序。

## 发布产物
- 仓库里只保留 `WorldImporter/WorldImporter.exe` 一份 exe；部署时拷贝到 Crafter 插件的 `addons/Crafter/importer/`。
- `build/` 等构建目录中的 exe 属于编译产物，不再入库（`.gitignore` 已覆盖）。




