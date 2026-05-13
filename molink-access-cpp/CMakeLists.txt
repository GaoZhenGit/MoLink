cmake_minimum_required(VERSION 3.14)
project(molink LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 64-bit MinGW — 工具已加入 PATH，由 CMake 自动检测

set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")

include_directories(${CMAKE_SOURCE_DIR}/third_party/libusb/include)
include_directories(${CMAKE_SOURCE_DIR}/src/utils)
include_directories(${CMAKE_BINARY_DIR})  # for generated version.h
link_directories(${CMAKE_SOURCE_DIR}/third_party/libusb/lib)

# 每次构建生成 version_gen.h（使用构建时的时间戳）
set(VERSION_GEN_H "${CMAKE_BINARY_DIR}/version_gen.h")
add_custom_target(gen_version ALL
    ${CMAKE_COMMAND} -DOUTPUT_FILE=${VERSION_GEN_H} -P ${CMAKE_SOURCE_DIR}/version.cmake
    COMMENT "Generating version header..."
)

add_executable(molink
    src/main.cpp
    src/version.cpp
    src/usb/usb_device.cpp
    src/adb/adb_transport.cpp
    src/adb/adb_rsa.cpp
    src/adb/adb_reader.cpp
    src/adb/adb_client.cpp
    src/adb/adb_sync.cpp
    src/adb/adb_shell.cpp
    src/cli/named_pipe.cpp
    src/cli/cli_utils.cpp
    src/cli/cli_apush.cpp
    src/cli/cli_apull.cpp
    src/cli/cli_adel.cpp
    src/cli/cli_del.cpp
    src/cli/cli_devices.cpp
    src/cli/cli_forward.cpp
    src/cli/cli_auth.cpp
    src/daemon/daemon_app.cpp
    src/forward/forwarder.cpp
    src/transfer/file_push.cpp
    src/transfer/file_pull.cpp
    src/transfer/file_list.cpp
)

target_link_libraries(molink libusb-1.0.a ws2_32 bcrypt crypt32 winusb setupapi ole32)
add_dependencies(molink gen_version)
