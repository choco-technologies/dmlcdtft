# dmlcdtft Documentation

`dmlcdtft` is a dmdrvi driver for parallel RGB LCD-TFT panels (LTDC on
STM32F4/F7). It exposes the framebuffer as `/dev/dmlcdtft0`.

## Contents

- **[api-reference.md](api-reference.md)** - device file semantics, ioctl commands, types, module API
- **[configuration.md](configuration.md)** - INI keys, framebuffer memory, panel control pins
- **[port-implementation.md](port-implementation.md)** - core/port split and adding a new port

## Quick Reference

```c
#include "dmlcdtft_types.h"

void *fb = Dmod_FileOpen("/dev/dmlcdtft0", "r+");
dmdrvi_gfx_fill_rect_t rect = { 0, 0, 100, 50, 0xFFFF0000 };
Dmod_Ioctl(fb, DMDRVI_IOCTL_GFX_FILL_RECT, &rect);
Dmod_FileClose(fb);
```

View documentation using `dmf-man`:

```bash
dmf-man dmlcdtft          # Main documentation
dmf-man dmlcdtft api      # API reference
```
