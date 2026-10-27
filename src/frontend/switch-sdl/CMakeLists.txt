cmake_minimum_required(VERSION 3.16)

find_package(Threads REQUIRED)
find_package(SDL2 REQUIRED)

add_executable(melonDS-switch
    main.cpp
    Platform.cpp
)

target_include_directories(melonDS-switch PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(melonDS-switch PRIVATE core Threads::Threads ${SDL2_LIBRARIES} nx)

# .nro (needs devkitPro: nacptool, elf2nro)
find_program(NACPTOOL nacptool HINTS $ENV{DEVKITPRO}/tools/bin)
find_program(ELF2NRO elf2nro HINTS $ENV{DEVKITPRO}/tools/bin)
if (NACPTOOL AND ELF2NRO)
    add_custom_command(TARGET melonDS-switch POST_BUILD
        COMMAND ${NACPTOOL} --create "melonDS fork" "FrcJules" "0.1" melonDS-switch.nacp
        COMMAND ${ELF2NRO} $<TARGET_FILE:melonDS-switch> melonDS-switch.nro --nacp=melonDS-switch.nacp
        COMMENT "Creating melonDS-switch.nro")
endif()
