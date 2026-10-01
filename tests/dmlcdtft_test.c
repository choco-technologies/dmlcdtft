#define DMOD_ENABLE_REGISTRATION ON
#define ENABLE_DIF_REGISTRATIONS ON
#include "dmod_test.h"
#include "dmlcdtft.h"
#include "dmdrvi.h"
#include "dmini.h"
#include <errno.h>
#include <string.h>

/* A small 16x8 panel keeps the framebuffer checks cheap. */
#define TEST_WIDTH      16
#define TEST_HEIGHT     8

#define TEST_INI_NO_FORMAT \
    "[lcd]\n" \
    "driver_name=dmlcdtft\n" \
    "width=16\nheight=8\npixel_clock=9600000\n" \
    "hsync_width=41\nhback_porch=13\nhfront_porch=32\n" \
    "vsync_width=10\nvback_porch=2\nvfront_porch=2\n" \
    "background_color=0x102030\nclear_color=0xFF0000FF\n"

#define TEST_INI    TEST_INI_NO_FORMAT "pixel_format=rgb565\n"

typedef struct
{
    dmod_dmdrvi_create_t    create;
    dmod_dmdrvi_free_t      free;
    dmod_dmdrvi_open_t      open;
    dmod_dmdrvi_close_t     close;
    dmod_dmdrvi_read_t      read;
    dmod_dmdrvi_write_t     write;
    dmod_dmdrvi_ioctl_t     ioctl;
    dmod_dmdrvi_stat_t      stat;
} driver_t;

typedef struct
{
    driver_t            drv;
    dmini_context_t     ini;
    dmdrvi_context_t    ctx;
    void*               handle;
} device_t;

static dmlcdtft_config_t g_config;

void dmod_test_setup(void)
{
    memset(&g_config, 0, sizeof(g_config));
    g_config.width                 = 480;
    g_config.height                = 272;
    g_config.pixel_format          = dmlcdtft_pixel_format_rgb565;
    g_config.timing.pixel_clock_hz = 9600000;
    g_config.timing.hsync_width    = 41;
    g_config.timing.vsync_width    = 10;
}

void dmod_test_teardown(void)
{
}

/* ---- Pure helpers ---- */

DMOD_TEST_STEP(dmlcdtft_validate_config_accepts_valid)
{
    DMOD_TEST_EXPECT_TRUE(dmlcdtft_validate_config(&g_config));
}

DMOD_TEST_STEP(dmlcdtft_validate_config_rejects_bad_values)
{
    DMOD_TEST_EXPECT_FALSE(dmlcdtft_validate_config(NULL));

    g_config.width = 0;
    DMOD_TEST_EXPECT_FALSE(dmlcdtft_validate_config(&g_config));

    dmod_test_setup();
    g_config.timing.pixel_clock_hz = 0;
    DMOD_TEST_EXPECT_FALSE(dmlcdtft_validate_config(&g_config));

    dmod_test_setup();
    g_config.pixel_format = dmlcdtft_pixel_format_count;
    DMOD_TEST_EXPECT_FALSE(dmlcdtft_validate_config(&g_config));

    dmod_test_setup();
    g_config.timing.hfront_porch = 4000;
    DMOD_TEST_EXPECT_FALSE(dmlcdtft_validate_config(&g_config));
}

DMOD_TEST_STEP(dmlcdtft_bytes_per_pixel_matches_formats)
{
    DMOD_TEST_EXPECT_EQ(dmlcdtft_bytes_per_pixel(dmlcdtft_pixel_format_argb8888), 4);
    DMOD_TEST_EXPECT_EQ(dmlcdtft_bytes_per_pixel(dmlcdtft_pixel_format_rgb888), 3);
    DMOD_TEST_EXPECT_EQ(dmlcdtft_bytes_per_pixel(dmlcdtft_pixel_format_rgb565), 2);
    DMOD_TEST_EXPECT_EQ(dmlcdtft_bytes_per_pixel(dmlcdtft_pixel_format_argb1555), 2);
    DMOD_TEST_EXPECT_EQ(dmlcdtft_bytes_per_pixel(dmlcdtft_pixel_format_argb4444), 2);
    DMOD_TEST_EXPECT_EQ(dmlcdtft_bytes_per_pixel(dmlcdtft_pixel_format_count), 0);
}

