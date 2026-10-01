#ifndef DMLCDTFT_H
#define DMLCDTFT_H

#include "dmlcdtft_defs.h"
#include "dmlcdtft_types.h"

/**
 * @brief Validate a configuration structure without touching hardware.
 *
 * Checks that the combination of parameters is self-consistent (non-zero
 * resolution and pixel clock, known pixel format, timing within the limits
 * of the controller registers). Does not require dmlcdtft_port to be loaded.
 *
 * @return true if the configuration is valid, false otherwise.
 */
dmod_dmlcdtft_api(1.0, bool, _validate_config, ( const dmlcdtft_config_t *config ));

/**
 * @brief Number of bytes one pixel takes in the given format.
 *
 * @return 2, 3 or 4, or 0 for an unknown format.
 */
dmod_dmlcdtft_api(1.0, uint8_t, _bytes_per_pixel, ( dmlcdtft_pixel_format_t format ));

/**
 * @brief Convert a 0xAARRGGBB color to the raw pixel value of a format.
 *
 * The result occupies the low dmlcdtft_bytes_per_pixel(format) bytes and is
 * stored in memory little-endian, the way the controller reads it.
 */
dmod_dmlcdtft_api(1.0, uint32_t, _color_to_pixel, ( dmlcdtft_pixel_format_t format, uint32_t argb ));

#endif // DMLCDTFT_H
