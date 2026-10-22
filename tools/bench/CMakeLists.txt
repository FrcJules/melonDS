cmake_minimum_required(VERSION 3.16)

find_package(Threads REQUIRED)

add_executable(melonDS-bench
    Bench.cpp
    Platform.cpp
)

target_link_libraries(melonDS-bench PRIVATE core Threads::Threads)

# BENCH_NULLGL: the OpenGL renderer (OpenGL ES 3.0 code path, as in the WebAssembly build) on a null
# GL driver, to measure its CPU cost without a GPU. Needs -DENABLE_OGLRENDERER=ON -DENABLE_OGL_COMPUTE=OFF
# and the Khronos headers (GLES3/gl3.h, GLES2/gl2ext.h, KHR/khrplatform.h) in BENCH_GLES_INCLUDE.
option(BENCH_NULLGL "melonDS-bench: OpenGL renderer on a null GL driver (--gl)" OFF)
set(BENCH_GLES_INCLUDE "" CACHE PATH "Directory with GLES3/gl3.h, GLES2/gl2ext.h and KHR/khrplatform.h")
if (BENCH_NULLGL)
    if (NOT ENABLE_OGLRENDERER OR ENABLE_OGL_COMPUTE OR NOT BENCH_GLES_INCLUDE)
        message(FATAL_ERROR "BENCH_NULLGL needs ENABLE_OGLRENDERER=ON, ENABLE_OGL_COMPUTE=OFF and BENCH_GLES_INCLUDE")
    endif()
    target_include_directories(core PUBLIC ${BENCH_GLES_INCLUDE})
    target_compile_definitions(core PUBLIC "MELONDS_GL_HEADER=<GLES3/gl3.h>" MELONDS_GLES KHRONOS_STATIC)
    target_sources(melonDS-bench PRIVATE NullGL.cpp)
    target_compile_definitions(melonDS-bench PRIVATE BENCH_NULLGL)
endif()
