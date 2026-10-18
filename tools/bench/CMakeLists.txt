cmake_minimum_required(VERSION 3.16)

find_package(Threads REQUIRED)

add_executable(melonDS-bench
    Bench.cpp
    Platform.cpp
)

target_link_libraries(melonDS-bench PRIVATE core Threads::Threads)
