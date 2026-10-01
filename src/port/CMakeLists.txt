# ======================================================================
#               dmlcdtft_port Module Configuration
# ======================================================================

# Name of the module
set(DMOD_MODULE_NAME        dmlcdtft_port)

# Version is already set above and used in project()

# Author (should be string)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")

# Stack size for the module (should be integer)
set(DMOD_STACK_SIZE         1024)

# Path to the DMR file for this module
set(DMOD_DMR_PATH           ${CMAKE_SOURCE_DIR}/dmlcdtft_port.dmr)

# Every STM32 family with an LTDC shares the same register layout and clock
# path (see src/port/stm32_common), so a stm32 family port only adds its
# thin port.c on top of the shared implementation.
set(COMMON_SOURCES "")
if(DMOD_CPU_FAMILY MATCHES "^stm32")
    set(COMMON_SOURCES stm32_common/stm32_common.c)
endif()

#
#   dmod_add_library - create a library module
#   it has the same signature as add_library
#   and can be used in the same way after the creation
#   (for example, to link libraries)
#
dmod_add_library(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    # List of source files - can include C and C++ files
    ${DMOD_CPU_FAMILY}/port.c
    ${COMMON_SOURCES}
)

# dmdrvi: headers only - dmlcdtft_types.h numbers the ioctl commands from
#         DMDRVI_IOCTL_CUSTOM_BASE.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmdrvi@>=2.0
)

# dmclk_port: SYSCLK, to derive the PLL input clock PLLSAI shares with the
#             main PLL when it runs from HSE.
# dmosi:      vsync/reload semaphore posted from the LTDC ISR, tick count for
#             timeouts, minimum interrupt priority.
if(DMOD_CPU_FAMILY MATCHES "^stm32")
    dmod_link_modules(${DMOD_MODULE_NAME}
        dmclk_port
        dmosi
    )
endif()

target_include_directories(${DMOD_MODULE_NAME} BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(${DMOD_MODULE_NAME}_if INTERFACE
    ${CMAKE_SOURCE_DIR}/include
)