DMOD_TEST_STEP(dmlcdtft_color_to_pixel_converts)
{
    DMOD_TEST_EXPECT_EQ(dmlcdtft_color_to_pixel(dmlcdtft_pixel_format_rgb565, 0xFFFF0000), 0xF800);
    DMOD_TEST_EXPECT_EQ(dmlcdtft_color_to_pixel(dmlcdtft_pixel_format_rgb565, 0xFF00FF00), 0x07E0);
    DMOD_TEST_EXPECT_EQ(dmlcdtft_color_to_pixel(dmlcdtft_pixel_format_rgb565, 0xFF0000FF), 0x001F);
    DMOD_TEST_EXPECT_EQ(dmlcdtft_color_to_pixel(dmlcdtft_pixel_format_rgb888, 0xFF123456), 0x123456);
    DMOD_TEST_EXPECT_EQ(dmlcdtft_color_to_pixel(dmlcdtft_pixel_format_argb8888, 0x80123456), 0x80123456);
    DMOD_TEST_EXPECT_EQ(dmlcdtft_color_to_pixel(dmlcdtft_pixel_format_argb1555, 0x80FF0000), 0xFC00);
    DMOD_TEST_EXPECT_EQ(dmlcdtft_color_to_pixel(dmlcdtft_pixel_format_argb4444, 0xF0F0A050), 0xFFA5);
}

/* ---- Through the dmdrvi DIF, the way dmdevfs calls the driver ---- */

static bool get_driver(driver_t* drv)
{
    Dmod_Context_t* module = Dmod_GetModuleContext("dmlcdtft");
    if (module == NULL)
    {
        return false;
    }
    drv->create = Dmod_GetDifFunction(module, dmod_dmdrvi_create_sig);
    drv->free   = Dmod_GetDifFunction(module, dmod_dmdrvi_free_sig);
    drv->open   = Dmod_GetDifFunction(module, dmod_dmdrvi_open_sig);
    drv->close  = Dmod_GetDifFunction(module, dmod_dmdrvi_close_sig);
    drv->read   = Dmod_GetDifFunction(module, dmod_dmdrvi_read_sig);
    drv->write  = Dmod_GetDifFunction(module, dmod_dmdrvi_write_sig);
    drv->ioctl  = Dmod_GetDifFunction(module, dmod_dmdrvi_ioctl_sig);
    drv->stat   = Dmod_GetDifFunction(module, dmod_dmdrvi_stat_sig);
    return drv->create != NULL && drv->free != NULL && drv->open != NULL && drv->close != NULL &&
           drv->read != NULL && drv->write != NULL && drv->ioctl != NULL && drv->stat != NULL;
}

static bool device_open(device_t* dev, const char* ini_text)
{
    memset(dev, 0, sizeof(*dev));
    if (!get_driver(&dev->drv))
    {
        return false;
    }
    dev->ini = dmini_create();
    dmini_parse_string(dev->ini, ini_text);
    dmini_set_active_section(dev->ini, "lcd", 0);

    dmdrvi_dev_num_t dev_num = { 0 };
    dev->ctx = dev->drv.create(dev->ini, &dev_num);
    dev->handle = (dev->ctx != NULL) ? dev->drv.open(dev->ctx, DMDRVI_O_RDWR, &dev_num) : NULL;
    return dev->handle != NULL && dev_num.flags == DMDRVI_NUM_MAJOR && dev_num.major == 0;
}

static void device_close(device_t* dev)
{
    if (dev->handle != NULL)
    {
        dev->drv.close(dev->ctx, dev->handle);
    }
    if (dev->ctx != NULL)
    {
        dev->drv.free(dev->ctx);
    }
    if (dev->ini != NULL)
    {
        dmini_destroy(dev->ini);
    }
}

