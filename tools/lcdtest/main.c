#include "dmod.h"
#include "dmosi.h"
#include "dmlcdtft.h"
#include <errno.h>
#include <string.h>

/**
 * @brief Manual dmlcdtft device test tool.
 *
 * Works on an already-configured dmlcdtft device node - it does not
 * configure or create anything itself. Everything goes through the device
 * file (ioctl/write/read), exactly as any other application would use it.
 */

#define DEFAULT_DEVICE      "/dev/dmlcdtft0"
#define VSYNC_TIMEOUT_MS    200U

typedef struct
{
    void           *fp;
    dmdrvi_gfx_info_t info;
} display_t;

static void print_usage(const char *name)
{
    Dmod_Printf("Usage: %s [-d DEVICE] COMMAND [ARGS]\n", name);
    Dmod_Printf("  info               print the display configuration\n");
    Dmod_Printf("  bars               color bars, gray ramp and a frame (default)\n");
    Dmod_Printf("  gradient           RGB gradient drawn through write()\n");
    Dmod_Printf("  fill 0xAARRGGBB    fill the whole screen\n");
    Dmod_Printf("  selftest           draw, read back and compare\n");
    Dmod_Printf("  vsync [FRAMES]     measure the refresh rate (default 60 frames)\n");
    Dmod_Printf("  anim [FRAMES]      bouncing box, double buffered if configured\n");
    Dmod_Printf("DEVICE defaults to %s\n", DEFAULT_DEVICE);
}

static bool display_open(display_t *d, const char *path)
{
    d->fp = Dmod_FileOpen(path, "r+");
    if (d->fp == NULL)
    {
        DMOD_LOG_ERROR("lcdtest: failed to open '%s'\n", path);
        return false;
    }
    if (Dmod_Ioctl(d->fp, DMDRVI_IOCTL_GFX_GET_INFO, &d->info) != 0)
    {
        DMOD_LOG_ERROR("lcdtest: '%s' is not a graphics device\n", path);
        Dmod_FileClose(d->fp);
        return false;
    }
    return true;
}

static int fill(display_t *d, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t argb)
{
    dmdrvi_gfx_fill_rect_t rect = { x, y, w, h, argb };
    return Dmod_Ioctl(d->fp, DMDRVI_IOCTL_GFX_FILL_RECT, &rect);
}

static uint32_t parse_hex(const char *s)
{
    uint32_t value = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    for (; *s != '\0'; s++)
    {
        char c = *s;
        uint32_t digit = (c >= '0' && c <= '9') ? (uint32_t)(c - '0')
                       : (c >= 'a' && c <= 'f') ? (uint32_t)(c - 'a' + 10)
                       : (c >= 'A' && c <= 'F') ? (uint32_t)(c - 'A' + 10) : 0U;
        value = (value << 4) | digit;
    }
    return value;
}

static uint32_t parse_count(const char *s, uint32_t default_value)
{
    uint32_t value = 0;
    if (s == NULL)
        return default_value;
    for (; *s >= '0' && *s <= '9'; s++)
        value = value * 10U + (uint32_t)(*s - '0');
    return (value != 0U) ? value : default_value;
}

/* A switch, not a table of name pointers: the dmod loader does not relocate
 * pointers stored in initialized data. */
static const char *format_name(dmdrvi_gfx_pixel_format_t format)
{
    switch (format)
    {
        case DMDRVI_GFX_PIXEL_FORMAT_ARGB8888: return "argb8888";
        case DMDRVI_GFX_PIXEL_FORMAT_RGB888:   return "rgb888";
        case DMDRVI_GFX_PIXEL_FORMAT_RGB565:   return "rgb565";
        case DMDRVI_GFX_PIXEL_FORMAT_ARGB1555: return "argb1555";
        case DMDRVI_GFX_PIXEL_FORMAT_ARGB4444: return "argb4444";
        default:                             return "?";
    }
}

static int cmd_info(display_t *d)
{
    const dmdrvi_gfx_info_t *i = &d->info;

    Dmod_Printf("resolution:   %ux%u\n", i->width, i->height);
    Dmod_Printf("pixel format: %s (%u bytes/pixel, stride %u)\n",
                format_name(i->pixel_format), i->bytes_per_pixel, i->stride);
    Dmod_Printf("framebuffer:  %u bytes x %u\n", i->framebuffer_size, i->buffer_count);

    dmlcdtft_status_t status;
    if (Dmod_Ioctl(d->fp, dmlcdtft_ioctl_cmd_get_status, &status) == 0)
    {
        Dmod_Printf("pixel clock:  %u Hz\n", status.pixel_clock_hz);
        Dmod_Printf("underruns:    %u\n", status.underrun_count);
    }
    return 0;
}

