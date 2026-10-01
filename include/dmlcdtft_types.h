#ifndef DMLCDTFT_TYPES_H
#define DMLCDTFT_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmdrvi_ioctl.h"

/**
 * @brief LCD-TFT controller instance (0-based - every STM32F4/F7 part with an
 *        LTDC has exactly one, see dmlcdtft_port_get_instance_count()).
 */
typedef uint8_t dmlcdtft_instance_t;

/**
 * @brief Active level of a synchronization signal.
 */
typedef enum
{
    dmlcdtft_polarity_active_low = 0,
    dmlcdtft_polarity_active_high,
} dmlcdtft_polarity_t;

/**
 * @brief Panel timing, in pixel clock cycles (horizontal) and lines (vertical).
 *
 * These are the plain widths from the panel datasheet - the port converts
 * them into whatever accumulated form its controller needs.
 */
typedef struct
{
    uint32_t            pixel_clock_hz;     /**< Requested pixel clock */
    uint16_t            hsync_width;        /**< HSYNC pulse width */
    uint16_t            hback_porch;        /**< Horizontal back porch */
    uint16_t            hfront_porch;       /**< Horizontal front porch */
    uint16_t            vsync_width;        /**< VSYNC pulse width */
    uint16_t            vback_porch;        /**< Vertical back porch */
    uint16_t            vfront_porch;       /**< Vertical front porch */
    dmlcdtft_polarity_t hsync_polarity;     /**< HSYNC active level */
    dmlcdtft_polarity_t vsync_polarity;     /**< VSYNC active level */
    dmlcdtft_polarity_t de_polarity;        /**< Data enable active level */
    bool                pclk_inverted;      /**< Sample data on the falling edge of the pixel clock */
} dmlcdtft_timing_t;

/**
 * @brief Driver configuration.
 *
 * Kept here rather than in dmlcdtft.h because dmlcdtft_port_init() takes the
 * whole structure, and the port module is built without access to the core
 * module's generated dmlcdtft_defs.h (same reasoning as dmeth_config_t).
 */
typedef struct
{
    dmlcdtft_instance_t     instance;           /**< Controller instance */
    uint16_t                width;              /**< Active width in pixels */
    uint16_t                height;             /**< Active height in lines */
    dmdrvi_gfx_pixel_format_t pixel_format;       /**< Framebuffer pixel format */
    dmlcdtft_timing_t       timing;             /**< Panel timing */
    uint32_t                background_color;   /**< 0xRRGGBB shown where the layer is transparent */
    uint8_t                 alpha;              /**< Constant alpha of the framebuffer layer */
    bool                    double_buffer;      /**< Allocate a second buffer for tear-free swaps */
    uint32_t                clear_color;        /**< 0xAARRGGBB the framebuffer is filled with at start */
} dmlcdtft_config_t;

/**
 * @brief Controller status returned by dmlcdtft_ioctl_cmd_get_status.
 *
 * Resolution, pixel format, stride and the drawing buffer are reported by the
 * generic DMDRVI_IOCTL_GFX_* commands (see dmdrvi_ioctl.h); only what is
 * specific to this controller is here.
 */
typedef struct
{
    uint32_t                pixel_clock_hz;     /**< Pixel clock actually programmed */
    uint32_t                underrun_count;     /**< FIFO underruns since the driver started */
} dmlcdtft_status_t;

/**
 * @brief Private IOCTL commands of the dmlcdtft device.
 *
 * Reading/writing the device node reads/writes the drawing buffer at the given
 * byte offset, like a Linux framebuffer device. Everything a generic graphics
 * client needs (info, framebuffer, swap, vsync, fill, display, backlight) is a
 * standard DMDRVI_IOCTL_GFX_* command; these commands cover the rest.
 */
typedef enum
{
    /* Private commands start at DMDRVI_IOCTL_CUSTOM_BASE: everything below
     * it is a standard dmdrvi command (network, block, monitor, graphics). */
    dmlcdtft_ioctl_cmd_get_status = DMDRVI_IOCTL_CUSTOM_BASE, /**< arg: dmlcdtft_status_t* */
    dmlcdtft_ioctl_cmd_set_background_color,    /**< arg: const uint32_t* 0xRRGGBB */
    dmlcdtft_ioctl_cmd_get_background_color,    /**< arg: uint32_t* */
    dmlcdtft_ioctl_cmd_set_alpha,               /**< arg: const uint8_t* */
    dmlcdtft_ioctl_cmd_get_alpha,               /**< arg: uint8_t* */

    dmlcdtft_ioctl_cmd_max
} dmlcdtft_ioctl_cmd_t;

/**
 * @brief Opaque driver context type (forward declaration)
 *
 * The concrete definition is private to the dmlcdtft driver.
 */
struct dmdrvi_context;
typedef struct dmdrvi_context *dmdrvi_context_t;

#endif /* DMLCDTFT_TYPES_H */
