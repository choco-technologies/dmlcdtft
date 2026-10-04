#define DMOD_ENABLE_REGISTRATION    ON
#include "dmod.h"
#include "dmlcdtft.h"
#include "dmlcdtft_port.h"
#include "dmdrvi.h"
#include "dmini.h"
#include "dmgpio_types.h"
#include <errno.h>
#include <string.h>

/* Magic set to "LCDT" */
#define DMLCDTFT_CONTEXT_MAGIC          0x4C434454

/* Framebuffers are allocated on a cache-line boundary, so cache maintenance
 * in the port never touches memory that does not belong to them. */
#define DMLCDTFT_FRAMEBUFFER_ALIGNMENT  64U

/* Default timeout of DMDRVI_IOCTL_GFX_WAIT_VSYNC - a few frames even for
 * slow panels. */
#define DMLCDTFT_VSYNC_TIMEOUT_MS       100U

/* Generic sanity limits - the port checks the exact limits of its controller. */
#define DMLCDTFT_MAX_TOTAL_WIDTH        4096U
#define DMLCDTFT_MAX_TOTAL_HEIGHT       2048U

/* ---- Panel control pins ----
 *
 * Panels usually have a "display on" input and a backlight enable next to the
 * RGB bus. Each is a separate dmgpio device in the same friends_group as this
 * device, with friend_role=display_enable or friend_role=backlight. dmdevfs
 * reports the GPIO's node path through dmdrvi_friend_changed(); we retain a
 * copy of it and drive the pin through the filesystem, exactly like dmspi's
 * chip select. dmdevfs owns GPIO creation, configuration, and lifetime.
 */
typedef struct
{
    char   *path;           /**< Friend GPIO path (NULL = not wired) */
    bool    active_high;    /**< true = "on" is a high level */
} panel_pin_t;

/**
 * @brief DMDRVI context structure
 */
struct dmdrvi_context
{
    uint32_t            magic;              /**< Magic number for validation */
    dmlcdtft_config_t   config;             /**< Configuration parameters */
    uint8_t            *buffers[2];         /**< Framebuffers (buffers[1] only with double_buffer) */
    uint8_t             draw_index;         /**< Buffer read()/write()/drawing goes to */
    uint8_t             bytes_per_pixel;    /**< Derived from config.pixel_format */
    uint32_t            stride;             /**< Bytes of one line */
    uint32_t            framebuffer_size;   /**< Bytes of one buffer */
    bool                display_enabled;    /**< Controller and display_enable pin on */
    bool                backlight_on;       /**< Backlight pin on */
    panel_pin_t         display_enable_pin; /**< friend_role=display_enable */
    panel_pin_t         backlight_pin;      /**< friend_role=backlight */
};

static bool is_valid_context(dmdrvi_context_t context)
{
    return (context != NULL && context->magic == DMLCDTFT_CONTEXT_MAGIC);
}

/* ---- Pixel helpers ---- */

dmod_dmlcdtft_api_declaration(1.0, uint8_t, _bytes_per_pixel, ( dmdrvi_gfx_pixel_format_t format ))
{
    switch (format)
    {
        case DMDRVI_GFX_PIXEL_FORMAT_ARGB8888: return 4;
        case DMDRVI_GFX_PIXEL_FORMAT_RGB888:   return 3;
        case DMDRVI_GFX_PIXEL_FORMAT_RGB565:
        case DMDRVI_GFX_PIXEL_FORMAT_ARGB1555:
        case DMDRVI_GFX_PIXEL_FORMAT_ARGB4444: return 2;
        default:                             return 0;
    }
}

