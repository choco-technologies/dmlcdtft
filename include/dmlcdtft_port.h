#ifndef DMLCDTFT_PORT_H
#define DMLCDTFT_PORT_H

#include "dmod_types.h"
#include "dmlcdtft_port_defs.h"
#include "dmlcdtft_types.h"

/*
 * The port only drives the display controller. Everything that does not
 * depend on the hardware - configuration parsing, framebuffer allocation,
 * drawing, the dmdrvi device interface and the panel control pins - lives in
 * the core module (src/dmlcdtft.c), so a new port implements nothing but the
 * calls below.
 */

/* --- Capability query --- */

dmod_dmlcdtft_port_api(1.0, dmlcdtft_instance_t, _get_instance_count, ( void ) );

/* --- Lifecycle ---
 *
 * dmlcdtft_port_init() programs the pixel clock, panel timing and the
 * framebuffer layer in one call, and starts scanning out @p framebuffer
 * (allocated by the core: config->width * config->height pixels in
 * config->pixel_format, lines packed without padding).
 */

dmod_dmlcdtft_port_api(1.0, int, _init,   ( dmlcdtft_instance_t instance, const dmlcdtft_config_t* config, const void* framebuffer ) );
dmod_dmlcdtft_port_api(1.0, int, _deinit, ( dmlcdtft_instance_t instance ) );

/* --- Runtime control --- */

/* Scan out another buffer of the same size. With wait_vblank the switch is
 * done at the next vertical blanking and the call blocks until it happened. */
dmod_dmlcdtft_port_api(1.0, int, _set_framebuffer,      ( dmlcdtft_instance_t instance, const void* framebuffer, bool wait_vblank ) );
dmod_dmlcdtft_port_api(1.0, int, _set_enabled,          ( dmlcdtft_instance_t instance, bool enabled ) );
dmod_dmlcdtft_port_api(1.0, int, _set_background_color, ( dmlcdtft_instance_t instance, uint32_t rgb ) );
dmod_dmlcdtft_port_api(1.0, int, _set_alpha,            ( dmlcdtft_instance_t instance, uint8_t alpha ) );

/* Block until the controller enters the vertical blanking interval.
 * Returns -ETIMEDOUT if that did not happen within timeout_ms. */
dmod_dmlcdtft_port_api(1.0, int, _wait_vsync, ( dmlcdtft_instance_t instance, uint32_t timeout_ms ) );

/* Make CPU writes to [address, address + size) visible to the controller
 * (data cache maintenance on cores that have one). */
dmod_dmlcdtft_port_api(1.0, void, _sync, ( dmlcdtft_instance_t instance, const void* address, size_t size ) );

/* --- Status --- */

dmod_dmlcdtft_port_api(1.0, uint32_t, _get_pixel_clock,    ( dmlcdtft_instance_t instance ) );
dmod_dmlcdtft_port_api(1.0, uint32_t, _get_underrun_count, ( dmlcdtft_instance_t instance ) );

#endif // DMLCDTFT_PORT_H