DMOD_TEST_STEP(dmlcdtft_create_reports_geometry)
{
    device_t dev;
    DMOD_TEST_EXPECT_TRUE(device_open(&dev, TEST_INI));
    if (dev.handle != NULL)
    {
        dmlcdtft_info_t info;
        memset(&info, 0, sizeof(info));
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmlcdtft_ioctl_cmd_get_info, &info), 0);
        DMOD_TEST_EXPECT_EQ(info.width, TEST_WIDTH);
        DMOD_TEST_EXPECT_EQ(info.height, TEST_HEIGHT);
        DMOD_TEST_EXPECT_EQ(info.bytes_per_pixel, 2);
        DMOD_TEST_EXPECT_EQ(info.stride, TEST_WIDTH * 2);
        DMOD_TEST_EXPECT_EQ(info.framebuffer_size, TEST_WIDTH * TEST_HEIGHT * 2);
        DMOD_TEST_EXPECT_EQ(info.buffer_count, 1);
        DMOD_TEST_EXPECT_EQ(info.pixel_clock_hz, 9600000);

        dmdrvi_stat_t st;
        DMOD_TEST_EXPECT_EQ(dev.drv.stat(dev.ctx, "/dev/dmlcdtft0", &st), 0);
        DMOD_TEST_EXPECT_EQ(st.size, (dmdrvi_size_t)(TEST_WIDTH * TEST_HEIGHT * 2));

        uint32_t background = 0;
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmlcdtft_ioctl_cmd_get_background_color, &background), 0);
        DMOD_TEST_EXPECT_EQ(background, 0x102030);
    }
    device_close(&dev);
}

DMOD_TEST_STEP(dmlcdtft_create_rejects_invalid_config)
{
    device_t dev;
    DMOD_TEST_EXPECT_FALSE(device_open(&dev, "[lcd]\ndriver_name=dmlcdtft\nwidth=16\nheight=8\n"));
    DMOD_TEST_EXPECT_NULL(dev.ctx);
    device_close(&dev);

    DMOD_TEST_EXPECT_FALSE(device_open(&dev, TEST_INI_NO_FORMAT "pixel_format=yuv\n"));
    DMOD_TEST_EXPECT_NULL(dev.ctx);
    device_close(&dev);
}

DMOD_TEST_STEP(dmlcdtft_framebuffer_is_cleared_and_writable)
{
    device_t dev;
    DMOD_TEST_EXPECT_TRUE(device_open(&dev, TEST_INI));
    if (dev.handle != NULL)
    {
        uint16_t line[TEST_WIDTH];
        memset(line, 0, sizeof(line));

        /* clear_color=0xFF0000FF -> RGB565 blue everywhere */
        DMOD_TEST_EXPECT_EQ(dev.drv.read(dev.ctx, dev.handle, line, sizeof(line), 0), (dmdrvi_ssize_t)sizeof(line));
        DMOD_TEST_EXPECT_EQ(line[0], 0x001F);
        DMOD_TEST_EXPECT_EQ(line[TEST_WIDTH - 1], 0x001F);

        for (int i = 0; i < TEST_WIDTH; i++)
        {
            line[i] = (uint16_t)(0x1000 + i);
        }
        dmdrvi_offset_t last_line = (TEST_HEIGHT - 1) * TEST_WIDTH * 2;
        DMOD_TEST_EXPECT_EQ(dev.drv.write(dev.ctx, dev.handle, line, sizeof(line), last_line), (dmdrvi_ssize_t)sizeof(line));

        uint16_t* fb = NULL;
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmlcdtft_ioctl_cmd_get_framebuffer, &fb), 0);
        DMOD_TEST_EXPECT_NOT_NULL(fb);
        if (fb != NULL)
        {
            DMOD_TEST_EXPECT_EQ(fb[(TEST_HEIGHT - 1) * TEST_WIDTH + 5], 0x1005);
        }

        /* Past the end: write is truncated, then refused; read hits EOF. */
        DMOD_TEST_EXPECT_EQ(dev.drv.write(dev.ctx, dev.handle, line, sizeof(line), last_line + 8), (dmdrvi_ssize_t)(sizeof(line) - 8));
        DMOD_TEST_EXPECT_EQ(dev.drv.write(dev.ctx, dev.handle, line, sizeof(line), last_line + sizeof(line)), -ENOSPC);
        DMOD_TEST_EXPECT_EQ(dev.drv.read(dev.ctx, dev.handle, line, sizeof(line), last_line + sizeof(line)), 0);
        DMOD_TEST_EXPECT_EQ(dev.drv.read(dev.ctx, dev.handle, line, sizeof(line), -1), -EINVAL);
    }
    device_close(&dev);
}