dmod_dmlcdtft_api_declaration(1.0, uint32_t, _color_to_pixel, ( dmdrvi_gfx_pixel_format_t format, uint32_t argb ))
{
    uint32_t a = (argb >> 24) & 0xFFU;
    uint32_t r = (argb >> 16) & 0xFFU;
    uint32_t g = (argb >> 8)  & 0xFFU;
    uint32_t b = argb & 0xFFU;

    switch (format)
    {
        case DMDRVI_GFX_PIXEL_FORMAT_ARGB8888: return argb;
        case DMDRVI_GFX_PIXEL_FORMAT_RGB888:   return argb & 0x00FFFFFFU;
        case DMDRVI_GFX_PIXEL_FORMAT_RGB565:   return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        case DMDRVI_GFX_PIXEL_FORMAT_ARGB1555: return ((a >> 7) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
        case DMDRVI_GFX_PIXEL_FORMAT_ARGB4444: return ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
        default:                             return 0;
    }
}

/* Writes `count` copies of a raw pixel value, little-endian, starting at dst.
 * dst is always pixel-aligned (buffers are 64-byte aligned, stride and x are
 * whole pixels), so the 16/32-bit stores are naturally aligned. */
static void store_pixels(uint8_t *dst, uint8_t bytes_per_pixel, uint32_t pixel, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        switch (bytes_per_pixel)
        {
            case 4: ((uint32_t *)dst)[i] = pixel; break;
            case 2: ((uint16_t *)dst)[i] = (uint16_t)pixel; break;
            default:
                dst[3 * i]     = (uint8_t)pixel;
                dst[3 * i + 1] = (uint8_t)(pixel >> 8);
                dst[3 * i + 2] = (uint8_t)(pixel >> 16);
                break;
        }
    }
}

/* Clips the rectangle to the screen and fills it in the drawing buffer: the
 * first line pixel by pixel, every further line as a copy of the first one. */
static void fill_rect(dmdrvi_context_t context, const dmdrvi_gfx_fill_rect_t *rect)
{
    const dmlcdtft_config_t *c = &context->config;
    if (rect->x >= c->width || rect->y >= c->height || rect->width == 0 || rect->height == 0)
        return;

    uint32_t width  = (rect->width  > c->width  - rect->x) ? (uint32_t)(c->width  - rect->x) : rect->width;
    uint32_t height = (rect->height > c->height - rect->y) ? (uint32_t)(c->height - rect->y) : rect->height;
    uint32_t line_bytes = width * context->bytes_per_pixel;
    uint8_t *first = context->buffers[context->draw_index]
                   + (uint32_t)rect->y * context->stride + (uint32_t)rect->x * context->bytes_per_pixel;

    store_pixels(first, context->bytes_per_pixel, dmlcdtft_color_to_pixel(c->pixel_format, rect->color), width);
    for (uint32_t line = 1; line < height; line++)
        memcpy(first + line * context->stride, first, line_bytes);

    dmlcdtft_port_sync(c->instance, first, (height - 1U) * context->stride + line_bytes);
}

/* ---- String conversion helpers ---- */

/* No table of name pointers here: the dmod loader does not relocate pointers
 * stored in initialized data, so string literals may only be referenced
 * from code. */
static int string_to_pixel_format(const char *s, dmdrvi_gfx_pixel_format_t *out_format)
{
    if (s == NULL)
        return -EINVAL;

    if (strcmp(s, "argb8888") == 0)      *out_format = DMDRVI_GFX_PIXEL_FORMAT_ARGB8888;
    else if (strcmp(s, "rgb888") == 0)   *out_format = DMDRVI_GFX_PIXEL_FORMAT_RGB888;
    else if (strcmp(s, "rgb565") == 0)   *out_format = DMDRVI_GFX_PIXEL_FORMAT_RGB565;
    else if (strcmp(s, "argb1555") == 0) *out_format = DMDRVI_GFX_PIXEL_FORMAT_ARGB1555;
    else if (strcmp(s, "argb4444") == 0) *out_format = DMDRVI_GFX_PIXEL_FORMAT_ARGB4444;
    else return -EINVAL;
    return 0;
}

static dmlcdtft_polarity_t string_to_polarity(const char *s)
{
    return (s != NULL && strcmp(s, "high") == 0) ? dmlcdtft_polarity_active_high : dmlcdtft_polarity_active_low;
}

