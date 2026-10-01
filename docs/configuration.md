# DMLCDTFT Configuration Guide

`dmlcdtft` is configured by `dmdevfs` from an INI section with
`driver_name=dmlcdtft`. The section name is free - dmdevfs hands the driver
its own section.

## Parameters

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `instance` | integer | 0 | Controller instance (STM32F4/F7 have one LTDC: 0) |
| `width` | integer | (required) | Active width in pixels |
| `height` | integer | (required) | Active height in lines |
| `pixel_format` | string | `rgb565` | `argb8888`, `rgb888`, `rgb565`, `argb1555`, `argb4444` |
| `pixel_clock` | integer | (required) | Pixel clock in Hz. The port picks the closest one it can generate (see `lcdtest info`) |
| `hsync_width` | integer | 1 | HSYNC pulse width, pixel clocks |
| `hback_porch` | integer | 0 | Horizontal back porch, pixel clocks |
| `hfront_porch` | integer | 0 | Horizontal front porch, pixel clocks |
| `vsync_width` | integer | 1 | VSYNC pulse width, lines |
| `vback_porch` | integer | 0 | Vertical back porch, lines |
| `vfront_porch` | integer | 0 | Vertical front porch, lines |
| `hsync_polarity` | string | `low` | Active level of HSYNC: `low` / `high` |
| `vsync_polarity` | string | `low` | Active level of VSYNC |
| `de_polarity` | string | `low` | Active level of data enable |
| `pclk_inverted` | `on`/`off` | `off` | Sample data on the falling pixel clock edge |
| `background_color` | `0xRRGGBB` | `0x000000` | Shown where the layer is transparent |
| `alpha` | integer | 255 | Constant opacity of the framebuffer layer |
| `clear_color` | `0xAARRGGBB` | `0xFF000000` | The framebuffer is filled with it at start |
| `double_buffer` | `on`/`off` | `off` | Allocate a second framebuffer for `swap_buffers` |
| `display` | `on`/`off` | `on` | Start with the display enabled |
| `backlight` | `on`/`off` | `on` | Start with the backlight on |
| `display_enable_active_level` | `high`/`low` | `high` | Level of the `display_enable` pin when on |
| `backlight_active_level` | `high`/`low` | `high` | Level of the `backlight` pin when on |

Timing values are the plain widths from the panel datasheet; the port turns
them into whatever its controller needs (accumulated values on the LTDC).
Total width (`hsync + hbp + width + hfp`) is limited to 4096 and total height
to 2048.

## Framebuffer memory

The framebuffer (`width * height * bytes_per_pixel`, twice with
`double_buffer=on`) is allocated through the default heap list on a 64-byte
boundary. A 480x272 RGB565 buffer is 255 KiB - more than the internal heap of
an STM32F746 has left - so it lands in external SDRAM registered by
[dmfmc](https://github.com/choco-technologies/dmfmc) with `heap_usage=heap`.
Give the display a `driver_order` after the SDRAM's.

## Panel pins

The RGB, sync, clock and DE signals are plain alternate-function pins: one
`dmgpio` section per port with a `pins=` bitmask is enough.

Most panels also have a *display on* input and a backlight enable. Configure
each as a `dmgpio` output in the same `friends_group` as the display, with
`friend_role=display_enable` or `friend_role=backlight`. `dmdevfs` reports
their node paths to dmlcdtft (`dmdrvi_friend_changed()`), which drives them
when the display starts and on `set_display_enabled` / `set_backlight`.

## Example (STM32F746G-DISCO)

See [`configs/board/stm32f746g-disco/lcd.ini`](../configs/board/stm32f746g-disco/lcd.ini):

```ini
[lcd]
driver_name=dmlcdtft
driver_order=6
friends_group=lcd
width=480
height=272
pixel_format=rgb565
pixel_clock=9600000
hsync_width=41
hback_porch=13
hfront_porch=32
vsync_width=10
vback_porch=2
vfront_porch=2

[lcd_backlight]
driver_name=dmgpio
driver_order=6
friends_group=lcd
friend_role=backlight
pin=PK3
mode=output
```
