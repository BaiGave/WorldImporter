# WorldImporter
 C++软件，可以导入MC地图

## 使用 CMake + vcpkg 构建
POSIX shell:
```bash
cmake -S. -Bbuild -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystem/vcpkg.cmake" [更多参数]
cmake --build build

```
Powershell:
```ps
cmake -S. -Bbuild -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystem/vcpkg.cmake" [更多参数]
cmake --build build
```
