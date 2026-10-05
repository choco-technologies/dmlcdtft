# dmlcdtft

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmlcdtft/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmlcdtft/actions/workflows/ci.yml)

DMOD driver for parallel RGB LCD-TFT displays.

## Description

`dmlcdtft` implements the [dmdrvi](https://github.com/choco-technologies/dmdrvi)
driver interface (2.0) for display controllers that scan a framebuffer out to
a parallel RGB panel - the LTDC on STM32F4 (429/439/469/479) and STM32F7
(746/756/767/769). `dmdevfs` configures it from an INI file and exposes the
display as `/dev/dmlcdtft0`:

- the device file *is* the framebuffer - `write()`/`read()` at a byte offset,
  like a Linux `/dev/fb0`,
- `ioctl` commands for geometry, direct framebuffer access, rectangle fill,
  vsync, double-buffer swapping and presenting (`DMDRVI_IOCTL_GFX_PRESENT`:
  the frame shown at the vertical blank, the area drawn copied into the next
  drawing buffer), display/backlight on/off, background color
  and layer alpha,
- RGB565, RGB888, ARGB8888, ARGB1555 and ARGB4444,
- the pixel clock is generated at run time from PLLSAI (closest achievable
  frequency to the requested one),
- a splash screen from the first frame on: `clear_color` with a logo
  (`.dmvir`, path in `splash_logo` or `$SPLASH_LOGO`) in the middle,
- the framebuffer is allocated from the heap - external SDRAM when dmfmc
  registers it (`heap_usage=heap`),
- panel *display on* and backlight pins are dmgpio friends of the display.

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake .. -DDMOD_CPU_FAMILY=stm32f7    # or stm32f4
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

## Testing

The unit tests run on the host against the `x86_64` port, a software
stand-in for the display controller (see `.github/workflows/ci.yml`):

```bash
cmake -S . -B build -DDMOD_TOOLS_NAME=arch/x86_64
cmake --build build --target dmlcdtft test_dmlcdtft
cmake -S . -B build-port -DDMOD_TOOLS_NAME=arch/x86_64 -DDMOD_CPU_FAMILY=x86_64
cmake --build build-port --target dmlcdtft_port
cp build-port/dmf/dmlcdtft_port.dmf build-port/dmf/dmlcdtft_port.dmd build/dmf/

export DMOD_DMF_DIR=$(pwd)/build/dmf
dmf-get install -d build/dmf/test_dmlcdtft.dmd -y
dmod_loader build/dmf/test_dmlcdtft.dmf
```

On a target, `lcdtest` (see [tools/lcdtest](tools/lcdtest/README.md)) draws
test patterns, reads the framebuffer back and measures the refresh rate.

## Usage

Add the driver and its board configuration to the firmware, e.g. in
dmod-boot's `configs/board/stm32f746g-disco/flash.dmd`:

```
dmlcdtft driver=board/stm32f746g-disco/lcd.ini
lcdtest
```

Then, from any module:

```c
#include "dmlcdtft_types.h"

void *fb = Dmod_FileOpen("/dev/dmlcdtft0", "r+");
dmdrvi_gfx_info_t info;
Dmod_Ioctl(fb, DMDRVI_IOCTL_GFX_GET_INFO, &info);
dmdrvi_gfx_fill_rect_t rect = { 0, 0, info.width, info.height, 0xFF0000FF };
Dmod_Ioctl(fb, DMDRVI_IOCTL_GFX_FILL_RECT, &rect);
Dmod_FileClose(fb);
```

## Documentation

- **[docs/api-reference.md](docs/api-reference.md)** - device file, ioctl commands, types
- **[docs/configuration.md](docs/configuration.md)** - INI keys and board setup
- **[docs/port-implementation.md](docs/port-implementation.md)** - adding a port
- **[configs/README.md](configs/README.md)** - ready-made board configurations

View documentation using `dmf-man dmlcdtft`.

## Hardware Port

This repository ships two DMOD modules: the architecture-independent
`dmlcdtft` and `dmlcdtft_port`. The STM32F4 and STM32F7 ports share their
implementation in `src/port/stm32_common/`; each family's `port.c` only adds
the lifecycle hooks, the two LTDC interrupt handlers and data cache
maintenance (F7). Select the family with `DMOD_CPU_FAMILY` (default
`stm32f7`).

## Project Structure

```
dmlcdtft/
├── configs/board/          # Ready-made board configurations
├── docs/                   # Documentation (markdown format)
├── include/
│   ├── dmlcdtft.h          # Module API
│   ├── dmlcdtft_port.h     # Port API
│   └── dmlcdtft_types.h    # Config, info, ioctl definitions
├── src/
│   ├── dmlcdtft.c          # dmdrvi driver (core)
│   ├── dmlcdtft_splash.c   # Splash logo (.dmvir) drawing
│   └── port/
│       ├── CMakeLists.txt
│       ├── stm32_common/   # LTDC + PLLSAI, shared by F4/F7
│       ├── stm32f4/        # config.cmake + thin port.c
│       ├── stm32f7/        # config.cmake + thin port.c
│       └── x86_64/         # Software stand-in for host tests
├── tests/                  # Host unit tests (dmod_loader)
├── tools/lcdtest/          # On-target test application
├── CMakeLists.txt
├── Makefile
├── dmlcdtft.dmr
├── dmlcdtft_port.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak

## License

MIT
