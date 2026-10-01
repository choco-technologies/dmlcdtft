# dmlcdtft API Reference

`dmlcdtft` is a [dmdrvi](https://github.com/choco-technologies/dmdrvi) driver
(DIF version 2.0) for parallel RGB LCD-TFT panels driven by a display
controller - the LTDC on STM32F4/F7. `dmdevfs` creates one device node per
configured controller, `/dev/dmlcdtft<instance>` (`/dev/dmlcdtft0`).

Applications use the node like a Linux framebuffer device: the file contents
*are* the framebuffer, and everything else goes through `ioctl`.

## Device file

| Operation | Behavior |
|-----------|----------|
| `open` / `close` | No per-handle state - the display keeps running. |
| `read(offset)` | Copies from the drawing buffer at a byte offset. Returns `0` (EOF) at or past the end. |
| `write(offset)` | Copies into the drawing buffer at a byte offset and makes it visible to the controller. A write crossing the end is truncated; one starting at or past the end fails with `-ENOSPC`. |
| `stat` | `size` = bytes of one framebuffer (`stride * height`). |
| `flush` | Makes the whole drawing buffer visible to the controller (data cache clean on STM32F7). |

Pixels are packed line by line with no padding (`stride = width * bytes_per_pixel`),
little-endian, in the configured pixel format. The byte at offset
`y * stride + x * bytes_per_pixel` is the first byte of pixel `(x, y)`.

## IOCTL commands

The graphics commands are the standard `DMDRVI_IOCTL_GFX_*` set; the driver's
own commands are numbered from `DMDRVI_IOCTL_CUSTOM_BASE`. Any other command,
including the standard dmdrvi network/block/monitor commands, returns `-ENOTTY`.

| Command | `arg` | Description |
|---------|-------|-------------|
| `DMDRVI_IOCTL_GFX_GET_INFO` | `dmdrvi_gfx_info_t*` | Resolution, pixel format, stride, buffer count |
| `DMDRVI_IOCTL_GFX_GET_FRAMEBUFFER` | `void**` | Address of the drawing buffer, for direct drawing (call `flush` afterwards on a cached core) |
| `DMDRVI_IOCTL_GFX_SWAP_BUFFERS` | `NULL` | `double_buffer=on` only: show the drawing buffer from the next frame on (blocks until the switch), then draw into the other one. `-ENOTSUP` with a single buffer |
| `DMDRVI_IOCTL_GFX_WAIT_VSYNC` | `const uint32_t*` timeout in ms, or `NULL` (100 ms) | Block until the next vertical blanking. `-ETIMEDOUT`, or `-EAGAIN` while the display is disabled |
| `DMDRVI_IOCTL_GFX_FILL_RECT` | `const dmdrvi_gfx_fill_rect_t*` | Fill a rectangle (clipped to the screen) with a `0xAARRGGBB` color |
| `DMDRVI_IOCTL_GFX_SET_DISPLAY_ENABLED` / `get_` | `bool*` | Controller scan-out and the `display_enable` pin |
| `DMDRVI_IOCTL_GFX_SET_BACKLIGHT` / `get_` | `bool*` | The `backlight` pin (only lit while the display is enabled) |
| `dmlcdtft_ioctl_cmd_get_status` | `dmlcdtft_status_t*` | Pixel clock actually programmed, FIFO underrun counter |
| `dmlcdtft_ioctl_cmd_set_background_color` / `get_` | `uint32_t*` `0xRRGGBB` | Color shown where the layer is transparent |
| `dmlcdtft_ioctl_cmd_set_alpha` / `get_` | `uint8_t*` | Constant opacity of the framebuffer layer |

A missing `arg` for a command that needs one returns `-EINVAL`.

The `DMDRVI_IOCTL_GFX_*` commands (and `dmdrvi_gfx_*` types) are standard
dmdrvi commands defined in `dmdrvi_ioctl.h`, shared by every graphics driver -
a client only needs the device path. The `dmlcdtft_ioctl_cmd_*` commands are
private to this driver.

## Types (`dmlcdtft_types.h` / `dmdrvi_ioctl.h`)

```c
typedef enum {
    DMDRVI_GFX_PIXEL_FORMAT_ARGB8888, DMDRVI_GFX_PIXEL_FORMAT_RGB888,
    DMDRVI_GFX_PIXEL_FORMAT_RGB565,   DMDRVI_GFX_PIXEL_FORMAT_ARGB1555,
    DMDRVI_GFX_PIXEL_FORMAT_ARGB4444,
} dmdrvi_gfx_pixel_format_t;

typedef struct {
    uint16_t width, height;
    dmdrvi_gfx_pixel_format_t pixel_format;
    uint8_t  bytes_per_pixel;
    uint32_t stride;            /* bytes of one line */
    uint32_t framebuffer_size;  /* bytes of one buffer */
    uint8_t  buffer_count;      /* 1, or 2 with double_buffer */
} dmdrvi_gfx_info_t;             /* dmdrvi_ioctl.h */

typedef struct {
    uint32_t pixel_clock_hz;    /* actually programmed */
    uint32_t underrun_count;    /* FIFO underruns / transfer errors */
} dmlcdtft_status_t;

typedef struct {
    uint16_t x, y, width, height;
    uint32_t color;             /* 0xAARRGGBB */
} dmdrvi_gfx_fill_rect_t;
```

`dmlcdtft_config_t` and `dmlcdtft_timing_t` mirror the configuration keys
(see [configuration.md](configuration.md)).

## Module API (`dmlcdtft.h`)

Callable from other modules (link with `dmod_link_modules(... dmlcdtft)`):

| Function | Description |
|----------|-------------|
| `bool dmlcdtft_validate_config(const dmlcdtft_config_t*)` | Check a configuration without touching hardware |
| `uint8_t dmlcdtft_bytes_per_pixel(dmdrvi_gfx_pixel_format_t)` | 2, 3 or 4 (0 for an unknown format) |
| `uint32_t dmlcdtft_color_to_pixel(dmdrvi_gfx_pixel_format_t, uint32_t argb)` | Raw pixel value of a `0xAARRGGBB` color |

## Example

```c
#include "dmlcdtft_types.h"

void *fb = Dmod_FileOpen("/dev/dmlcdtft0", "r+");
dmdrvi_gfx_info_t info;
Dmod_Ioctl(fb, DMDRVI_IOCTL_GFX_GET_INFO, &info);

dmdrvi_gfx_fill_rect_t sky = { 0, 0, info.width, info.height / 2, 0xFF3080FF };
Dmod_Ioctl(fb, DMDRVI_IOCTL_GFX_FILL_RECT, &sky);

/* or write raw pixels: one line at y = 10 */
Dmod_FileSeek(fb, 10 * info.stride, DMOD_SEEK_SET);
Dmod_FileWrite(line, 1, info.stride, fb);

Dmod_FileClose(fb);
```

See `tools/lcdtest/main.c` for a complete application.
