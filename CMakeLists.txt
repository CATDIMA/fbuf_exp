cmake_minimum_required(VERSION 3.15.0)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

project(fbuf)
set(CMAKE_BUILD_TYPE Debug)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)

add_executable(${PROJECT_NAME}  src/main.cpp 
								src/drm_output.cpp
								src/libbmp.cpp)
target_link_libraries(${PROJECT_NAME} PkgConfig::LIBDRM)
target_include_directories(${PROJECT_NAME} PRIVATE inc)

#target_compile_options(${PROJECT_NAME} PRIVATE
#						-Wall
#						-Wextra
#						-Werror
#						-Wunused-private-field)
