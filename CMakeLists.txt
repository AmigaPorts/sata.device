# SPDX-License-Identifier: MPL-2.0
cmake_minimum_required(VERSION 3.14.0)
project(sata.device LANGUAGES C VERSION 1.0)

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(CPM-init)

get_verstring(VERSTRING)
option(SATA_DEBUG "Enable Exec raw serial diagnostics (requires a configured serial backend)" OFF)

if(NOT TARGET pcie)
    CPMGetPackage(pcie.library)
endif()

# Set default install prefix to ./install subdirectory
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH "Install path" FORCE)
endif()

add_library(sata_transport OBJECT
    src/ata.c
    src/ahci.c
)

target_include_directories(sata_transport PRIVATE include/private)
target_link_libraries(sata_transport PRIVATE pcie)
target_compile_options(sata_transport PRIVATE -Os -m68040 -fomit-frame-pointer
    -ffreestanding -fno-builtin -fno-common -Wall -Wextra -Werror)

add_executable(sata.device
    src/main.c
    src/init.c
    src/beginio.c
    src/io.c
    src/scsi.c
    src/unit_task.c
    src/partition.c
    src/container.c
    src/rdb.c
    src/hunk.c
    src/end.c
)

if(SATA_DEBUG)
    target_compile_definitions(sata.device PRIVATE SATA_DEBUG)
endif()

target_include_directories(sata.device PRIVATE include/private)
target_compile_definitions(sata.device PRIVATE
    VERSION_STRING="${VERSTRING}"
    LIBRARY_VERSION=${PROJECT_VERSION_MAJOR}
    LIBRARY_REVISION=${PROJECT_VERSION_MINOR}
    LIBRARY_NAME="${PROJECT_NAME}"
)
set_property(TARGET sata.device APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ldscript.lds")
# The toolchain provides a 68020 libgcc multilib, also usable by 68040 code.
# Selecting -m68040 at link time falls back to its default 68000 runtime.
target_link_options(sata.device PRIVATE -m68020 -nostdlib -nostartfiles -Wall -Wextra -s
    "-Wl,-T${CMAKE_CURRENT_SOURCE_DIR}/ldscript.lds"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/sata.device.map")
target_compile_options(sata.device PRIVATE -O2 -m68040 -fomit-frame-pointer
    -ffreestanding -fno-builtin -fno-common -Wall -Wextra -Werror -DDEBUG)
set_source_files_properties(src/beginio.c PROPERTIES COMPILE_OPTIONS "-fcall-saved-d1;-fcall-saved-a0;-fcall-saved-a1")

target_link_libraries(sata.device PRIVATE sata_transport pcie gcc m)
check_rom(sata.device sata.device)
bin_to_header(sata.device)

# Set up information for Aminet
add_project_info(
    "sata.device for AmigaOS 3.x"
    "marlon@amigadev.com (Marlon Beijer)"
    "driver/other"
    "m68k-amigaos"
)

add_aminet_readme_generator(sata.device "sata.device.readme")

install(FILES LICENSE DESTINATION Developer)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/sata.device DESTINATION Devs)
