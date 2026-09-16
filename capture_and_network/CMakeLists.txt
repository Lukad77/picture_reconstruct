cmake_minimum_required(VERSION 3.16)
project(picture_reconstruct_pipeline VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
enable_testing()

set(PROJECT_INCLUDE_DIRS
    ${PROJECT_SOURCE_DIR}/include/common
    ${PROJECT_SOURCE_DIR}/include/capture
    ${PROJECT_SOURCE_DIR}/include/network
    ${PROJECT_SOURCE_DIR}/include/protocol
    ${PROJECT_SOURCE_DIR}/include/app
    ${PROJECT_SOURCE_DIR}/include/storage
)

add_library(v2_transfer src/network/V2Transfer.cpp)
target_include_directories(v2_transfer PUBLIC
    ${PROJECT_SOURCE_DIR}/include/common
    ${PROJECT_SOURCE_DIR}/include/network
    ${PROJECT_SOURCE_DIR}/include/protocol
)
target_link_libraries(v2_transfer PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(v2_transfer PRIVATE /W4 /permissive-)
else()
    target_compile_options(v2_transfer PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(pipeline_config src/config/PipelineConfig.cpp)
target_include_directories(pipeline_config PUBLIC
    ${PROJECT_SOURCE_DIR}/include/config
    ${PROJECT_SOURCE_DIR}/third_party
)
target_link_libraries(pipeline_config PUBLIC v2_transfer)
if(MSVC)
    target_compile_options(pipeline_config PRIVATE /W4 /permissive-)
else()
    target_compile_options(pipeline_config PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(linux_pipeline_sender apps/client/linux_pipeline_sender.cpp)
target_include_directories(linux_pipeline_sender PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(linux_pipeline_sender PRIVATE v2_transfer pipeline_config ${OpenCV_LIBS})

add_executable(linux_reconstruct_receiver apps/server/linux_reconstruct_receiver.cpp)
target_include_directories(linux_reconstruct_receiver PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(linux_reconstruct_receiver PRIVATE v2_transfer pipeline_config ${OpenCV_LIBS})

add_executable(v2_receiver apps/server/v2_receiver.cpp)
target_link_libraries(v2_receiver PRIVATE v2_transfer)

add_executable(v2_sender apps/client/v2_sender.cpp)
target_link_libraries(v2_sender PRIVATE v2_transfer)

add_executable(test_v2_end_to_end tests/test_v2_end_to_end.cpp)
target_link_libraries(test_v2_end_to_end PRIVATE v2_transfer)
add_test(NAME v2_end_to_end COMMAND test_v2_end_to_end)
add_executable(test_v2_pipeline tests/test_v2_pipeline.cpp)
target_link_libraries(test_v2_pipeline PRIVATE v2_transfer)
add_test(NAME v2_pipeline COMMAND test_v2_pipeline)
add_executable(benchmark_v2_transfer tests/benchmark_v2_transfer.cpp)
target_link_libraries(benchmark_v2_transfer PRIVATE v2_transfer)

add_executable(test_pipeline_config tests/test_pipeline_config.cpp)
target_link_libraries(test_pipeline_config PRIVATE pipeline_config)
add_test(NAME pipeline_config COMMAND test_pipeline_config)

option(BUILD_LEGACY_COMPONENTS "Build the pre-V2 camera, protocol, and regression targets" OFF)
if(BUILD_LEGACY_COMPONENTS)
    add_library(lan_video_core
        src/common/FrameBufferPool.cpp
        src/capture/CameraCapture.cpp
        src/network/TcpClient.cpp
        src/network/RawFrameSender.cpp
        src/network/ResumeSession.cpp
        src/protocol/RawFrameProtocol.cpp
        src/storage/SenderSpool.cpp
        src/app/CameraNetworkSender.cpp
    )
    target_include_directories(lan_video_core PUBLIC ${PROJECT_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(lan_video_core PUBLIC Threads::Threads ${OpenCV_LIBS} v2_transfer)
    if(MSVC)
        target_compile_options(lan_video_core PRIVATE /W4 /permissive-)
    else()
        target_compile_options(lan_video_core PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(raw_camera_client apps/client/main_client.cpp)
    add_executable(raw_test_server apps/server/test_server.cpp)
    add_executable(test_camera tests/test_camera.cpp)
    add_executable(test_sender_spool_resume tests/test_sender_spool_resume.cpp)
    add_executable(test_reliable_resume tests/test_reliable_resume.cpp)
    add_executable(test_phase5_checkpoint tests/test_phase5_checkpoint.cpp)
    add_executable(test_processing_resume tests/test_processing_resume.cpp)
    foreach(target
        raw_camera_client raw_test_server test_camera test_sender_spool_resume
        test_reliable_resume test_phase5_checkpoint test_processing_resume)
        target_include_directories(${target} PRIVATE ${PROJECT_INCLUDE_DIRS})
        target_link_libraries(${target} PRIVATE lan_video_core)
    endforeach()
endif()