/* Classic SMPTE-like layout: 8 color bars on the top 2/3, a 16-step gray
 * ramp below and a 2 pixel white frame - every channel and both ends of the
 * range are visible at a glance. */
static int cmd_bars(display_t *d)
{
    static const uint32_t bars[8] = { 0xFFFFFFFF, 0xFFFFFF00, 0xFF00FFFF, 0xFF00FF00,
                                      0xFFFF00FF, 0xFFFF0000, 0xFF0000FF, 0xFF000000 };
    uint16_t w = d->info.width, h = d->info.height;
    uint16_t bars_h = (uint16_t)(h * 2U / 3U);
    int ret = 0;

    for (uint16_t i = 0; i < 8U && ret == 0; i++)
        ret = fill(d, (uint16_t)(i * w / 8U), 0, (uint16_t)((i + 1U) * w / 8U - i * w / 8U), bars_h, bars[i]);
    for (uint16_t i = 0; i < 16U && ret == 0; i++)
    {
        uint32_t level = i * 255U / 15U;
        ret = fill(d, (uint16_t)(i * w / 16U), bars_h, (uint16_t)((i + 1U) * w / 16U - i * w / 16U),
                   (uint16_t)(h - bars_h), 0xFF000000U | (level << 16) | (level << 8) | level);
    }
    if (ret == 0) ret = fill(d, 0, 0, w, 2, 0xFFFFFFFF);
    if (ret == 0) ret = fill(d, 0, (uint16_t)(h - 2U), w, 2, 0xFFFFFFFF);
    if (ret == 0) ret = fill(d, 0, 0, 2, h, 0xFFFFFFFF);
    if (ret == 0) ret = fill(d, (uint16_t)(w - 2U), 0, 2, h, 0xFFFFFFFF);
    return ret;
}

static void store_pixel(uint8_t *dst, uint8_t bytes_per_pixel, uint32_t pixel)
{
    for (uint8_t b = 0; b < bytes_per_pixel; b++)
        dst[b] = (uint8_t)(pixel >> (8U * b));
}

/* Red grows left to right, green top to bottom, blue the other way - drawn
 * one line at a time through write(), so it exercises the file interface. */
static int cmd_gradient(display_t *d)
{
    const dmdrvi_gfx_info_t *i = &d->info;
    uint8_t *line = Dmod_Malloc(i->stride);
    int ret = 0;

    if (line == NULL)
        return -1;
    for (uint32_t y = 0; y < i->height && ret == 0; y++)
    {
        for (uint32_t x = 0; x < i->width; x++)
        {
            uint32_t argb = 0xFF000000U | ((x * 255U / (i->width - 1U)) << 16)
                          | ((y * 255U / (i->height - 1U)) << 8) | (255U - x * 255U / (i->width - 1U));
            store_pixel(&line[x * i->bytes_per_pixel], i->bytes_per_pixel, dmlcdtft_color_to_pixel(i->pixel_format, argb));
        }
        if (Dmod_FileSeek(d->fp, (Dmod_FileOffset_t)(y * i->stride), DMOD_SEEK_SET) != 0 ||
            Dmod_FileWrite(line, 1, i->stride, d->fp) != i->stride)
        {
            DMOD_LOG_ERROR("lcdtest: write of line %u failed\n", y);
            ret = -1;
        }
    }
    Dmod_Free(line);
    return ret;
}

static bool expect_pixel(display_t *d, uint32_t x, uint32_t y, uint32_t argb)
{
    uint8_t raw[4] = { 0 };
    uint32_t expected = dmlcdtft_color_to_pixel(d->info.pixel_format, argb);
    uint32_t actual = 0;

    Dmod_FileSeek(d->fp, (Dmod_FileOffset_t)(y * d->info.stride + x * d->info.bytes_per_pixel), DMOD_SEEK_SET);
    Dmod_FileRead(raw, 1, d->info.bytes_per_pixel, d->fp);
    for (uint8_t b = 0; b < d->info.bytes_per_pixel; b++)
        actual |= (uint32_t)raw[b] << (8U * b);

    if (actual != expected)
        DMOD_LOG_ERROR("lcdtest: pixel (%u,%u) is 0x%X, expected 0x%X\n", x, y, actual, expected);
    return actual == expected;
}

