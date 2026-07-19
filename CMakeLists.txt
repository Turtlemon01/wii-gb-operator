cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{DEVKITPRO})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/Wii.cmake" CACHE PATH "toolchain file")
    else()
        message(FATAL_ERROR "DEVKITPRO environment variable is not set")
    endif()
endif()

project(wii_gb_operator C)

# ---------------------------------------------------------------------------
# mGBA static library — compiled from vendored source, no mGBA CMake system
# ---------------------------------------------------------------------------
set(MGBA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/mgba")

set(MGBA_SRC
    # version stub (replaces CMake-generated version.c)
    ${MGBA_DIR}/wii-version.c

    # ARM CPU
    ${MGBA_DIR}/src/arm/arm.c
    ${MGBA_DIR}/src/arm/decoder.c
    ${MGBA_DIR}/src/arm/decoder-arm.c
    ${MGBA_DIR}/src/arm/decoder-thumb.c
    ${MGBA_DIR}/src/arm/isa-arm.c
    ${MGBA_DIR}/src/arm/isa-thumb.c

    # SM83 CPU (GB/GBC)
    ${MGBA_DIR}/src/sm83/sm83.c
    ${MGBA_DIR}/src/sm83/decoder.c
    ${MGBA_DIR}/src/sm83/isa-sm83.c

    # Core
    ${MGBA_DIR}/src/core/bitmap-cache.c
    ${MGBA_DIR}/src/core/cache-set.c
    ${MGBA_DIR}/src/core/cheats.c
    ${MGBA_DIR}/src/core/config.c
    ${MGBA_DIR}/src/core/core.c
    ${MGBA_DIR}/src/core/directories.c
    ${MGBA_DIR}/src/core/input.c
    ${MGBA_DIR}/src/core/interface.c
    ${MGBA_DIR}/src/core/library.c
    ${MGBA_DIR}/src/core/lockstep.c
    ${MGBA_DIR}/src/core/log.c
    ${MGBA_DIR}/src/core/map-cache.c
    ${MGBA_DIR}/src/core/mem-search.c
    ${MGBA_DIR}/src/core/rewind.c
    ${MGBA_DIR}/src/core/serialize.c
    ${MGBA_DIR}/src/core/sync.c
    ${MGBA_DIR}/src/core/thread.c
    ${MGBA_DIR}/src/core/tile-cache.c
    ${MGBA_DIR}/src/core/timing.c

    # GB/GBC audio (shared module — included once even though both GB and GBA use it)
    ${MGBA_DIR}/src/gb/audio.c
    # GB/GBC board
    ${MGBA_DIR}/src/gb/cheats.c
    ${MGBA_DIR}/src/gb/core.c
    ${MGBA_DIR}/src/gb/gb.c
    ${MGBA_DIR}/src/gb/input.c
    ${MGBA_DIR}/src/gb/io.c
    ${MGBA_DIR}/src/gb/mbc.c
    ${MGBA_DIR}/src/gb/memory.c
    ${MGBA_DIR}/src/gb/overrides.c
    ${MGBA_DIR}/src/gb/serialize.c
    ${MGBA_DIR}/src/gb/renderers/cache-set.c
    ${MGBA_DIR}/src/gb/renderers/software.c
    ${MGBA_DIR}/src/gb/sio.c
    ${MGBA_DIR}/src/gb/timer.c
    ${MGBA_DIR}/src/gb/video.c

    # GBA board (audio.c here is GBA-specific DMA/mixer; PSG is from gb/audio.c above)
    ${MGBA_DIR}/src/gba/audio.c
    ${MGBA_DIR}/src/gba/bios.c
    ${MGBA_DIR}/src/gba/cart/ereader.c
    ${MGBA_DIR}/src/gba/cart/gpio.c
    ${MGBA_DIR}/src/gba/cart/matrix.c
    ${MGBA_DIR}/src/gba/cart/vfame.c
    ${MGBA_DIR}/src/gba/cheats.c
    ${MGBA_DIR}/src/gba/cheats/codebreaker.c
    ${MGBA_DIR}/src/gba/cheats/gameshark.c
    ${MGBA_DIR}/src/gba/cheats/parv3.c
    ${MGBA_DIR}/src/gba/core.c
    ${MGBA_DIR}/src/gba/dma.c
    ${MGBA_DIR}/src/gba/gba.c
    ${MGBA_DIR}/src/gba/hle-bios.c
    ${MGBA_DIR}/src/gba/input.c
    ${MGBA_DIR}/src/gba/io.c
    ${MGBA_DIR}/src/gba/memory.c
    ${MGBA_DIR}/src/gba/overrides.c
    ${MGBA_DIR}/src/gba/renderers/cache-set.c
    ${MGBA_DIR}/src/gba/renderers/common.c
    ${MGBA_DIR}/src/gba/renderers/gl.c
    ${MGBA_DIR}/src/gba/renderers/software-bg.c
    ${MGBA_DIR}/src/gba/renderers/software-mode0.c
    ${MGBA_DIR}/src/gba/renderers/software-obj.c
    ${MGBA_DIR}/src/gba/renderers/video-software.c
    ${MGBA_DIR}/src/gba/savedata.c
    ${MGBA_DIR}/src/gba/serialize.c
    ${MGBA_DIR}/src/gba/sharkport.c
    ${MGBA_DIR}/src/gba/sio.c
    ${MGBA_DIR}/src/gba/sio/gbp.c
    ${MGBA_DIR}/src/gba/sio/joybus.c
    ${MGBA_DIR}/src/gba/timer.c
    ${MGBA_DIR}/src/gba/video.c

    # Utilities
    ${MGBA_DIR}/src/util/circle-buffer.c
    ${MGBA_DIR}/src/util/configuration.c
    ${MGBA_DIR}/src/util/crc32.c
    ${MGBA_DIR}/src/util/formatting.c
    ${MGBA_DIR}/src/util/gbk-table.c
    ${MGBA_DIR}/src/util/hash.c
    ${MGBA_DIR}/src/util/memory.c
    ${MGBA_DIR}/src/util/patch.c
    ${MGBA_DIR}/src/util/patch-fast.c
    ${MGBA_DIR}/src/util/patch-ips.c
    ${MGBA_DIR}/src/util/patch-ups.c
    ${MGBA_DIR}/src/util/ring-fifo.c
    ${MGBA_DIR}/src/util/string.c
    ${MGBA_DIR}/src/util/table.c
    ${MGBA_DIR}/src/util/text-codec.c
    ${MGBA_DIR}/src/util/vfs.c
    # VFS: memory-mapped (for ROM/save buffers) and FIFO
    ${MGBA_DIR}/src/util/vfs/vfs-mem.c
    ${MGBA_DIR}/src/util/vfs/vfs-fifo.c
    # VFS: file-descriptor based (for loading files from SD)
    ${MGBA_DIR}/src/util/vfs/vfs-fd.c
    ${MGBA_DIR}/src/util/vfs/vfs-dirent.c

    # Third-party
    ${MGBA_DIR}/src/third-party/blip_buf/blip_buf.c
    ${MGBA_DIR}/src/third-party/inih/ini.c
)

add_library(mgba STATIC ${MGBA_SRC})

target_include_directories(mgba PUBLIC
    ${MGBA_DIR}/include
    ${MGBA_DIR}/src
    "$ENV{DEVKITPRO}/libogc/include"
)

# Wii-specific compile defines for mGBA (from vendor/mgba/src/platform/wii/CMakeLists.txt)
target_compile_definitions(mgba PRIVATE
    GEKKO
    COLOR_16_BIT
    COLOR_5_6_5
    IOAPI_NO_64
    _GNU_SOURCE
    M_CORE_GBA
    M_CORE_GB
    BUILD_STATIC
    DISABLE_THREADING
    # MINIMAL_CORE=1: removes inputMap from struct mCore and skips mVideoLogCoreFind
    # (video-logger is not compiled); dirs is still included (MINIMAL_CORE < 2)
    MINIMAL_CORE=1
    # devkitPPC newlib already defines locale_t and strtof_l — suppress mGBA's fallback stubs
    HAVE_LOCALE
    HAVE_STRTOF_L
    HAVE_STRDUP
    HAVE_STRNDUP
    HAVE_LOCALTIME_R
)

# Suppress warnings from vendored code that we can't fix
target_compile_options(mgba PRIVATE
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -Wno-sign-compare
    -Wno-unused-function
    -Wno-implicit-fallthrough
)

# ---------------------------------------------------------------------------
# Main application
# ---------------------------------------------------------------------------
add_executable(wii_gb_operator
    source/main.c
    source/gb_operator.c
    source/rom_cache.c
    source/mgba_frontend.c
    source/cart_sync.c
    source/dol_reload.c
    source/settings.c
    source/cartindex.c
    source/gametitles.c
)

target_include_directories(wii_gb_operator PRIVATE
    "$ENV{DEVKITPRO}/libogc/include"
    source
    ${MGBA_DIR}/include
    ${MGBA_DIR}/src
)

target_compile_definitions(wii_gb_operator PRIVATE
    COLOR_16_BIT
    COLOR_5_6_5
    M_CORE_GBA
    M_CORE_GB
    MINIMAL_CORE=1
)

target_link_directories(wii_gb_operator BEFORE PRIVATE
    "$ENV{DEVKITPRO}/libogc/lib/wii"
)

# fat must come before ogc (dependency order); mgba before both; aesnd for audio
target_link_libraries(wii_gb_operator PRIVATE
    mgba
    fat
    aesnd
    ogc
)

# Convert ELF -> DOL for loading via Homebrew Channel
add_custom_command(TARGET wii_gb_operator POST_BUILD
    COMMAND "$ENV{DEVKITPRO}/tools/bin/elf2dol"
            $<TARGET_FILE:wii_gb_operator>
            "${CMAKE_CURRENT_BINARY_DIR}/wii_gb_operator.dol"
    COMMENT "Converting ELF to DOL"
)