DMOD_TEST_STEP(dmlcdtft_fill_rect_clips_to_screen)
{
    device_t dev;
    DMOD_TEST_EXPECT_TRUE(device_open(&dev, TEST_INI));
    if (dev.handle != NULL)
    {
        /* Starts inside, reaches far past the bottom-right corner. */
        dmlcdtft_fill_rect_t rect = { 12, 6, 100, 100, 0xFFFF0000 };
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmlcdtft_ioctl_cmd_fill_rect, &rect), 0);

        uint16_t* fb = NULL;
        dev.drv.ioctl(dev.ctx, dev.handle, dmlcdtft_ioctl_cmd_get_framebuffer, &fb);
        if (fb != NULL)
        {
            DMOD_TEST_EXPECT_EQ(fb[6 * TEST_WIDTH + 12], 0xF800);
            DMOD_TEST_EXPECT_EQ(fb[7 * TEST_WIDTH + 15], 0xF800);
            DMOD_TEST_EXPECT_EQ(fb[6 * TEST_WIDTH + 11], 0x001F);
            DMOD_TEST_EXPECT_EQ(fb[5 * TEST_WIDTH + 12], 0x001F);
        }
    }
    device_close(&dev);
}

DMOD_TEST_STEP(dmlcdtft_double_buffer_swaps)
{
    device_t dev;
    DMOD_TEST_EXPECT_TRUE(device_open(&dev, TEST_INI "double_buffer=on\n"));
    if (dev.handle != NULL)
    {
        dmlcdtft_info_t info;
        void* first = NULL;
        void* second = NULL;
        dev.drv.ioctl(dev.ctx, dev.handle, dmlcdtft_ioctl_cmd_get_info, &info);
        DMOD_TEST_EXPECT_EQ(info.buffer_count, 2);

        dev.drv.ioctl(dev.ctx, dev.handle, dmlcdtft_ioctl_cmd_get_framebuffer, &first);
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmlcdtft_ioctl_cmd_swap_buffers, NULL), 0);
        dev.drv.ioctl(dev.ctx, dev.handle, dmlcdtft_ioctl_cmd_get_framebuffer, &second);
        DMOD_TEST_EXPECT_NOT_NULL(first);
        DMOD_TEST_EXPECT_NOT_NULL(second);
        DMOD_TEST_EXPECT_TRUE(first != second);
    }
    device_close(&dev);

    DMOD_TEST_EXPECT_TRUE(device_open(&dev, TEST_INI));
    if (dev.handle != NULL)
    {
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmlcdtft_ioctl_cmd_swap_buffers, NULL), -ENOTSUP);
    }
    device_close(&dev);
}

DMOD_TEST_STEP(dmlcdtft_ioctl_answers_only_its_own_command_range)
{
    device_t dev;
    DMOD_TEST_EXPECT_TRUE(device_open(&dev, TEST_INI));
    if (dev.handle != NULL)
    {
        uint32_t probe[16] = { 0 };
        bool enabled = false;

        /* Standard dmdrvi commands - dmdevfs sends the first two to every node. */
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, DMDRVI_IOCTL_BLOCK_GET_INFO, probe), -ENOTTY);
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, DMDRVI_IOCTL_MONITOR_GET_POLICY, probe), -ENOTTY);
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, DMDRVI_IOCTL_NET_GET_MAC_ADDR, probe), -ENOTTY);
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, 0, probe), -ENOTTY);
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, -1, probe), -ENOTTY);
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmlcdtft_ioctl_cmd_max, probe), -ENOTTY);

        /* dmlcdtft's own commands, from DMDRVI_IOCTL_CUSTOM_BASE. */
        DMOD_TEST_EXPECT_EQ((int)dmlcdtft_ioctl_cmd_get_info, DMDRVI_IOCTL_CUSTOM_BASE);
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmlcdtft_ioctl_cmd_get_info, NULL), -EINVAL);
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmlcdtft_ioctl_cmd_get_display_enabled, &enabled), 0);
        DMOD_TEST_EXPECT_TRUE(enabled);
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmlcdtft_ioctl_cmd_wait_vsync, NULL), 0);

        enabled = false;
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmlcdtft_ioctl_cmd_set_display_enabled, &enabled), 0);
        DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmlcdtft_ioctl_cmd_wait_vsync, NULL), -EAGAIN);
    }
    device_close(&dev);
}
