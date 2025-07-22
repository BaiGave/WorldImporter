cmake_minimum_required(VERSION 3.30.0)
project(WorldImporter VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(zlib REQUIRED)
find_package(libzip CONFIG REQUIRED)

file(GLOB SOURCE_FILES "WorldImporter/*.cpp")
add_executable(WorldImporter ${SOURCE_FILES})

target_link_libraries(WorldImporter PRIVATE libzip::zip ZLIB::ZLIB)
