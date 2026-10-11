cmake_minimum_required(VERSION 3.21)
project(mikeDAW-Bridge32 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPENDAW_VST2_SDK_PATH "C:/GitHuhb/vst_sdk2_4_rev2/vstsdk2.4"
    CACHE PATH "Steinberg VST 2.4 SDK root (contains pluginterfaces/vst2.x)")

add_executable(mikeDAW-Bridge32 WIN32 main.cpp)

target_include_directories(mikeDAW-Bridge32 PRIVATE
    "${OPENDAW_VST2_SDK_PATH}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Source/Engine")

target_compile_definitions(mikeDAW-Bridge32 PRIVATE VST_FORCE_DEPRECATED=0)

target_link_libraries(mikeDAW-Bridge32 PRIVATE user32 gdi32)

target_link_options(mikeDAW-Bridge32 PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup)
