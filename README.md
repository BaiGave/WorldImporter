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




