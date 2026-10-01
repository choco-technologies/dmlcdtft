/**
 * @file port.c
 * @brief dmlcdtft_port implementation for the x86_64 host.
 *
 * There is no display controller on a CI runner, so this port only keeps
 * the state a real controller would have (which buffer is shown, enabled
 * flag, background color, alpha) and reports the requested pixel clock as
 * the programmed one. The framebuffer itself is ordinary memory owned by the
 * core module, so drawing, read() and write() behave exactly like on target.
 * This exists purely so dmlcdtft's dmod_loader-based tests (see ../../tests)
 * can enable the full module - with its declared dmlcdtft_port dependency -
 * on a host that will never see real hardware.
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmlcdtft_port.h"
#include "dmod.h"
#include <errno.h>

typedef struct
{
    bool        initialized;
    bool        enabled;
    const void *framebuffer;
    uint32_t    background_color;
    uint8_t     alpha;
    uint32_t    pixel_clock_hz;
} display_state_t;

static display_state_t g_display;

static bool is_ready(dmlcdtft_instance_t instance)
{
    return instance == 0 && g_display.initialized;
}

int dmod_init(const Dmod_Config_t *Config)
{
    g_display = (display_state_t){ 0 };
    return 0;
}

int dmod_deinit(void)
{
    g_display.initialized = false;
    return 0;
}

dmod_dmlcdtft_port_api_declaration(1.0, dmlcdtft_instance_t, _get_instance_count, ( void ))
{
    return 1;
}

dmod_dmlcdtft_port_api_declaration(1.0, int, _init, ( dmlcdtft_instance_t instance, const dmlcdtft_config_t* config, const void* framebuffer ))
{
    if (instance != 0 || config == NULL || framebuffer == NULL || g_display.initialized)
        return -EINVAL;

    g_display = (display_state_t){
        .initialized      = true,
        .enabled          = true,
        .framebuffer      = framebuffer,
        .background_color = config->background_color,
        .alpha            = config->alpha,
        .pixel_clock_hz   = config->timing.pixel_clock_hz,
    };
    return 0;
}

dmod_dmlcdtft_port_api_declaration(1.0, int, _deinit, ( dmlcdtft_instance_t instance ))
{
    if (!is_ready(instance))
        return -EINVAL;
    g_display.initialized = false;
    return 0;
}

dmod_dmlcdtft_port_api_declaration(1.0, int, _set_framebuffer, ( dmlcdtft_instance_t instance, const void* framebuffer, bool wait_vblank ))
{
    if (!is_ready(instance) || framebuffer == NULL)
        return -EINVAL;
    g_display.framebuffer = framebuffer;
    return 0;
}

dmod_dmlcdtft_port_api_declaration(1.0, int, _set_enabled, ( dmlcdtft_instance_t instance, bool enabled ))
{
    if (!is_ready(instance))
        return -EINVAL;
    g_display.enabled = enabled;
    return 0;
}

dmod_dmlcdtft_port_api_declaration(1.0, int, _set_background_color, ( dmlcdtft_instance_t instance, uint32_t rgb ))
{
    if (!is_ready(instance))
        return -EINVAL;
    g_display.background_color = rgb;
    return 0;
}

dmod_dmlcdtft_port_api_declaration(1.0, int, _set_alpha, ( dmlcdtft_instance_t instance, uint8_t alpha ))
{
    if (!is_ready(instance))
        return -EINVAL;
    g_display.alpha = alpha;
    return 0;
}

dmod_dmlcdtft_port_api_declaration(1.0, int, _wait_vsync, ( dmlcdtft_instance_t instance, uint32_t timeout_ms ))
{
    if (!is_ready(instance))
        return -EINVAL;
    return g_display.enabled ? 0 : -EAGAIN;
}

dmod_dmlcdtft_port_api_declaration(1.0, void, _sync, ( dmlcdtft_instance_t instance, const void* address, size_t size ))
{
    /* Host memory is coherent - nothing to do. */
}

dmod_dmlcdtft_port_api_declaration(1.0, uint32_t, _get_pixel_clock, ( dmlcdtft_instance_t instance ))
{
    return is_ready(instance) ? g_display.pixel_clock_hz : 0U;
}

dmod_dmlcdtft_port_api_declaration(1.0, uint32_t, _get_underrun_count, ( dmlcdtft_instance_t instance ))
{
    return 0;
}
