cmake_minimum_required(VERSION 3.14)
project(shm_ipc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -pedantic)

# 可选 sanitizer 构建：-DSHM_IPC_SANITIZE=address,undefined 或 thread
set(SHM_IPC_SANITIZE "" CACHE STRING
    "Enable sanitizers (e.g. address,undefined or thread)")
if(SHM_IPC_SANITIZE)
    add_compile_options(-fsanitize=${SHM_IPC_SANITIZE} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${SHM_IPC_SANITIZE})
endif()

add_library(shm_ipc INTERFACE)
target_include_directories(shm_ipc INTERFACE include/)
# shm_server.hpp 内部持有握手线程（std::thread/mutex），接口依赖须传递给使用方
find_package(Threads REQUIRED)
target_link_libraries(shm_ipc INTERFACE Threads::Threads)

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

# 上层接口吞吐（ShmServer + ShmAsyncClient + SimplePodCodec）
add_executable(bench_acceptance bench/bench_acceptance.cpp)
target_link_libraries(bench_acceptance PRIVATE shm_ipc)

add_executable(bench_app bench/bench_app.cpp)
target_link_libraries(bench_app PRIVATE shm_ipc)

add_executable(test_audit_round3_fixes test/test_audit_round3_fixes.cpp)
target_link_libraries(test_audit_round3_fixes PRIVATE shm_ipc)
target_compile_options(test_audit_round3_fixes PRIVATE -UNDEBUG)
target_link_options(test_audit_round3_fixes PRIVATE -Wl,--wrap=listen)

add_executable(test_audit_round2_fixes test/test_audit_round2_fixes.cpp)
target_link_libraries(test_audit_round2_fixes PRIVATE shm_ipc)
target_compile_options(test_audit_round2_fixes PRIVATE -UNDEBUG)
target_link_options(test_audit_round2_fixes PRIVATE -Wl,--wrap=accept4)

add_executable(test_audit_core_fixes test/test_audit_core_fixes.cpp)
target_link_libraries(test_audit_core_fixes PRIVATE shm_ipc)
target_compile_options(test_audit_core_fixes PRIVATE -UNDEBUG)
target_link_options(test_audit_core_fixes PRIVATE -Wl,--wrap=write)

add_executable(test_audit_eof_reentry test/test_audit_eof_reentry.cpp)
target_link_libraries(test_audit_eof_reentry PRIVATE shm_ipc)
target_compile_options(test_audit_eof_reentry PRIVATE -UNDEBUG)
target_link_options(test_audit_eof_reentry PRIVATE -Wl,--wrap=poll)

add_executable(test_audit_hardening test/test_audit_hardening.cpp)
target_link_libraries(test_audit_hardening PRIVATE shm_ipc)
target_compile_options(test_audit_hardening PRIVATE -UNDEBUG)
target_link_options(test_audit_hardening PRIVATE -Wl,--wrap=mmap)

add_executable(test_audit_second test/test_audit_second.cpp)
target_link_libraries(test_audit_second PRIVATE shm_ipc)
target_compile_options(test_audit_second PRIVATE -UNDEBUG)

add_executable(test_audit_regression test/test_audit_regression.cpp)
target_link_libraries(test_audit_regression PRIVATE shm_ipc)
target_compile_options(test_audit_regression PRIVATE -UNDEBUG)
target_link_options(test_audit_regression PRIVATE -Wl,--wrap=poll)

add_executable(test_codec test/test_codec.cpp)
target_link_libraries(test_codec PRIVATE shm_ipc)

add_executable(test_ringbuf_wrap test/test_ringbuf_wrap.cpp)
target_link_libraries(test_ringbuf_wrap PRIVATE shm_ipc)

add_executable(test_ringbuf_model test/test_ringbuf_model.cpp)
target_link_libraries(test_ringbuf_model PRIVATE shm_ipc)

add_executable(test_ringbuf_process test/test_ringbuf_process.cpp)
target_link_libraries(test_ringbuf_process PRIVATE shm_ipc)

add_executable(test_eventloop test/test_eventloop.cpp)
target_link_libraries(test_eventloop PRIVATE shm_ipc)

add_executable(test_ringbuf_boundary test/test_ringbuf_boundary.cpp)
target_link_libraries(test_ringbuf_boundary PRIVATE shm_ipc)

