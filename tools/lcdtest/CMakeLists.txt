# =====================================================================
#               lcdtest - Manual dmlcdtft Device Test Tool
# =====================================================================
# Added via add_subdirectory() from the top-level CMakeLists.txt, so it
# shares dmlcdtft's own dmod fetch/configuration instead of setting up a
# second, separate DMOD project - same pattern as dmspi/tools/spitest.

# Path to the DMR file for this module - produces a ready-to-publish
# release package under build/packages/lcdtest alongside dmlcdtft.
set(DMOD_DMR_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lcdtest.dmr)

dmod_add_executable(lcdtest ${DMOD_MODULE_VERSION}
    main.c
)

# The device is driven through Dmod_FileOpen/_Ioctl/_FileWrite/_FileRead
# (dmod SAL). dmlcdtft_if: the ioctl definitions and dmlcdtft_color_to_pixel().
# dmosi: tick count for the refresh rate measurement.
dmod_link_modules(lcdtest
    dmosi
    dmdrvi@>=2.2
)

target_link_libraries(lcdtest dmlcdtft_if)
