#include "init.h"
#include "RegionModelExporter.h"
#ifdef _WIN32
#include <windows.h>
#endif
void init() {
#ifdef _WIN32
    // 尝试将进程优先级设置为高
    if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
        // 可以选择在这里添加错误处理，例如打印一条警告信息
        std::cerr << "Warning: Failed to set process priority." << std::endl;
    }
#endif
    // 配置必须先加载
    SetGlobalLocale();
    // 删除纹理文件夹
    DeleteTexturesFolder();
    config = LoadConfig("config\\config.json");
    
    // 配置加载完成后，再初始化缓存
    InitializeAllCaches();
    LoadSolidBlocks(config.solidBlocksFile);
    LoadFluidBlocks(config.fluidsFile);
    RegisterFluidTextures();
    InitializeGlobalBlockPalette();
}