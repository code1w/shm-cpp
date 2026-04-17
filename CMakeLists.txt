cmake_minimum_required(VERSION 3.14)
project(shm_ipc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -pedantic)

add_library(shm_ipc INTERFACE)
target_include_directories(shm_ipc INTERFACE include/)

add_executable(client src/client.cpp)
target_link_libraries(client PRIVATE shm_ipc)

add_executable(server src/server.cpp)
target_link_libraries(server PRIVATE shm_ipc)

add_executable(bench_socket bench/bench_socket.cpp)
target_link_libraries(bench_socket PRIVATE shm_ipc)

add_executable(bench_shm bench/bench_shm.cpp)
target_link_libraries(bench_shm PRIVATE shm_ipc)

add_executable(bench_batch bench/bench_batch.cpp)
target_link_libraries(bench_batch PRIVATE shm_ipc)

add_executable(bench_codec bench/bench_codec.cpp)
target_link_libraries(bench_codec PRIVATE shm_ipc)

add_executable(test_codec test/test_codec.cpp)
target_link_libraries(test_codec PRIVATE shm_ipc)

add_executable(test_ringbuf_wrap test/test_ringbuf_wrap.cpp)
target_link_libraries(test_ringbuf_wrap PRIVATE shm_ipc)

add_executable(socket_server src/socket_server.cpp)
target_link_libraries(socket_server PRIVATE shm_ipc)

add_executable(socket_client src/socket_client.cpp)
target_link_libraries(socket_client PRIVATE shm_ipc)

# --- Protobuf（本地安装到 3rd/protobuf） ---
set(PROTOBUF_ROOT ${CMAKE_SOURCE_DIR}/3rd/protobuf)
list(APPEND CMAKE_PREFIX_PATH ${PROTOBUF_ROOT})
# protobuf 3.21 在 CentOS 上安装到 lib64
list(APPEND CMAKE_PREFIX_PATH ${PROTOBUF_ROOT}/lib64/cmake/protobuf)
find_package(Protobuf QUIET)

if(Protobuf_FOUND)
    message(STATUS "Protobuf found: ${Protobuf_VERSION}")

    add_library(shm_proto proto/shm_messages.pb.cc)
    target_include_directories(shm_proto PUBLIC
        ${CMAKE_SOURCE_DIR}/proto
        ${Protobuf_INCLUDE_DIRS})
    target_link_libraries(shm_proto PUBLIC protobuf::libprotobuf)

    add_executable(test_proto_codec test/test_proto_codec.cpp)
    target_link_libraries(test_proto_codec PRIVATE shm_ipc shm_proto)

    target_link_libraries(client PRIVATE shm_proto)
    target_compile_definitions(client PRIVATE SHM_IPC_HAS_PROTOBUF)
    target_link_libraries(server PRIVATE shm_proto)
    target_compile_definitions(server PRIVATE SHM_IPC_HAS_PROTOBUF)
else()
    message(STATUS "Protobuf not found, skipping proto targets")
endif()
