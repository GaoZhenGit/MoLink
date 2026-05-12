cmake_minimum_required(VERSION 3.14)
project(molink LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 64-bit MinGW — 工具已加入 PATH，由 CMake 自动检测

set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++")

include_directories(${CMAKE_SOURCE_DIR}/third_party/libusb/include)
include_directories(${CMAKE_SOURCE_DIR}/src/utils)
link_directories(${CMAKE_SOURCE_DIR}/third_party/libusb/lib)

add_executable(molink
    src/main.cpp
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
