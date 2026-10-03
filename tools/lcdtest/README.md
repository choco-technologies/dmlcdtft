# lcdtest

Manual test tool for a configured `dmlcdtft` display. It only opens the
device node - configuration is done by `dmdevfs` from the board INI.

```
lcdtest [-d DEVICE] COMMAND [ARGS]
  info               print the display configuration
  bars               color bars, gray ramp and a frame (default)
  gradient           RGB gradient drawn through write()
  fill 0xAARRGGBB    fill the whole screen
  selftest           draw, read back and compare
  vsync [FRAMES]     measure the refresh rate (default 60 frames)
  anim [FRAMES]      bouncing box, double buffered if configured
```

`DEVICE` defaults to `/dev/dmlcdtft0`.

`vsync` is the quickest way to check the pixel clock and timing without
looking at the panel: the rate must equal
`pixel_clock / ((hsync + hbp + width + hfp) * (vsync + vbp + height + vfp))`
- 59.3 Hz on the STM32F746G-DISCO. In Renode it reports the emulator's fixed
repaint rate (`ltdc FramesPerVirtualSecond`, 25 by default) instead.

What `bars`, `gradient`, `fill` and `selftest` draw goes on the screen with
`DMDRVI_IOCTL_GFX_PRESENT` - with `double_buffer=on` the drawing buffer is
not the one shown until then.
