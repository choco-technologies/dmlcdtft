# DMLCDTFT Configuration Files

Ready-made display configurations for development boards.

```
configs/
└── board/
    └── stm32f746g-disco/
        └── lcd.ini        # 4.3" 480x272 RK043FN48H on the LTDC
```

Each board file contains:
- one `dmgpio` section per GPIO port carrying LTDC signals (`pins=` bitmask,
  alternate function 14 - or 9 for the few pins that use it),
- the `[lcd]` section with `driver_name=dmlcdtft`, resolution, pixel format
  and panel timing,
- `dmgpio` output sections for the panel's *display on* and backlight pins,
  as friends of the display (`friend_role=display_enable` / `backlight`).

See [docs/configuration.md](../docs/configuration.md) for every key.

## Boards

| Board | File | Panel | Notes |
|-------|------|-------|-------|
| 32F746G-DISCOVERY | `board/stm32f746g-disco/lcd.ini` | Rocktech RK043FN48H, 480x272 | Pinout and timing from ST's BSP (`stm32746g_discovery_lcd.c`, `rk043fn48h.h`); verified on the board and in Renode. 9.6 MHz pixel clock, 59.3 Hz refresh. The framebuffer lives in SDRAM (dmfmc `heap_usage=heap`) |

Boards whose display is not a plain parallel RGB panel - e.g. the
STM32F769I-DISCO (MIPI-DSI) or STM32F429I-DISCO (ILI9341, needs SPI
initialization of the panel controller) - need more than this driver.
