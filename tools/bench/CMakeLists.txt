cmake_minimum_required(VERSION 3.16)

find_package(Threads REQUIRED)

add_executable(melonDS-bench
    Bench.cpp
    Platform.cpp
)

target_link_libraries(melonDS-bench PRIVATE core Threads::Threads)

# --gl : renderer OpenGL natif sans fenêtre (EGL + Mesa). Nécessite ENABLE_OGLRENDERER (défaut) et libegl.
option(BENCH_GL "melonDS-bench: add --gl (headless OpenGL renderer through EGL)" OFF)
if (BENCH_GL)
    target_sources(melonDS-bench PRIVATE ${CMAKE_SOURCE_DIR}/src/frontend/glad/glad.c)
    target_include_directories(melonDS-bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_include_directories(core PRIVATE ${CMAKE_SOURCE_DIR}/src)   # glad.h is under src/frontend/glad
    target_compile_definitions(core PUBLIC MELONDS_GL_HEADER=\"frontend/glad/glad.h\")   # same definition as the Qt frontend
    target_compile_definitions(melonDS-bench PRIVATE BENCH_GL)
    target_link_libraries(melonDS-bench PRIVATE EGL ${CMAKE_DL_LIBS})
endif()
