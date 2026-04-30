cmake_minimum_required(VERSION 3.14)
project(molink LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 64-bit MinGW
set(CMAKE_C_COMPILER "D:/software/w64devkit/x64/bin/gcc.exe")
set(CMAKE_CXX_COMPILER "D:/software/w64devkit/x64/bin/g++.exe")

# 指定 compiler 工具链路径（as/ld 等），防止找到 x86 的版本
set(TOOLCHAIN_PREFIX "D:/software/w64devkit/x64/bin")
set(CMAKE_C_FLAGS "-B${TOOLCHAIN_PREFIX}")
set(CMAKE_CXX_FLAGS "-B${TOOLCHAIN_PREFIX}")

set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")

include_directories(${CMAKE_SOURCE_DIR}/vendor/libusb)
link_directories(${CMAKE_SOURCE_DIR}/libs)

add_executable(molink
    src/poc_main.cpp
    src/usb/usb_device.cpp
    src/adb/adb_transport.cpp
    src/adb/adb_rsa.cpp
)

target_link_libraries(molink libusb-1.0.dll.a ws2_32 bcrypt)

# 构建后复制 libusb DLL 到输出目录
add_custom_command(TARGET molink POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/libs/libusb-1.0.dll"
        "$<TARGET_FILE_DIR:molink>"
)
