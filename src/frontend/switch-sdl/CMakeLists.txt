cmake_minimum_required(VERSION 3.16)

find_package(Threads REQUIRED)
find_package(SDL2 REQUIRED)

set(SWITCH_APP_NAME "melonDS fork" CACHE STRING "Title shown in hbmenu")

add_executable(melonDS-switch
    main.cpp
    Platform.cpp
)

if (ENABLE_OGLRENDERER)
    # GLES 3 renderer (Mesa/nouveau on the console): same definitions as the WebGL build
    target_compile_definitions(core PUBLIC "MELONDS_GL_HEADER=<GLES3/gl3.h>" MELONDS_GLES MELONDS_GL_PROFILE)
    target_include_directories(core PUBLIC $ENV{DEVKITPRO}/portlibs/switch/include)
endif()

target_include_directories(melonDS-switch PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(melonDS-switch PRIVATE core Threads::Threads SDL2_ttf freetype harfbuzz png16 bz2 z ${SDL2_LIBRARIES} EGL GLESv2 glapi drm_nouveau nx)

# .nro (needs devkitPro: nacptool, elf2nro)
find_program(NACPTOOL nacptool HINTS $ENV{DEVKITPRO}/tools/bin)
find_program(ELF2NRO elf2nro HINTS $ENV{DEVKITPRO}/tools/bin)
if (NACPTOOL AND ELF2NRO)
    add_custom_command(TARGET melonDS-switch POST_BUILD
        COMMAND ${NACPTOOL} --create "${SWITCH_APP_NAME}" "FrcJules" "0.1" melonDS-switch.nacp
        COMMAND ${ELF2NRO} $<TARGET_FILE:melonDS-switch> melonDS-switch.nro --nacp=melonDS-switch.nacp
        COMMENT "Creating melonDS-switch.nro")
endif()
