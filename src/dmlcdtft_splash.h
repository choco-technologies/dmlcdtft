#ifndef DMLCDTFT_SPLASH_H
#define DMLCDTFT_SPLASH_H

#include "dmlcdtft_types.h"

/**
 * @brief Framebuffer the splash logo is drawn into.
 */
typedef struct
{
    uint8_t                    *buffer;         /**< Start of the framebuffer */
    uint32_t                    stride;         /**< Bytes of one line */
    uint16_t                    width;          /**< Pixels per line */
    uint16_t                    height;         /**< Lines */
    dmdrvi_gfx_pixel_format_t   pixel_format;   /**< Format of the framebuffer */
    uint32_t                    background;     /**< 0xRRGGBB the logo is blended over */
} dmlcdtft_splash_target_t;

/**
 * @brief Draws the logo of a .dmvir file in the middle of the framebuffer.
 *
 * A .dmvir is an uncompressed .dmvi (dmview's image format): the 56-byte
 * header and the raw pixels - RGB565, RGB565A8 or ARGB8888. Transparent
 * pixels are blended over target->background, which the framebuffer is
 * expected to be filled with already.
 *
 * @return 0 on success, -ENOENT when the file does not exist, another
 *         negative errno when it is not a logo this driver can draw
 */
int dmlcdtft_splash_draw(const char *path, const dmlcdtft_splash_target_t *target);

#endif /* DMLCDTFT_SPLASH_H */
