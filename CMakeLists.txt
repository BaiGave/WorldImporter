cmake_minimum_required(VERSION 3.30.0)
project(WorldImporter VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ZLIB REQUIRED)
find_package(libzip CONFIG REQUIRED)

file(GLOB SOURCE_FILES CONFIGURE_DEPENDS "WorldImporter/*.cpp")
list(REMOVE_ITEM SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/main.cpp")
add_library(WorldImporterCore STATIC ${SOURCE_FILES})
target_include_directories(WorldImporterCore PUBLIC WorldImporter)
target_link_libraries(WorldImporterCore PUBLIC libzip::zip ZLIB::ZLIB)
if (WIN32)
    target_link_libraries(WorldImporterCore PUBLIC winhttp)
endif()
add_executable(WorldImporter WorldImporter/main.cpp)
target_link_libraries(WorldImporter PRIVATE WorldImporterCore)

# Windows 正式发行使用 MSVC：其线程运行时在多模型线程下稳定，且 /EHsc
# 保证 try/catch 能正确展开。/utf-8 避免中文注释源文件按本地代码页误解析。
function(wi_apply_defaults target)
    if (MSVC)
        target_compile_options(${target} PRIVATE /EHsc /utf-8 /Zc:preprocessor)
        # Windows.h defines min/max macros that break std::min/std::max (C2589).
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endfunction()

wi_apply_defaults(WorldImporter)
wi_apply_defaults(WorldImporterCore)

include(CTest)

option(WI_BUILD_CTM_TESTS "Build CTM texture baking regression tests" ON)

if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/ctm_orient_tests.cpp")
    add_executable(ctm_orient_tests WorldImporter/tests/ctm_orient_tests.cpp)
    target_include_directories(ctm_orient_tests PRIVATE WorldImporter)
    wi_apply_defaults(ctm_orient_tests)
    add_test(NAME ctm_orient COMMAND ctm_orient_tests)
    set_tests_properties(ctm_orient PROPERTIES TIMEOUT 30)
endif()

if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/model_deduplicator_tests.cpp")
    add_executable(model_deduplicator_tests WorldImporter/tests/model_deduplicator_tests.cpp)
    target_link_libraries(model_deduplicator_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(model_deduplicator_tests)
    add_test(NAME model_deduplicator COMMAND model_deduplicator_tests)
    set_tests_properties(model_deduplicator PROPERTIES TIMEOUT 30)
endif()

if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/ctm_random_tests.cpp")
    add_executable(ctm_random_tests WorldImporter/tests/ctm_random_tests.cpp)
    target_link_libraries(ctm_random_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(ctm_random_tests)
    add_test(NAME ctm_random COMMAND ctm_random_tests)
    set_tests_properties(ctm_random PROPERTIES TIMEOUT 30)
endif()

if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/cache_init_tests.cpp")
    add_executable(cache_init_tests WorldImporter/tests/cache_init_tests.cpp)
    target_link_libraries(cache_init_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(cache_init_tests)
    add_test(NAME cache_init COMMAND cache_init_tests)
    set_tests_properties(cache_init PROPERTIES TIMEOUT 30)
endif()

if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/palette_format_tests.cpp")
    add_executable(palette_format_tests WorldImporter/tests/palette_format_tests.cpp)
    target_link_libraries(palette_format_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(palette_format_tests)
    add_test(NAME palette_format COMMAND palette_format_tests)
    set_tests_properties(palette_format PROPERTIES TIMEOUT 30)
endif()

# Pure sign-text helpers (no jar, no font cache). Links the extracted sources
# directly rather than WorldImporterCore so it stays fast.
if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/sign_text_tests.cpp")
    add_executable(sign_text_tests
        WorldImporter/tests/sign_text_tests.cpp
        WorldImporter/signtext_style.cpp
        WorldImporter/signtext_unihex.cpp
        WorldImporter/signtext_hash.cpp)
    target_include_directories(sign_text_tests PRIVATE WorldImporter)
    wi_apply_defaults(sign_text_tests)
    add_test(NAME sign_text COMMAND sign_text_tests)
    set_tests_properties(sign_text PROPERTIES TIMEOUT 30)
endif()

if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/resource_json_tests.cpp")
    add_executable(resource_json_tests WorldImporter/tests/resource_json_tests.cpp)
    target_link_libraries(resource_json_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(resource_json_tests)
    add_test(NAME resource_json COMMAND resource_json_tests)
    set_tests_properties(resource_json PROPERTIES TIMEOUT 30)
endif()

if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/uvlock_tests.cpp")
    add_executable(uvlock_tests WorldImporter/tests/uvlock_tests.cpp)
    target_link_libraries(uvlock_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(uvlock_tests)
    add_test(NAME uvlock COMMAND uvlock_tests)
    set_tests_properties(uvlock PROPERTIES TIMEOUT 30)
endif()

if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/obj_model_tests.cpp")
    add_executable(obj_model_tests WorldImporter/tests/obj_model_tests.cpp)
    target_link_libraries(obj_model_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(obj_model_tests)
    add_test(NAME obj_model COMMAND obj_model_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(obj_model PROPERTIES TIMEOUT 30)
endif()

# This harness ships with the importer; tests/ below is an optional external suite.
if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/biome_colors_tests.cpp")
    add_executable(biome_colors_tests WorldImporter/tests/biome_colors_tests.cpp)
    target_link_libraries(biome_colors_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(biome_colors_tests)
    add_test(NAME biome_colors COMMAND biome_colors_tests)

    set(WI_BIOME_LEGACY_ARCHIVE "" CACHE FILEPATH "Optional pre-1.21.11 JAR for biome regression")
    set(WI_BIOME_ATTRIBUTES_ARCHIVE "" CACHE FILEPATH "Optional 1.21.11+ JAR or Nether fixture ZIP")
    foreach(schema LEGACY ATTRIBUTES)
        if (WI_BIOME_${schema}_ARCHIVE)
            add_test(NAME biome_colors_${schema}
                COMMAND biome_colors_tests --${schema} "${WI_BIOME_${schema}_ARCHIVE}")
            set_tests_properties(biome_colors_${schema} PROPERTIES
                TIMEOUT 120 RESOURCE_LOCK biome_texture_output)
        endif()
    endforeach()
    set_tests_properties(biome_colors PROPERTIES TIMEOUT 30 RESOURCE_LOCK biome_texture_output)
endif()

if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/variant_pick_tests.cpp")
    add_executable(variant_pick_tests WorldImporter/tests/variant_pick_tests.cpp)
    target_link_libraries(variant_pick_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(variant_pick_tests)
    add_test(NAME variant_pick COMMAND variant_pick_tests)
    set_tests_properties(variant_pick PROPERTIES TIMEOUT 30)
endif()

if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/double_sided_merge_tests.cpp")
    add_executable(double_sided_merge_tests WorldImporter/tests/double_sided_merge_tests.cpp)
    target_link_libraries(double_sided_merge_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(double_sided_merge_tests)
    add_test(NAME double_sided_merge COMMAND double_sided_merge_tests)
    set_tests_properties(double_sided_merge PROPERTIES TIMEOUT 30)
endif()

if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/plant_offset_tests.cpp")
    add_executable(plant_offset_tests WorldImporter/tests/plant_offset_tests.cpp)
    target_link_libraries(plant_offset_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(plant_offset_tests)
    add_test(NAME plant_offset COMMAND plant_offset_tests)
    set_tests_properties(plant_offset PROPERTIES TIMEOUT 30)
endif()

if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/legacy_biomes_tests.cpp")
    add_executable(legacy_biomes_tests WorldImporter/tests/legacy_biomes_tests.cpp)
    target_link_libraries(legacy_biomes_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(legacy_biomes_tests)
    add_test(NAME legacy_biomes COMMAND legacy_biomes_tests)
    set_tests_properties(legacy_biomes PROPERTIES TIMEOUT 30)
endif()

# Biome zoom / blend golden vectors (no world, no PNG). Java generators live in
# WorldImporter/tests/vref3 and are not required at ctest time.
if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/biome_blend_tests.cpp")
    add_executable(biome_blend_tests WorldImporter/tests/biome_blend_tests.cpp)
    target_link_libraries(biome_blend_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(biome_blend_tests)
    add_test(NAME biome_blend COMMAND biome_blend_tests)
    set_tests_properties(biome_blend PROPERTIES TIMEOUT 30)
endif()

if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/pot_sherds_tests.cpp")
    add_executable(pot_sherds_tests WorldImporter/tests/pot_sherds_tests.cpp)
    target_link_libraries(pot_sherds_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(pot_sherds_tests)
    add_test(NAME pot_sherds COMMAND pot_sherds_tests)
    set_tests_properties(pot_sherds PROPERTIES TIMEOUT 30)
endif()

# tests/ 是可选目录：删掉它（例如只发布主程序源码）时必须仍能正常编译主程序。
set(WI_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
if (BUILD_TESTING AND NOT EXISTS "${WI_TESTS_DIR}")
    message(STATUS "tests/ 不存在: 跳过测试目标（要跑测试请恢复 tests/，或显式传 -DBUILD_TESTING=OFF）")
endif()

if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/showblock_random_tests.cpp")
    add_executable(showblock_random_tests WorldImporter/tests/showblock_random_tests.cpp)
    target_link_libraries(showblock_random_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(showblock_random_tests)
    add_test(NAME showblock_random COMMAND showblock_random_tests)
    set_tests_properties(showblock_random PROPERTIES TIMEOUT 30)
endif()

if (BUILD_TESTING AND EXISTS "${WI_TESTS_DIR}")
if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/painting_tests.cpp")
    add_executable(painting_tests tests/painting_tests.cpp)
    target_link_libraries(painting_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(painting_tests)
    add_test(NAME painting COMMAND painting_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(painting PROPERTIES TIMEOUT 30)
endif()

# 下面这组用例随可选的 tests/ 套件一起发布。只放了 painting_tests.cpp 时不要把整组拉进来。
if (BUILD_TESTING AND EXISTS "${WI_TESTS_DIR}/chunk_io_tests.cpp")
    # 跑法（构建目录内）：
    #   ctest                        全部用例
    #   ctest -L ctm                 只跑 CTM 烘焙回归
    #   ctest -R chunk_io            只跑单个用例
    #   ctest --output-on-failure    失败时打印输出

    add_executable(chunk_io_tests
        tests/chunk_io_tests.cpp
        WorldImporter/chunk.cpp
        WorldImporter/decompressor.cpp
        WorldImporter/locutil.cpp
        WorldImporter/nbtutils.cpp)
    target_include_directories(chunk_io_tests PRIVATE WorldImporter)
    target_link_libraries(chunk_io_tests PRIVATE ZLIB::ZLIB)
    wi_apply_defaults(chunk_io_tests)
    add_test(NAME chunk_io COMMAND chunk_io_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(chunk_io PROPERTIES TIMEOUT 30)

    add_executable(import_geometry_tests tests/import_geometry_tests.cpp)
    target_link_libraries(import_geometry_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(import_geometry_tests)
    add_test(NAME import_geometry COMMAND import_geometry_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(import_geometry PROPERTIES TIMEOUT 30)

    add_executable(slab_regression_test tests/slab_regression_test.cpp)
    target_link_libraries(slab_regression_test PRIVATE WorldImporterCore)
    wi_apply_defaults(slab_regression_test)
    add_test(NAME slab_regression COMMAND slab_regression_test WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(slab_regression PROPERTIES TIMEOUT 30)

    add_executable(foliage_model_tests tests/foliage_model_tests.cpp)
    target_link_libraries(foliage_model_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(foliage_model_tests)
    add_test(NAME foliage_model COMMAND foliage_model_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(foliage_model PROPERTIES TIMEOUT 30)

    add_executable(variant_seed_tests tests/variant_seed_tests.cpp)
    target_link_libraries(variant_seed_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(variant_seed_tests)
    add_test(NAME variant_seed COMMAND variant_seed_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(variant_seed PROPERTIES TIMEOUT 30)

    # 独立 CTM 回归（自包含：直接编译 CTM.cpp）。烘焙产物写在测试可执行文件旁边，
    # 用例之间用 RESOURCE_LOCK 串行化，避免互相覆盖。
    if (WI_BUILD_CTM_TESTS)
        add_executable(ctm_regression tests/ctm_regression.cpp)
        target_link_libraries(ctm_regression PRIVATE libzip::zip ZLIB::ZLIB)
        wi_apply_defaults(ctm_regression)
        if (NOT MSVC)
            target_compile_definitions(ctm_regression PRIVATE _GLIBCXX_ASSERTIONS)
        endif()
        foreach(case plain padded tiny odd mixed mcmeta lowercase matching identity invalid prefix_tile road_tile_scope proxy_pattern grid_random pillar)
            add_test(NAME ctm_${case} COMMAND ctm_regression ${case}
                     WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
            set_tests_properties(ctm_${case} PROPERTIES
                RESOURCE_LOCK ctm_texture_output
                LABELS ctm)
        endforeach()
    endif()
endif()
endif()

if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/ctm_mod_grid_tests.cpp")
    add_executable(ctm_mod_grid_tests WorldImporter/tests/ctm_mod_grid_tests.cpp)
    target_link_libraries(ctm_mod_grid_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(ctm_mod_grid_tests)
    add_test(NAME ctm_mod_grid COMMAND ctm_mod_grid_tests)
    set_tests_properties(ctm_mod_grid PROPERTIES TIMEOUT 30)
endif()

# Mek CTM-mod + ModCT connect_to harness (optional; needs tests/modct_mek_test.cpp).
option(WI_BUILD_MODCT_MEK_TEST "Build Mek industrial CT adapter harness" OFF)
if (WI_BUILD_MODCT_MEK_TEST AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/modct_mek_test.cpp")
    add_executable(modct_mek_test tests/modct_mek_test.cpp)
    target_link_libraries(modct_mek_test PRIVATE libzip::zip ZLIB::ZLIB)
    wi_apply_defaults(modct_mek_test)
    if (NOT MSVC)
        target_compile_definitions(modct_mek_test PRIVATE _GLIBCXX_ASSERTIONS)
    endif()
endif()

if (BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/WorldImporter/tests/ctm_mod_pillar_tests.cpp")
    add_executable(ctm_mod_pillar_tests WorldImporter/tests/ctm_mod_pillar_tests.cpp)
    target_link_libraries(ctm_mod_pillar_tests PRIVATE WorldImporterCore)
    wi_apply_defaults(ctm_mod_pillar_tests)
    add_test(NAME ctm_mod_pillar COMMAND ctm_mod_pillar_tests)
    set_tests_properties(ctm_mod_pillar PROPERTIES TIMEOUT 30)
endif()
