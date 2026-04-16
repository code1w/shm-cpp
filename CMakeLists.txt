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
