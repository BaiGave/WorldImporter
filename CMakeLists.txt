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
add_executable(WorldImporter WorldImporter/main.cpp)
target_link_libraries(WorldImporter PRIVATE WorldImporterCore)

# Windows 正式发行使用 MSVC：其线程运行时在多模型线程下稳定，且 /EHsc
# 保证 try/catch 能正确展开。/utf-8 避免中文注释源文件按本地代码页误解析。
if (MSVC)
    target_compile_options(WorldImporter PRIVATE /EHsc /utf-8 /Zc:preprocessor)
    target_compile_options(WorldImporterCore PRIVATE /EHsc /utf-8 /Zc:preprocessor)
endif()

include(CTest)
if (BUILD_TESTING)
    add_executable(chunk_io_tests
        tests/chunk_io_tests.cpp
        WorldImporter/chunk.cpp
        WorldImporter/decompressor.cpp
        WorldImporter/locutil.cpp
        WorldImporter/nbtutils.cpp)
    target_include_directories(chunk_io_tests PRIVATE WorldImporter)
    target_link_libraries(chunk_io_tests PRIVATE ZLIB::ZLIB)
    if (MSVC)
        target_compile_options(chunk_io_tests PRIVATE /EHsc /utf-8 /Zc:preprocessor)
    endif()
    add_test(NAME chunk_io COMMAND chunk_io_tests)
    set_tests_properties(chunk_io PROPERTIES TIMEOUT 30)

    add_executable(import_geometry_tests tests/import_geometry_tests.cpp)
    target_link_libraries(import_geometry_tests PRIVATE WorldImporterCore)
    if (MSVC)
        target_compile_options(import_geometry_tests PRIVATE /EHsc /utf-8 /Zc:preprocessor)
    endif()
    add_test(NAME import_geometry COMMAND import_geometry_tests)
    set_tests_properties(import_geometry PROPERTIES TIMEOUT 30)
endif()

if (BUILD_TESTING)
    add_executable(slab_regression_test tests/slab_regression_test.cpp)
    target_link_libraries(slab_regression_test PRIVATE WorldImporterCore)
    if (MSVC)
        target_compile_options(slab_regression_test PRIVATE /EHsc /utf-8 /Zc:preprocessor)
    endif()
    add_test(NAME slab_regression COMMAND slab_regression_test)
    set_tests_properties(slab_regression PROPERTIES TIMEOUT 30)

    add_executable(foliage_model_tests tests/foliage_model_tests.cpp)
    target_link_libraries(foliage_model_tests PRIVATE WorldImporterCore)
    if (MSVC)
        target_compile_options(foliage_model_tests PRIVATE /EHsc /utf-8 /Zc:preprocessor)
    endif()
    add_test(NAME foliage_model COMMAND foliage_model_tests)
    set_tests_properties(foliage_model PROPERTIES TIMEOUT 30)
endif()

# Standalone CTM regression harness (self-contained: compiles CTM.cpp directly).
option(WI_BUILD_CTM_TESTS "Build CTM texture baking regression tests" OFF)
if (WI_BUILD_CTM_TESTS)
    add_executable(ctm_regression tests/ctm_regression.cpp)
    target_link_libraries(ctm_regression PRIVATE libzip::zip ZLIB::ZLIB)
    if (MSVC)
        target_compile_options(ctm_regression PRIVATE /EHsc /utf-8 /Zc:preprocessor)
    else()
        target_compile_definitions(ctm_regression PRIVATE _GLIBCXX_ASSERTIONS)
    endif()
    foreach(case plain padded tiny odd mixed mcmeta lowercase matching identity invalid prefix_tile proxy_pattern grid_random pillar)
        add_test(NAME ctm_${case} COMMAND ctm_regression ${case})
        set_tests_properties(ctm_${case} PROPERTIES RESOURCE_LOCK ctm_texture_output)
    endforeach()
endif()

