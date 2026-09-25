cmake_minimum_required(VERSION 3.20.0)

# Provides the SSD1306 OLED (with the exact init parameters this specific
# panel needs - segment-remap/com-invdir/multiplex-ratio etc, easy to get
# wrong by hand) and the D1 button as a proper gpio-keys node. Set here
# (before find_package) so plain `west build` picks it up with no extra
# flag needed, same reasoning as the self-contained CDC-ACM console setup.
# Does not cover the buzzer - that pinctrl/PWM setup stays in
# boards/xiao_rp2040.overlay, since the shield doesn't know about it.
set(SHIELD seeed_xiao_expansion_board)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(xiao_rp2040_mplay)

target_sources(app PRIVATE src/main.c src/mod_player.c src/vu_neopixel.c src/oled_display.c src/spectrum.c)

# Embed the MOD file as flash data (mod_data[] / mod_data_len), generated at
# build time into the build directory rather than committing a ~6x larger
# generated header - same approach as pico_mplay. Swap songs by changing
# MOD_SOURCE_FILE.
set(MOD_SOURCE_FILE ${CMAKE_CURRENT_LIST_DIR}/AXEL_F.MOD)
set(MOD_DATA_HEADER ${CMAKE_CURRENT_BINARY_DIR}/mod_data.h)
add_custom_command(
        OUTPUT ${MOD_DATA_HEADER}
        COMMAND ${CMAKE_COMMAND} -E copy ${MOD_SOURCE_FILE} mod_data
        COMMAND sh -c "xxd -i mod_data | sed \
                -e 's/^unsigned char mod_data\\[\\]/static const unsigned char mod_data[]/' \
                -e 's/^unsigned int mod_data_len/static const unsigned int mod_data_len/' \
                > mod_data.h"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS ${MOD_SOURCE_FILE}
        VERBATIM
        COMMENT "Generating mod_data.h from ${MOD_SOURCE_FILE}"
)
add_custom_target(mod_data_header DEPENDS ${MOD_DATA_HEADER})
add_dependencies(app mod_data_header)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