# 测试使用 assert()，Release 构建的 -DNDEBUG 会将其全部编译掉，
# 必须显式恢复，否则测试形同虚设
foreach(t test_codec test_ringbuf_wrap test_eventloop test_ringbuf_boundary)
    target_compile_options(${t} PRIVATE -UNDEBUG)
endforeach()

add_executable(socket_server src/socket_server.cpp)
target_link_libraries(socket_server PRIVATE shm_ipc)

add_executable(socket_client src/socket_client.cpp)
target_link_libraries(socket_client PRIVATE shm_ipc)

# --- 上层应用接口示例与集成测试 ---
add_executable(app_server src/app_server.cpp)
target_link_libraries(app_server PRIVATE shm_ipc)

add_executable(app_client src/app_client.cpp)
target_link_libraries(app_client PRIVATE shm_ipc)

add_executable(test_shm_app test/test_shm_app.cpp)
target_link_libraries(test_shm_app PRIVATE shm_ipc pthread)
target_compile_options(test_shm_app PRIVATE -UNDEBUG)

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
    target_compile_options(test_proto_codec PRIVATE -UNDEBUG)

    add_executable(test_proto_wrap test/test_proto_wrap.cpp)
    target_link_libraries(test_proto_wrap PRIVATE shm_ipc shm_proto)
    target_compile_options(test_proto_wrap PRIVATE -UNDEBUG)

    target_link_libraries(client PRIVATE shm_proto)
    target_compile_definitions(client PRIVATE SHM_IPC_HAS_PROTOBUF)
    target_link_libraries(server PRIVATE shm_proto)
    target_compile_definitions(server PRIVATE SHM_IPC_HAS_PROTOBUF)
else()
    message(STATUS "Protobuf not found, skipping proto targets")
endif()

# Random-batch benchmark helper has no exception or allocation dependency.
add_executable(test_random_batch test/test_random_batch.cpp)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_random_batch PRIVATE -fno-exceptions)
endif()

# End-to-end four-process benchmark: local IPC / TCP / local IPC.
add_executable(bench_chain bench/bench_chain.cpp)
target_link_libraries(bench_chain PRIVATE shm_ipc)

add_executable(test_chain_wire test/test_chain_wire.cpp)
target_link_libraries(test_chain_wire PRIVATE shm_ipc)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_chain_wire PRIVATE -fno-exceptions)
endif()
add_executable(test_chain_stream test/test_chain_stream.cpp)
target_link_libraries(test_chain_stream PRIVATE shm_ipc)

if(Protobuf_FOUND)
    set(CHAIN_PROTO_CC ${CMAKE_CURRENT_BINARY_DIR}/chain_bench.pb.cc)
    set(CHAIN_PROTO_H ${CMAKE_CURRENT_BINARY_DIR}/chain_bench.pb.h)
    add_custom_command(OUTPUT ${CHAIN_PROTO_CC} ${CHAIN_PROTO_H}
        COMMAND protobuf::protoc --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
                --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
                ${CMAKE_CURRENT_SOURCE_DIR}/proto/chain_bench.proto
        DEPENDS proto/chain_bench.proto protobuf::protoc
        VERBATIM)
    add_library(chain_proto STATIC ${CHAIN_PROTO_CC})
    target_include_directories(chain_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(chain_proto PUBLIC protobuf::libprotobuf)
    add_executable(bench_chain_protobuf bench/bench_chain.cpp)
    target_compile_definitions(bench_chain_protobuf PRIVATE SHM_CHAIN_PROTOBUF=1)
    target_link_libraries(bench_chain_protobuf PRIVATE shm_ipc chain_proto)
    add_executable(test_chain_protobuf test/test_chain_protobuf.cpp)
    target_compile_definitions(test_chain_protobuf PRIVATE SHM_CHAIN_PROTOBUF=1)
    target_link_libraries(test_chain_protobuf PRIVATE shm_ipc chain_proto)
    target_compile_options(test_chain_protobuf PRIVATE -fno-exceptions)
endif()

add_executable(test_registration test/test_registration.cpp)
target_link_libraries(test_registration PRIVATE shm_ipc)
target_link_options(test_registration PRIVATE -Wl,--wrap=_ZnwmRKSt9nothrow_t)

add_executable(test_registration_app test/test_registration_app.cpp)
target_link_libraries(test_registration_app PRIVATE shm_ipc)
target_link_options(test_registration_app PRIVATE -Wl,--wrap=_ZnwmRKSt9nothrow_t)

target_compile_options(test_registration_app PRIVATE -UNDEBUG)
