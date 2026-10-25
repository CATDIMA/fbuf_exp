cmake_minimum_required(VERSION 3.15.0)
project(fbuf)
set(CMAKE_BUILD_TYPE Debug)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)

add_executable(${PROJECT_NAME}  src/main.cpp 
								src/drm_output.cpp)
target_link_libraries(${PROJECT_NAME} PkgConfig::LIBDRM)
target_include_directories(${PROJECT_NAME} PRIVATE inc)
