cmake_minimum_required(VERSION 3.16)

project(lan_video_transfer
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 导出 compile_commands.json，方便 clangd / VS Code / Qt Creator 使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -----------------------------
# Dependencies
# -----------------------------

find_package(Threads REQUIRED)

# 如果你的 CameraCapture 使用 OpenCV，则启用下面两行
find_package(OpenCV REQUIRED)

# -----------------------------
# Core library
# -----------------------------

add_library(lan_video_core
    src/common/FrameBufferPool.cpp
    src/capture/CameraCapture.cpp
    src/network/TcpClient.cpp
    src/network/RawFrameSender.cpp
    src/protocol/RawFrameProtocol.cpp
    src/app/CameraNetworkSender.cpp
)
set(LAN_VIDEO_INCLUDE_DIRS
    ${PROJECT_SOURCE_DIR}/include/common
    ${PROJECT_SOURCE_DIR}/include/capture
    ${PROJECT_SOURCE_DIR}/include/network
    ${PROJECT_SOURCE_DIR}/include/protocol
    ${PROJECT_SOURCE_DIR}/include/app
)

target_include_directories(lan_video_core
    PUBLIC
        ${LAN_VIDEO_INCLUDE_DIRS}
)


target_link_libraries(lan_video_core
    PUBLIC
        Threads::Threads
        ${OpenCV_LIBS}
)

target_compile_options(lan_video_core
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

# -----------------------------
# Client executable
# -----------------------------

add_executable(raw_camera_client
    apps/client/main_client.cpp
)

target_include_directories(raw_camera_client
    PRIVATE
        ${LAN_VIDEO_INCLUDE_DIRS}
)

target_link_libraries(raw_camera_client
    PRIVATE
        lan_video_core
)

# -----------------------------
# Test server executable
# -----------------------------

add_executable(raw_test_server
    apps/server/test_server.cpp
)

target_include_directories(raw_test_server
    PRIVATE
        ${LAN_VIDEO_INCLUDE_DIRS}
)

target_link_libraries(raw_test_server
    PRIVATE
        lan_video_core
)

# -----------------------------
# Camera test executable
# -----------------------------

add_executable(test_camera
    tests/test_camera.cpp
)

target_include_directories(test_camera
    PRIVATE
        ${LAN_VIDEO_INCLUDE_DIRS}
)

target_link_libraries(test_camera
    PRIVATE
        lan_video_core
)