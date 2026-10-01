# Adding a New MCU Port to dmlcdtft

`dmlcdtft_port` currently supports `stm32f4` and `stm32f7` (the LTDC), plus
an `x86_64` software stand-in used only by the host tests. The port is split
from the core `dmlcdtft` module so a new display controller can be added
without touching the architecture-independent logic.

## What lives where

| Core (`src/dmlcdtft.c`) | Port (`src/port/<family>/`) |
|-------------------------|-----------------------------|
| INI parsing and validation | Pixel clock generation |
| Framebuffer allocation and clearing | Panel timing registers |
| `read`/`write`/`fill_rect`, double buffering | Layer setup, scan-out on/off |
| dmdrvi interface, ioctl dispatch | Vsync / reload interrupts |
| Panel control pins (`display_enable`, `backlight` friends) | Data cache maintenance |

The whole port API is in [`include/dmlcdtft_port.h`](../include/dmlcdtft_port.h):

| Function | Purpose |
|----------|---------|
| `_get_instance_count()` | How many controllers the chip has |
| `_init(instance, config, framebuffer)` | Program clock, timing and layer, start scan-out of `framebuffer` |
| `_deinit(instance)` | Stop the controller and release its clock |
| `_set_framebuffer(instance, fb, wait_vblank)` | Scan out another buffer (at the next vblank when requested) |
| `_set_enabled` / `_set_background_color` / `_set_alpha` | Runtime control |
| `_wait_vsync(instance, timeout_ms)` | Block until vertical blanking |
| `_sync(instance, address, size)` | Make CPU writes visible to the controller |
| `_get_pixel_clock` / `_get_underrun_count` | Status |

## STM32: shared implementation

The LTDC is the same IP block on every STM32F4 (429/439/469/479) and STM32F7
(746/756/767/769) part, and so is its clock path (PLLSAI R output divided by
PLLSAIDIVR, same RCC register offsets). All of it lives in
`src/port/stm32_common/`; each family's `port.c` only contains:

- `dmod_init()` / `dmod_deinit()`,
- `DMOD_IRQ_HANDLER(88)` / `DMOD_IRQ_HANDLER(89)` calling `stm32_ltdc_irq_handler()`,
- `stm32_ltdc_family_clean_dcache()` - the one real difference: the
  Cortex-M7 cleans its data cache by address, the Cortex-M4 has none.

The pixel clock is derived at run time: PLLSAI shares the main PLL's input
divider, so its VCO input is computed from `RCC_PLLCFGR` and the SYSCLK
reported by `dmclk_port`, and the closest N/R/DIVR combination is searched.
Only the N and R fields of `RCC_PLLSAICFGR` are touched - P (STM32F7 CK48)
and Q (SAI) keep their values.

`GCR` and `IER` are kept in shadow variables and only ever written: the ISR
must not race with thread-side read-modify-write, and Renode 1.15's LTDC
model reads `GCR` back as 0.

## Steps to add another architecture

1. Create `src/port/<family>/config.cmake`, setting `DMOD_TOOLS_NAME` for the
   target architecture (it must match a directory under `dmod/configs/arch/...`).
2. Create `src/port/<family>/port.c` implementing every function of
   `include/dmlcdtft_port.h` with `dmod_dmlcdtft_port_api_declaration(...)`,
   plus `dmod_init`/`dmod_deinit` and any `DMOD_IRQ_HANDLER(...)`. For another
   STM32 family with an LTDC, only add the thin `port.c` described above -
   `src/port/CMakeLists.txt` adds `stm32_common` for every `stm32*` family.
3. Build with `cmake .. -DDMOD_CPU_FAMILY=<family>`.
4. Do not introduce a module-specific variable (e.g. `<MODULE>_MCU_SERIES`) -
   `DMOD_CPU_FAMILY` is the ecosystem-wide convention used by `dmf-get`.

Note: the dmod loader does not relocate pointers stored in initialized data,
so a module must not contain tables of pointers (e.g. `static const char
*names[]`) - reference string literals from code only.
