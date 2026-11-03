cmake_minimum_required(VERSION 3.22)
project(hsrprobe C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 不生成调试信息: 编译更快, .so 更小, 探针用不到
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g0")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g0")

# Dobby: 只编静态库, 不开 example/test
set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(Plugin.SymbolResolver OFF CACHE BOOL "" FORCE)
set(Plugin.ImportTableReplace OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/Dobby dobby_build)

add_library(hsrprobe SHARED src/vk_probe.cpp src/gles_probe.cpp)
target_compile_options(hsrprobe PRIVATE -O2 -fvisibility=hidden -Wall -Wno-unused-parameter)
target_link_libraries(hsrprobe PRIVATE dobby_static log android)