static int cmd_selftest(display_t *d)
{
    uint16_t w = d->info.width, h = d->info.height;
    bool ok = fill(d, 0, 0, w, h, 0xFF0000FF) == 0
           && fill(d, (uint16_t)(w / 4U), (uint16_t)(h / 4U), (uint16_t)(w / 2U), (uint16_t)(h / 2U), 0xFFFF0000) == 0;

    ok = ok && expect_pixel(d, 0, 0, 0xFF0000FF);
    ok = ok && expect_pixel(d, w - 1U, h - 1U, 0xFF0000FF);
    ok = ok && expect_pixel(d, w / 2U, h / 2U, 0xFFFF0000);
    ok = ok && expect_pixel(d, w / 4U - 1U, h / 2U, 0xFF0000FF);
    ok = ok && expect_pixel(d, w / 4U, h / 4U, 0xFFFF0000);

    Dmod_Printf("lcdtest: selftest %s\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : -1;
}

static int cmd_vsync(display_t *d, uint32_t frames)
{
    uint32_t timeout = VSYNC_TIMEOUT_MS;
    int ret = Dmod_Ioctl(d->fp, DMDRVI_IOCTL_GFX_WAIT_VSYNC, &timeout);
    uint32_t start = dmosi_get_tick_count();

    for (uint32_t i = 0; i < frames && ret == 0; i++)
        ret = Dmod_Ioctl(d->fp, DMDRVI_IOCTL_GFX_WAIT_VSYNC, &timeout);
    if (ret != 0)
    {
        DMOD_LOG_ERROR("lcdtest: waiting for vsync failed (%d)\n", ret);
        return ret;
    }

    uint32_t elapsed = dmosi_get_tick_count() - start;
    uint32_t centihertz = (elapsed != 0U) ? (frames * 100000U / elapsed) : 0U;
    Dmod_Printf("lcdtest: %u frames in %u ms = %u.%02u Hz\n", frames, elapsed, centihertz / 100U, centihertz % 100U);
    return 0;
}

/* Draws one frame of the animation: background, then the box. */
static int draw_frame(display_t *d, uint16_t x, uint16_t y, uint16_t size, uint32_t frame)
{
    int ret = fill(d, 0, 0, d->info.width, d->info.height, 0xFF101828);
    if (ret == 0)
        ret = fill(d, x, y, size, size, 0xFF000000U | ((frame * 7U) & 0xFFU) << 16 | 0x00C040U);
    return ret;
}

static int cmd_anim(display_t *d, uint32_t frames)
{
    uint16_t size = (uint16_t)(d->info.height / 4U);
    int32_t x = 0, y = 0, dx = 4, dy = 3;
    bool swap = d->info.buffer_count > 1U;
    int ret = 0;

    for (uint32_t frame = 0; frame < frames && ret == 0; frame++)
    {
        ret = draw_frame(d, (uint16_t)x, (uint16_t)y, size, frame);
        if (ret == 0)
            ret = swap ? Dmod_Ioctl(d->fp, DMDRVI_IOCTL_GFX_SWAP_BUFFERS, NULL)
                       : Dmod_Ioctl(d->fp, DMDRVI_IOCTL_GFX_WAIT_VSYNC, NULL);

        x += dx;
        y += dy;
        if (x < 0 || x + size > d->info.width)  { dx = -dx; x += 2 * dx; }
        if (y < 0 || y + size > d->info.height) { dy = -dy; y += 2 * dy; }
    }
    return ret;
}

/* What was drawn into the drawing buffer goes on the screen - with two
 * buffers that is a switch; a driver without PRESENT has the drawing on the
 * screen already. */
static int present(display_t *d, int ret)
{
    if (ret != 0)
        return ret;
    ret = Dmod_Ioctl(d->fp, DMDRVI_IOCTL_GFX_PRESENT, NULL);
    return (ret == -ENOTTY) ? 0 : ret;
}

static int run_command(display_t *d, int argc, char *argv[])
{
    const char *cmd = (argc > 0) ? argv[0] : "bars";
    const char *arg = (argc > 1) ? argv[1] : NULL;

    if (strcmp(cmd, "info") == 0)       return cmd_info(d);
    if (strcmp(cmd, "bars") == 0)       return present(d, cmd_bars(d));
    if (strcmp(cmd, "gradient") == 0)   return present(d, cmd_gradient(d));
    if (strcmp(cmd, "selftest") == 0)   return present(d, cmd_selftest(d));
    if (strcmp(cmd, "vsync") == 0)      return cmd_vsync(d, parse_count(arg, 60));
    if (strcmp(cmd, "anim") == 0)       return cmd_anim(d, parse_count(arg, 300));
    if (strcmp(cmd, "fill") == 0 && arg != NULL)
        return present(d, fill(d, 0, 0, d->info.width, d->info.height, parse_hex(arg)));

    DMOD_LOG_ERROR("lcdtest: unknown command '%s'\n", cmd);
    return -1;
}

int main(int argc, char *argv[])
{
    const char *device = DEFAULT_DEVICE;
    int first = 1;
    display_t display;

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))
    {
        print_usage(argv[0]);
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "-d") == 0)
    {
        device = argv[2];
        first = 3;
    }

    if (!display_open(&display, device))
        return 1;

    int ret = run_command(&display, argc - first, &argv[first]);
    Dmod_FileClose(display.fp);
    return (ret == 0) ? 0 : 1;
}