static bool string_to_switch(const char *s, bool default_value)
{
    if (s == NULL)
        return default_value;
    return strcmp(s, "on") == 0;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parses a color as "0xRRGGBB"/"#RRGGBB" (also 8 digits for AARRGGBB). */
static int parse_color(const char *s, uint32_t *out_color)
{
    if (s[0] == '#')
        s += 1;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;

    uint32_t value = 0;
    int digits = 0;
    for (; *s != '\0'; s++, digits++)
    {
        int d = hex_digit(*s);
        if (d < 0 || digits >= 8)
            return -EINVAL;
        value = (value << 4) | (uint32_t)d;
    }
    if (digits == 0)
        return -EINVAL;

    *out_color = value;
    return 0;
}

static int read_color(dmini_context_t ini, const char *key, uint32_t default_value, uint32_t *out_color)
{
    const char *s = dmini_get_string(ini, NULL, key, NULL);
    *out_color = default_value;
    if (s != NULL && parse_color(s, out_color) != 0)
    {
        DMOD_LOG_ERROR("Invalid %s in configuration: '%s' (expected 0xRRGGBB)\n", key, s);
        return -EINVAL;
    }
    return 0;
}

/* ---- Configuration ---- */

static int check_config_parameters(const dmlcdtft_config_t *c)
{
    const dmlcdtft_timing_t *t = &c->timing;
    uint32_t total_width  = (uint32_t)t->hsync_width + t->hback_porch + c->width  + t->hfront_porch;
    uint32_t total_height = (uint32_t)t->vsync_width + t->vback_porch + c->height + t->vfront_porch;

    if (c->width == 0 || c->height == 0)
    {
        DMOD_LOG_ERROR("Display width/height not set in configuration\n");
        return -EINVAL;
    }
    if (dmlcdtft_bytes_per_pixel(c->pixel_format) == 0)
    {
        DMOD_LOG_ERROR("Invalid pixel format %d\n", (int)c->pixel_format);
        return -EINVAL;
    }
    if (t->pixel_clock_hz == 0 || t->hsync_width == 0 || t->vsync_width == 0)
    {
        DMOD_LOG_ERROR("pixel_clock, hsync_width and vsync_width must be non-zero\n");
        return -EINVAL;
    }
    if (total_width > DMLCDTFT_MAX_TOTAL_WIDTH || total_height > DMLCDTFT_MAX_TOTAL_HEIGHT)
    {
        DMOD_LOG_ERROR("Display timing too large (%ux%u total)\n", total_width, total_height);
        return -EINVAL;
    }
    return 0;
}

dmod_dmlcdtft_api_declaration(1.0, bool, _validate_config, ( const dmlcdtft_config_t *config ))
{
    return (config != NULL) && (check_config_parameters(config) == 0);
}

static void read_timing(dmini_context_t ini, dmlcdtft_timing_t *t)
{
    t->pixel_clock_hz = (uint32_t)dmini_get_int(ini, NULL, "pixel_clock", 0);
    t->hsync_width    = (uint16_t)dmini_get_int(ini, NULL, "hsync_width", 1);
    t->hback_porch    = (uint16_t)dmini_get_int(ini, NULL, "hback_porch", 0);
    t->hfront_porch   = (uint16_t)dmini_get_int(ini, NULL, "hfront_porch", 0);
    t->vsync_width    = (uint16_t)dmini_get_int(ini, NULL, "vsync_width", 1);
    t->vback_porch    = (uint16_t)dmini_get_int(ini, NULL, "vback_porch", 0);
    t->vfront_porch   = (uint16_t)dmini_get_int(ini, NULL, "vfront_porch", 0);
    t->hsync_polarity = string_to_polarity(dmini_get_string(ini, NULL, "hsync_polarity", "low"));
    t->vsync_polarity = string_to_polarity(dmini_get_string(ini, NULL, "vsync_polarity", "low"));
    t->de_polarity    = string_to_polarity(dmini_get_string(ini, NULL, "de_polarity", "low"));
    t->pclk_inverted  = string_to_switch(dmini_get_string(ini, NULL, "pclk_inverted", NULL), false);
}

/**
 * @brief Parse this driver's .ini configuration.
 *
 * Passes NULL as the section to every dmini call: dmdevfs locks the ini
 * context to this driver's own section before calling dmdrvi_create(), and
 * while that restriction is active section == NULL means "the active section"
 * (see dmini.h) - same as dmeth.
 */
static int read_config_parameters(dmdrvi_context_t context, dmini_context_t ini)
{
    dmlcdtft_config_t *c = &context->config;
    const char *format = dmini_get_string(ini, NULL, "pixel_format", "rgb565");

    c->instance      = (dmlcdtft_instance_t)dmini_get_int(ini, NULL, "instance", 0);
    c->width         = (uint16_t)dmini_get_int(ini, NULL, "width", 0);
    c->height        = (uint16_t)dmini_get_int(ini, NULL, "height", 0);
    c->alpha         = (uint8_t)dmini_get_int(ini, NULL, "alpha", 255);
    c->double_buffer = string_to_switch(dmini_get_string(ini, NULL, "double_buffer", NULL), false);
    read_timing(ini, &c->timing);

    if (string_to_pixel_format(format, &c->pixel_format) != 0)
    {
        DMOD_LOG_ERROR("Invalid pixel_format in configuration: '%s'\n", format);
        return -EINVAL;
    }
    if (read_color(ini, "background_color", 0x000000U, &c->background_color) != 0 ||
        read_color(ini, "clear_color", 0xFF000000U, &c->clear_color) != 0)
    {
        return -EINVAL;
    }

    context->display_enabled = string_to_switch(dmini_get_string(ini, NULL, "display", NULL), true);
    context->backlight_on    = string_to_switch(dmini_get_string(ini, NULL, "backlight", NULL), true);
    context->display_enable_pin.active_high =
        (strcmp(dmini_get_string(ini, NULL, "display_enable_active_level", "high"), "high") == 0);
    context->backlight_pin.active_high =
        (strcmp(dmini_get_string(ini, NULL, "backlight_active_level", "high"), "high") == 0);

    return check_config_parameters(c);
}

/* ---- Panel control pins ---- */

static void panel_pin_apply(const panel_pin_t *pin, bool on)
{
    if (pin->path == NULL)
        return;

    dmgpio_pins_state_t state = (on == pin->active_high)
        ? dmgpio_pins_state_all_high
        : dmgpio_pins_state_all_low;

    void *file = Dmod_FileOpen(pin->path, "r+");
    if (file == NULL)
    {
        DMOD_LOG_ERROR("Failed to open panel control GPIO: %s\n", pin->path);
        return;
    }
    if (Dmod_Ioctl(file, dmgpio_ioctl_cmd_set_pins_state, &state) != 0)
        DMOD_LOG_ERROR("Failed to update panel control GPIO: %s\n", pin->path);
    Dmod_FileClose(file);
}

static panel_pin_t *panel_pin_by_role(dmdrvi_context_t context, const char *role)
{
    if (role == NULL)
        return NULL;
    if (strcmp(role, "display_enable") == 0)
        return &context->display_enable_pin;
    if (strcmp(role, "backlight") == 0)
        return &context->backlight_pin;
    return NULL;
}

static void panel_pins_apply(dmdrvi_context_t context)
{
    panel_pin_apply(&context->display_enable_pin, context->display_enabled);
    panel_pin_apply(&context->backlight_pin, context->display_enabled && context->backlight_on);
}

/* ---- Framebuffers ---- */

static void free_framebuffers(dmdrvi_context_t context)
{
    for (int i = 0; i < 2; i++)
    {
        Dmod_Free(context->buffers[i]);
        context->buffers[i] = NULL;
    }
}

/* Allocates one or two framebuffers through the default heap list - with
 * dmfmc's heap_usage=heap this ends up in external SDRAM, since a framebuffer
 * is larger than what the internal heap usually has left. */
static int allocate_framebuffers(dmdrvi_context_t context)
{
    const dmlcdtft_config_t *c = &context->config;
    int count = c->double_buffer ? 2 : 1;

    context->bytes_per_pixel  = dmlcdtft_bytes_per_pixel(c->pixel_format);
    context->stride           = (uint32_t)c->width * context->bytes_per_pixel;
    context->framebuffer_size = context->stride * c->height;

    for (int i = 0; i < count; i++)
    {
        context->buffers[i] = Dmod_AlignedMalloc(context->framebuffer_size, DMLCDTFT_FRAMEBUFFER_ALIGNMENT);
        if (context->buffers[i] == NULL)
        {
            DMOD_LOG_ERROR("Failed to allocate %u bytes for framebuffer %d\n", context->framebuffer_size, i);
            free_framebuffers(context);
            return -ENOMEM;
        }
    }
    return 0;
}

/* Fills every buffer with clear_color, so the panel never shows heap garbage. */
static void clear_framebuffers(dmdrvi_context_t context)
{
    uint8_t draw_index = context->draw_index;
    dmdrvi_gfx_fill_rect_t all = { 0, 0, context->config.width, context->config.height, context->config.clear_color };

    for (uint8_t i = 0; i < 2; i++)
    {
        if (context->buffers[i] == NULL)
            continue;
        context->draw_index = i;
        fill_rect(context, &all);
    }
    context->draw_index = draw_index;
}

/* ---- Controller bring-up ---- */

static int start_display(dmdrvi_context_t context)
{
    const dmlcdtft_config_t *c = &context->config;

    if (c->instance >= dmlcdtft_port_get_instance_count())
    {
        DMOD_LOG_ERROR("LCD-TFT instance %u out of range (this target supports %u)\n",
                       c->instance, dmlcdtft_port_get_instance_count());
        return -ENODEV;
    }

    int ret = allocate_framebuffers(context);
    if (ret != 0)
        return ret;

    /* With double buffering buffer 0 is shown first and drawing goes to 1. */
    context->draw_index = c->double_buffer ? 1 : 0;
    clear_framebuffers(context);

    ret = dmlcdtft_port_init(c->instance, c, context->buffers[0]);
    if (ret == 0 && !context->display_enabled)
        ret = dmlcdtft_port_set_enabled(c->instance, false);
    if (ret != 0)
    {
        DMOD_LOG_ERROR("Failed to initialize LCD-TFT controller %u (%d)\n", c->instance, ret);
        dmlcdtft_port_deinit(c->instance);
        free_framebuffers(context);
        return ret;
    }

    DMOD_LOG_INFO("LCD-TFT%u: %ux%u, %u bpp, pixel clock %u Hz, framebuffer at %p\n",
                  c->instance, c->width, c->height, 8U * context->bytes_per_pixel,
                  dmlcdtft_port_get_pixel_clock(c->instance), (void *)context->buffers[0]);
    return 0;
}

/* ---- IOCTL helpers ---- */

static void get_info(dmdrvi_context_t context, dmdrvi_gfx_info_t *info)
{
    const dmlcdtft_config_t *c = &context->config;

    info->width            = c->width;
    info->height           = c->height;
    info->pixel_format     = c->pixel_format;
    info->bytes_per_pixel  = context->bytes_per_pixel;
    info->stride           = context->stride;
    info->framebuffer_size = context->framebuffer_size;
    info->buffer_count     = c->double_buffer ? 2 : 1;
}

static void get_status(dmdrvi_context_t context, dmlcdtft_status_t *status)
{
    status->pixel_clock_hz = dmlcdtft_port_get_pixel_clock(context->config.instance);
    status->underrun_count = dmlcdtft_port_get_underrun_count(context->config.instance);
}

/* Shows the drawing buffer from the next frame on, and draws into the other
 * one afterwards. The new drawing buffer keeps whatever it held before. */
static int swap_buffers(dmdrvi_context_t context)
{
    if (!context->config.double_buffer)
        return -ENOTSUP;

    uint8_t *shown = context->buffers[context->draw_index];
    dmlcdtft_port_sync(context->config.instance, shown, context->framebuffer_size);

    int ret = dmlcdtft_port_set_framebuffer(context->config.instance, shown, true);
    if (ret == 0)
        context->draw_index ^= 1U;
    return ret;
}

/*
 * DMDRVI_IOCTL_GFX_PRESENT: makes `area` (NULL: everything) of the drawing
 * buffer visible. Double buffered, the drawing buffer is shown from the next
 * frame on - set_framebuffer() waits for the reload in the vertical blank -
 * and only then `area` is copied into the other buffer, which is no longer
 * scanned out: it becomes the drawing buffer, as the screen is.
 */
static int present(dmdrvi_context_t context, const dmdrvi_gfx_rect_t *area)
{
    const dmlcdtft_config_t *c = &context->config;
    uint32_t x = 0, y = 0, w = c->width, h = c->height;
    if (area != NULL)
    {
        if (area->x >= c->width || area->y >= c->height || area->width == 0 || area->height == 0)
            return 0;                       /* Nothing drawn */
        x = area->x;
        y = area->y;
        w = (area->width > c->width - x) ? c->width - x : area->width;
        h = (area->height > c->height - y) ? c->height - y : area->height;
    }

    uint32_t line_bytes = w * context->bytes_per_pixel;
    uint32_t offset = y * context->stride + x * context->bytes_per_pixel;
    uint8_t *drawn = context->buffers[context->draw_index];
    dmlcdtft_port_sync(c->instance, drawn + offset, (h - 1U) * context->stride + line_bytes);
    if (!c->double_buffer)
        return 0;

    int ret = dmlcdtft_port_set_framebuffer(c->instance, drawn, true);
    if (ret != 0)
        return ret;
    context->draw_index ^= 1U;
    uint8_t *next = context->buffers[context->draw_index];
    for (uint32_t line = 0; line < h; line++)
        memcpy(next + offset + line * context->stride, drawn + offset + line * context->stride, line_bytes);
    return 0;
}

static int set_display_enabled(dmdrvi_context_t context, bool enabled)
{
    int ret = dmlcdtft_port_set_enabled(context->config.instance, enabled);
    if (ret == 0)
    {
        context->display_enabled = enabled;
        panel_pins_apply(context);
    }
    return ret;
}

static bool ioctl_needs_arg(int command)
{
    bool is_gfx = command >= (int)DMDRVI_IOCTL_GFX_GET_INFO && command <= (int)DMDRVI_IOCTL_GFX_GET_BACKLIGHT;
    bool is_own = command >= (int)dmlcdtft_ioctl_cmd_get_status && command < (int)dmlcdtft_ioctl_cmd_max;

    return (is_gfx || is_own) &&
           command != (int)DMDRVI_IOCTL_GFX_SWAP_BUFFERS &&
           command != (int)DMDRVI_IOCTL_GFX_WAIT_VSYNC;
}

/* Commands that change the controller or the panel. */
static int ioctl_control(dmdrvi_context_t context, int command, void *arg)
{
    dmlcdtft_config_t *c = &context->config;
    int ret;

    switch (command)
    {
        case DMDRVI_IOCTL_GFX_SWAP_BUFFERS:
            return swap_buffers(context);
        case DMDRVI_IOCTL_GFX_PRESENT:
            return present(context, (const dmdrvi_gfx_rect_t *)arg);
        case DMDRVI_IOCTL_GFX_WAIT_VSYNC:
            return dmlcdtft_port_wait_vsync(c->instance, (arg != NULL) ? *(const uint32_t *)arg : DMLCDTFT_VSYNC_TIMEOUT_MS);
        case DMDRVI_IOCTL_GFX_FILL_RECT:
            fill_rect(context, (const dmdrvi_gfx_fill_rect_t *)arg);
            return 0;
        case DMDRVI_IOCTL_GFX_SET_DISPLAY_ENABLED:
            return set_display_enabled(context, *(const bool *)arg);
        case DMDRVI_IOCTL_GFX_SET_BACKLIGHT:
            context->backlight_on = *(const bool *)arg;
            panel_pins_apply(context);
            return 0;
        case dmlcdtft_ioctl_cmd_set_background_color:
            ret = dmlcdtft_port_set_background_color(c->instance, *(const uint32_t *)arg & 0x00FFFFFFU);
            if (ret == 0)
                c->background_color = *(const uint32_t *)arg & 0x00FFFFFFU;
            return ret;
        case dmlcdtft_ioctl_cmd_set_alpha:
            ret = dmlcdtft_port_set_alpha(c->instance, *(const uint8_t *)arg);
            if (ret == 0)
                c->alpha = *(const uint8_t *)arg;
            return ret;
        default:
            return -ENOTTY;
    }
}

/* Commands that only report state. */
static int ioctl_query(dmdrvi_context_t context, int command, void *arg)
{
    switch (command)
    {
        case DMDRVI_IOCTL_GFX_GET_INFO:
            get_info(context, (dmdrvi_gfx_info_t *)arg);
            return 0;
        case dmlcdtft_ioctl_cmd_get_status:
            get_status(context, (dmlcdtft_status_t *)arg);
            return 0;
        case DMDRVI_IOCTL_GFX_GET_FRAMEBUFFER:
            *(void **)arg = context->buffers[context->draw_index];
            return 0;
        case DMDRVI_IOCTL_GFX_GET_DISPLAY_ENABLED:
            *(bool *)arg = context->display_enabled;
            return 0;
        case DMDRVI_IOCTL_GFX_GET_BACKLIGHT:
            *(bool *)arg = context->backlight_on;
            return 0;
        case dmlcdtft_ioctl_cmd_get_background_color:
            *(uint32_t *)arg = context->config.background_color;
            return 0;
        case dmlcdtft_ioctl_cmd_get_alpha:
            *(uint8_t *)arg = context->config.alpha;
            return 0;
        default:
            return ioctl_control(context, command, arg);
    }
}

/* ---- DMOD lifecycle ---- */

int dmod_init(const Dmod_Config_t *Config)
{
    DMOD_LOG_INFO("DMLCDTFT interface module initialized\n");
    return 0;
}

int dmod_deinit(void)
{
    DMOD_LOG_INFO("DMLCDTFT interface module deinitialized\n");
    return 0;
}

/* ---- DMDRVI interface ---- */

dmod_dmdrvi_dif_api_declaration(2.0, dmlcdtft, dmdrvi_context_t, _create, ( dmini_context_t config, dmdrvi_dev_num_t* dev_num ))
{
    if (config == NULL || dev_num == NULL)
    {
        DMOD_LOG_ERROR("Invalid parameters to dmlcdtft_dmdrvi_create\n");
        return NULL;
    }

    dmdrvi_context_t context = Dmod_Malloc(sizeof(struct dmdrvi_context));
    if (context == NULL)
        return NULL;

    memset(context, 0, sizeof(*context));
    context->magic = DMLCDTFT_CONTEXT_MAGIC;

    if (read_config_parameters(context, config) != 0 || start_display(context) != 0)
    {
        DMOD_LOG_ERROR("Failed to create DMDRVI context with provided configuration\n");
        context->magic = 0;
        Dmod_Free(context);
        return NULL;
    }

    /* One major number per controller -> /dev/dmlcdtft0 */
    dev_num->flags = DMDRVI_NUM_MAJOR;
    dev_num->major = (dmdrvi_dev_id_t)context->config.instance;
    return context;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmlcdtft, void, _free, ( dmdrvi_context_t context ))
{
    if (!is_valid_context(context))
        return;

    /* Do not touch the panel pins here: dmdevfs tears friends down in reverse
     * order, so their nodes may already be gone (see dmspi's chip select). */
    dmlcdtft_port_deinit(context->config.instance);
    free_framebuffers(context);
    Dmod_Free(context->display_enable_pin.path);
    Dmod_Free(context->backlight_pin.path);
    context->magic = 0;
    Dmod_Free(context);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmlcdtft, void, _friend_changed,
    ( dmdrvi_context_t context, const dmdrvi_friend_info_t* info ))
{
    if (!is_valid_context(context) || info == NULL)
        return;

    panel_pin_t *pin = panel_pin_by_role(context, info->friend_role);
    if (pin == NULL)
        return;

    Dmod_Free(pin->path);
    pin->path = NULL;
    if (info->state != dmdrvi_dev_state_ready || info->node_path == NULL)
        return;

    pin->path = Dmod_StrDup(info->node_path);
    if (pin->path == NULL)
    {
        DMOD_LOG_ERROR("Failed to retain panel control GPIO path\n");
        return;
    }
    panel_pins_apply(context);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmlcdtft, void*, _open, ( dmdrvi_context_t context, int flags, const dmdrvi_dev_num_t* dev_num ))
{
    if (!is_valid_context(context))
    {
        DMOD_LOG_ERROR("Invalid DMDRVI context in dmlcdtft_dmdrvi_open\n");
        return NULL;
    }
    return context;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmlcdtft, void, _close, ( dmdrvi_context_t context, void* handle ))
{
    /* No per-handle state - the display keeps running until the driver is freed. */
}

dmod_dmdrvi_dif_api_declaration(2.0, dmlcdtft, dmdrvi_ssize_t, _read, ( dmdrvi_context_t context, void* handle, void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    if (!is_valid_context(context) || (buffer == NULL && size != 0) || offset < 0)
        return -EINVAL;
    if (size > (size_t)INT64_MAX)
        return -EOVERFLOW;
    if (size == 0 || offset >= (dmdrvi_offset_t)context->framebuffer_size)
        return 0;

    size_t available = context->framebuffer_size - (size_t)offset;
    size_t count = (size < available) ? size : available;
    memcpy(buffer, context->buffers[context->draw_index] + offset, count);
    return (dmdrvi_ssize_t)count;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmlcdtft, dmdrvi_ssize_t, _write, ( dmdrvi_context_t context, void* handle, const void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    if (!is_valid_context(context) || (buffer == NULL && size != 0) || offset < 0)
        return -EINVAL;
    if (size > (size_t)INT64_MAX)
        return -EOVERFLOW;
    if (size == 0)
        return 0;
    if (offset >= (dmdrvi_offset_t)context->framebuffer_size)
        return -ENOSPC;

    size_t available = context->framebuffer_size - (size_t)offset;
    size_t count = (size < available) ? size : available;
    uint8_t *dst = context->buffers[context->draw_index] + offset;
    memcpy(dst, buffer, count);
    dmlcdtft_port_sync(context->config.instance, dst, count);
    return (dmdrvi_ssize_t)count;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmlcdtft, int, _ioctl, ( dmdrvi_context_t context, void* handle, int command, void* arg ))
{
    if (!is_valid_context(context))
    {
        DMOD_LOG_ERROR("Invalid DMDRVI context in dmlcdtft_dmdrvi_ioctl\n");
        return -EINVAL;
    }

    if (arg == NULL && ioctl_needs_arg(command))
    {
        DMOD_LOG_ERROR("Null argument for ioctl command %d\n", command);
        return -EINVAL;
    }

    /* Unknown commands - including the standard block/monitor/network ones
     * dmdevfs probes every node with - are answered quietly with -ENOTTY. */
    return ioctl_query(context, command, arg);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmlcdtft, int, _flush, ( dmdrvi_context_t context, void* handle ))
{
    if (!is_valid_context(context))
    {
        DMOD_LOG_ERROR("Invalid DMDRVI context in dmlcdtft_dmdrvi_flush\n");
        return -EINVAL;
    }

    dmlcdtft_port_sync(context->config.instance, context->buffers[context->draw_index], context->framebuffer_size);
    return 0;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmlcdtft, int, _stat, ( dmdrvi_context_t context, const char* path, dmdrvi_stat_t* stat ))
{
    if (!is_valid_context(context) || stat == NULL)
    {
        DMOD_LOG_ERROR("Invalid parameters in dmlcdtft_dmdrvi_stat\n");
        return -EINVAL;
    }

    stat->size = (dmdrvi_size_t)context->framebuffer_size;
    stat->mode = 0666;
    return 0;
}
