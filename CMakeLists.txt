cmake_minimum_required(VERSION 3.16)
project(infiniflow_sender C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)

add_executable(sender
    src/core/fc_header.h
    src/core/flow.h
    src/core/vc_ring.h
    src/sender/sender_ctx.h
    src/sender/main.c
    src/sender/csv_loader.c
    src/sender/activity_manager.c
    src/sender/scheduler.c
    src/sender/forward.c
)

target_include_directories(sender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${DPDK_INCLUDE_DIRS}
)

target_compile_options(sender PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    ${DPDK_CFLAGS_OTHER}
)

target_link_directories(sender PRIVATE ${DPDK_LIBRARY_DIRS})
target_link_libraries(sender PRIVATE ${DPDK_LIBRARIES})

add_executable(receiver
    src/core/fc_header.h
    src/receiver/receiver_ctx.h
    src/receiver/main.c
)

target_include_directories(receiver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${DPDK_INCLUDE_DIRS}
)

target_compile_options(receiver PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    ${DPDK_CFLAGS_OTHER}
)

target_link_directories(receiver PRIVATE ${DPDK_LIBRARY_DIRS})
target_link_libraries(receiver PRIVATE ${DPDK_LIBRARIES})
