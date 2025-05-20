#include "init.h"
#include "RegionModelExporter.h"
void init() {
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