/* DMOD_ENABLE_REGISTRATION is intentionally NOT set here: each family's
 * port.c (which includes dmlcdtft_port.h with the flag on) is the single
 * translation unit that emits this module's API registration table - same
 * split as dmspi/dmgpio. */
#include "dmod.h"
#include "dmosi.h"
#include "dmclk_port.h"
#include "dmlcdtft_port.h"
#include "stm32_common.h"
#include <errno.h>

/**
 * All register-level logic lives here, shared between STM32F4 and STM32F7
 * (identical LTDC block and PLLSAI clock path - see stm32_common.h); each
 * family's src/port/<family>/port.c only adds the lifecycle hooks, the IRQ
 * handler wrappers and data cache maintenance.
 */

/* Every STM32F4/F7 part with an LTDC has exactly one. */
#define STM32_LTDC_INSTANCE_COUNT       1U

/* How long PLLSAI may take to lock. */
#define STM32_PLLSAI_TIMEOUT_MS         100U

/* How long a register reload (framebuffer swap) may take - a few frames. */
#define STM32_LTDC_RELOAD_TIMEOUT_MS    200U

typedef struct
{
    bool                initialized;
    bool                pllsai_started;     /* PLLSAI was off before we needed it */
    uint32_t            pixel_clock_hz;     /* Actually programmed pixel clock */
    uint32_t            gcr;                /* Shadow of LTDC_GCR - written, never read back */
    uint32_t            ier;                /* Shadow of LTDC_IER - only changed from thread context */
    volatile uint32_t   underrun_count;
    dmosi_semaphore_t   event_sem;          /* Posted from the ISR on line/reload events */
} ltdc_state_t;

typedef struct
{
    uint32_t n;
    uint32_t r;
    uint32_t divr_bits;     /* PLLSAIDIVR encoding: /2, /4, /8, /16 */
    uint32_t frequency;
} pllsai_setting_t;

static ltdc_state_t s_ltdc;

static void stop_ltdc(void);

void stm32_ltdc_common_init(void)
{
    s_ltdc.initialized    = false;
    s_ltdc.pllsai_started = false;
    s_ltdc.pixel_clock_hz = 0;
    s_ltdc.underrun_count = 0;
    s_ltdc.event_sem      = NULL;
}

void stm32_ltdc_common_deinit(void)
{
    if (s_ltdc.initialized)
        stop_ltdc();
}

static bool is_valid_instance(dmlcdtft_instance_t instance)
{
    return instance < STM32_LTDC_INSTANCE_COUNT;
}

/* ---- NVIC ---- */

static void nvic_enable_irq(uint32_t irqn)
{
    /* At or below dmosi's minimum priority: the ISR posts a dmosi semaphore. */
    volatile uint8_t  *nvic_ip   = (volatile uint8_t *)0xE000E400UL;
    volatile uint32_t *nvic_iser = (volatile uint32_t *)0xE000E100UL;
    nvic_ip[irqn] = (uint8_t)dmosi_get_min_interrupt_priority();
    nvic_iser[irqn >> 5U] = 1U << (irqn & 0x1FU);
}

static void nvic_disable_irq(uint32_t irqn)
{
    volatile uint32_t *nvic_icer = (volatile uint32_t *)0xE000E180UL;
    nvic_icer[irqn >> 5U] = 1U << (irqn & 0x1FU);
}

/* ---- Pixel clock (PLLSAI) ---- */

static bool wait_for_flag(volatile uint32_t *reg, uint32_t mask, bool set, uint32_t timeout_ms)
{
    uint32_t start = dmosi_get_tick_count();
    while (((*reg & mask) != 0U) != set)
    {
        if ((uint32_t)(dmosi_get_tick_count() - start) >= timeout_ms)
            return false;
    }
    return true;
}

/* PLLSAI shares the main PLL's input divider (PLLM), so its VCO input is the
 * main PLL's VCO input: HSI/M, or - when HSE feeds the PLL, whose frequency
 * is not readable from any register - SYSCLK * P / N while the PLL drives
 * SYSCLK (dmclk knows SYSCLK). */
static uint32_t vco_input_frequency(void)
{
    uint32_t pllcfgr = STM32_RCC->PLLCFGR;
    uint32_t m = pllcfgr & STM32_RCC_PLLCFGR_PLLM_Msk;
    uint32_t n = (pllcfgr & STM32_RCC_PLLCFGR_PLLN_Msk) >> STM32_RCC_PLLCFGR_PLLN_Pos;
    uint32_t p = (((pllcfgr & STM32_RCC_PLLCFGR_PLLP_Msk) >> STM32_RCC_PLLCFGR_PLLP_Pos) + 1U) * 2U;
    uint32_t sws = (STM32_RCC->CFGR & STM32_RCC_CFGR_SWS_Msk) >> STM32_RCC_CFGR_SWS_Pos;

    if (m == 0U || n == 0U)
        return 0;
    if ((pllcfgr & STM32_RCC_PLLCFGR_PLLSRC) == 0U)
        return STM32_HSI_VALUE / m;
    if (sws != STM32_RCC_CFGR_SWS_PLL)
        return 0;

    return (uint32_t)(((uint64_t)dmclk_port_get_current_frequency() * p) / n);
}

static uint32_t distance(uint32_t a, uint32_t b)
{
    return (a > b) ? (a - b) : (b - a);
}

/* Exhaustive search over N, R and DIVR (~9k combinations) for the pixel
 * clock closest to the target, keeping the VCO within its limits. */
static int find_pllsai_setting(uint32_t vco_in, uint32_t target, pllsai_setting_t *best)
{
    best->frequency = 0;
    for (uint32_t n = STM32_PLLSAI_N_MIN; n <= STM32_PLLSAI_N_MAX; n++)
    {
        uint64_t vco = (uint64_t)vco_in * n;
        if (vco < STM32_PLLSAI_VCO_MIN || vco > STM32_PLLSAI_VCO_MAX)
            continue;

        for (uint32_t r = STM32_PLLSAI_R_MIN; r <= STM32_PLLSAI_R_MAX; r++)
        {
            for (uint32_t divr_bits = 0; divr_bits < 4U; divr_bits++)
            {
                uint32_t frequency = (uint32_t)(vco / r / (2U << divr_bits));
                if (best->frequency == 0U || distance(frequency, target) < distance(best->frequency, target))
                {
                    *best = (pllsai_setting_t){ n, r, divr_bits, frequency };
                    if (frequency == target)
                        return 0;
                }
            }
        }
    }
    return (best->frequency != 0U) ? 0 : -ERANGE;
}

static int apply_pllsai_setting(const pllsai_setting_t *s)
{
    volatile stm32_rcc_t *rcc = STM32_RCC;

    s_ltdc.pllsai_started = (rcc->CR & STM32_RCC_CR_PLLSAION) == 0U;
    rcc->CR &= ~STM32_RCC_CR_PLLSAION;
    if (!wait_for_flag(&rcc->CR, STM32_RCC_CR_PLLSAIRDY, false, STM32_PLLSAI_TIMEOUT_MS))
        return -ETIMEDOUT;

    rcc->PLLSAICFGR = (rcc->PLLSAICFGR & ~(STM32_RCC_PLLSAICFGR_N_Msk | STM32_RCC_PLLSAICFGR_R_Msk))
                    | (s->n << STM32_RCC_PLLSAICFGR_N_Pos) | (s->r << STM32_RCC_PLLSAICFGR_R_Pos);
    rcc->DCKCFGR    = (rcc->DCKCFGR & ~STM32_RCC_DCKCFGR_DIVR_Msk) | (s->divr_bits << STM32_RCC_DCKCFGR_DIVR_Pos);

    rcc->CR |= STM32_RCC_CR_PLLSAION;
    if (!wait_for_flag(&rcc->CR, STM32_RCC_CR_PLLSAIRDY, true, STM32_PLLSAI_TIMEOUT_MS))
        return -ETIMEDOUT;
    return 0;
}

static int configure_pixel_clock(uint32_t target)
{
    uint32_t vco_in = vco_input_frequency();
    pllsai_setting_t setting;

    if (vco_in == 0U)
    {
        DMOD_LOG_ERROR("LTDC: cannot determine the PLL input clock (is dmclk configured?)\n");
        return -EIO;
    }
    if (find_pllsai_setting(vco_in, target, &setting) != 0)
    {
        DMOD_LOG_ERROR("LTDC: no PLLSAI setting for a %u Hz pixel clock\n", target);
        return -ERANGE;
    }

    int ret = apply_pllsai_setting(&setting);
    if (ret != 0)
    {
        DMOD_LOG_ERROR("LTDC: PLLSAI did not lock\n");
        return ret;
    }

    s_ltdc.pixel_clock_hz = setting.frequency;
    DMOD_LOG_INFO("LTDC: pixel clock %u Hz (requested %u, PLLSAI N=%u R=%u DIVR=/%u)\n",
                  setting.frequency, target, setting.n, setting.r, 2U << setting.divr_bits);
    return 0;
}

/* ---- LTDC configuration ----
 *
 * GCR and IER are kept in shadows and only ever written: read-modify-write
 * on them would race with the ISR, and emulators do not always read them
 * back (Renode 1.15 returns 0 for GCR while the controller runs). */

static void write_gcr(uint32_t value)
{
    s_ltdc.gcr = value;
    STM32_LTDC->GCR = value;
}

static void write_ier(uint32_t value)
{
    s_ltdc.ier = value;
    STM32_LTDC->IER = value;
}

static void reset_ltdc(void)
{
    STM32_RCC->APB2ENR  |= STM32_RCC_APB2_LTDC;
    (void)STM32_RCC->APB2ENR;
    STM32_RCC->APB2RSTR |= STM32_RCC_APB2_LTDC;
    STM32_RCC->APB2RSTR &= ~STM32_RCC_APB2_LTDC;
}

/* The LTDC takes accumulated values: every register holds the position of
 * the last pixel/line of its area, counted from the start of the sync pulse. */
static void configure_timing(const dmlcdtft_config_t *c)
{
    const dmlcdtft_timing_t *t = &c->timing;
    uint32_t ahbp = (uint32_t)t->hsync_width + t->hback_porch - 1U;
    uint32_t avbp = (uint32_t)t->vsync_width + t->vback_porch - 1U;
    uint32_t aaw  = ahbp + c->width;
    uint32_t aah  = avbp + c->height;
    uint32_t gcr  = 0;

    STM32_LTDC->SSCR = ((uint32_t)(t->hsync_width - 1U) << 16) | (uint32_t)(t->vsync_width - 1U);
    STM32_LTDC->BPCR = (ahbp << 16) | avbp;
    STM32_LTDC->AWCR = (aaw << 16) | aah;
    STM32_LTDC->TWCR = ((aaw + t->hfront_porch) << 16) | (aah + t->vfront_porch);
    STM32_LTDC->LIPCR = aah + 1U;   /* first line of the vertical front porch */

    if (t->hsync_polarity == dmlcdtft_polarity_active_high) gcr |= STM32_LTDC_GCR_HSPOL;
    if (t->vsync_polarity == dmlcdtft_polarity_active_high) gcr |= STM32_LTDC_GCR_VSPOL;
    if (t->de_polarity    == dmlcdtft_polarity_active_high) gcr |= STM32_LTDC_GCR_DEPOL;
    if (t->pclk_inverted)                                   gcr |= STM32_LTDC_GCR_PCPOL;
    write_gcr(gcr);
    STM32_LTDC->BCCR = c->background_color & 0x00FFFFFFU;
}

/* Layer 1 covers the whole active area. Its pixel format values are the
 * LTDC PF[2:0] encoding (see dmdrvi_gfx_pixel_format_t). */
static void configure_layer(const dmlcdtft_config_t *c, const void *framebuffer, uint32_t stride)
{
    volatile stm32_ltdc_layer_t *layer = STM32_LTDC_LAYER1;
    uint32_t ahbp = (uint32_t)c->timing.hsync_width + c->timing.hback_porch - 1U;
    uint32_t avbp = (uint32_t)c->timing.vsync_width + c->timing.vback_porch - 1U;

    layer->WHPCR  = ((ahbp + c->width) << 16) | (ahbp + 1U);
    layer->WVPCR  = ((avbp + c->height) << 16) | (avbp + 1U);
    layer->PFCR   = (uint32_t)c->pixel_format;
    layer->CACR   = c->alpha;
    layer->DCCR   = 0;
    layer->BFCR   = STM32_LTDC_BFCR_PAXCA;
    layer->CFBAR  = (uint32_t)(uintptr_t)framebuffer;
    layer->CFBLR  = (stride << 16) | (stride + 3U);
    layer->CFBLNR = c->height;
    layer->CR     = STM32_LTDC_LxCR_LEN;
}

static uint32_t bytes_per_pixel(dmdrvi_gfx_pixel_format_t format)
{
    return (format == DMDRVI_GFX_PIXEL_FORMAT_ARGB8888) ? 4U
         : (format == DMDRVI_GFX_PIXEL_FORMAT_RGB888)   ? 3U : 2U;
}

static int check_limits(const dmlcdtft_config_t *c)
{
    uint32_t stride = (uint32_t)c->width * bytes_per_pixel(c->pixel_format);

    if (c->pixel_format >= DMDRVI_GFX_PIXEL_FORMAT_COUNT || stride + 3U > STM32_LTDC_CFBLL_MAX ||
        c->height > STM32_LTDC_CFBLNBR_MAX)
    {
        DMOD_LOG_ERROR("LTDC: unsupported framebuffer geometry %ux%u\n", c->width, c->height);
        return -EINVAL;
    }
    return 0;
}

static int start_interrupts(void)
{
    s_ltdc.event_sem = dmosi_semaphore_create(0, 1);
    if (s_ltdc.event_sem == NULL)
        return -ENOMEM;

    STM32_LTDC->ICR = STM32_LTDC_IT_ALL;
    write_ier(STM32_LTDC_IT_FIFO_UNDERRUN | STM32_LTDC_IT_TRANSFER_ERR);
    nvic_enable_irq(STM32_LTDC_IRQN);
    nvic_enable_irq(STM32_LTDC_ER_IRQN);
    return 0;
}

/* Arms a one-shot line/reload interrupt and waits until the ISR posts it.
 * The caller clears the event's flag before whatever triggers it, so an
 * event that already happened by now still fires as soon as it is enabled. */
static int wait_for_event(uint32_t event, uint32_t timeout_ms)
{
    while (dmosi_semaphore_wait(s_ltdc.event_sem, 1, 0) == 0)
    {
        /* drop a stale post */
    }

    write_ier(s_ltdc.ier | event);
    int ret = dmosi_semaphore_wait(s_ltdc.event_sem, 1, (int32_t)timeout_ms);
    write_ier(s_ltdc.ier & ~event);
    return (ret == 0) ? 0 : -ETIMEDOUT;
}

void stm32_ltdc_irq_handler(void)
{
    uint32_t status = STM32_LTDC->ISR & s_ltdc.ier;

    if ((status & (STM32_LTDC_IT_FIFO_UNDERRUN | STM32_LTDC_IT_TRANSFER_ERR)) != 0U)
        s_ltdc.underrun_count++;

    /* The waiting thread disables the event again - clearing the flag is
     * enough to keep it from firing until the next frame. */
    if ((status & (STM32_LTDC_IT_LINE | STM32_LTDC_IT_RELOAD)) != 0U && s_ltdc.event_sem != NULL)
        dmosi_semaphore_post(s_ltdc.event_sem, 1);
    STM32_LTDC->ICR = status;
}

static void stop_ltdc(void)
{
    write_gcr(s_ltdc.gcr & ~STM32_LTDC_GCR_LTDCEN);
    write_ier(0);
    nvic_disable_irq(STM32_LTDC_IRQN);
    nvic_disable_irq(STM32_LTDC_ER_IRQN);
    STM32_RCC->APB2ENR &= ~STM32_RCC_APB2_LTDC;

    if (s_ltdc.pllsai_started)
        STM32_RCC->CR &= ~STM32_RCC_CR_PLLSAION;

    dmosi_semaphore_destroy(s_ltdc.event_sem);
    s_ltdc.event_sem   = NULL;
    s_ltdc.initialized = false;
}

/* ---- Port API ---- */

dmod_dmlcdtft_port_api_declaration(1.0, dmlcdtft_instance_t, _get_instance_count, ( void ))
{
    return (dmlcdtft_instance_t)STM32_LTDC_INSTANCE_COUNT;
}

dmod_dmlcdtft_port_api_declaration(1.0, int, _init, ( dmlcdtft_instance_t instance, const dmlcdtft_config_t* config, const void* framebuffer ))
{
    if (!is_valid_instance(instance) || config == NULL || framebuffer == NULL || s_ltdc.initialized)
        return -EINVAL;

    int ret = check_limits(config);
    if (ret == 0)
        ret = configure_pixel_clock(config->timing.pixel_clock_hz);
    if (ret != 0)
        return ret;

    reset_ltdc();
    configure_timing(config);
    configure_layer(config, framebuffer, (uint32_t)config->width * bytes_per_pixel(config->pixel_format));
    STM32_LTDC->SRCR = STM32_LTDC_SRCR_IMR;

    ret = start_interrupts();
    if (ret != 0)
    {
        STM32_RCC->APB2ENR &= ~STM32_RCC_APB2_LTDC;
        return ret;
    }

    write_gcr(s_ltdc.gcr | STM32_LTDC_GCR_LTDCEN);
    s_ltdc.underrun_count = 0;
    s_ltdc.initialized = true;
    return 0;
}

dmod_dmlcdtft_port_api_declaration(1.0, int, _deinit, ( dmlcdtft_instance_t instance ))
{
    if (!is_valid_instance(instance) || !s_ltdc.initialized)
        return -EINVAL;

    stop_ltdc();
    return 0;
}

dmod_dmlcdtft_port_api_declaration(1.0, int, _set_framebuffer, ( dmlcdtft_instance_t instance, const void* framebuffer, bool wait_vblank ))
{
    if (!is_valid_instance(instance) || !s_ltdc.initialized || framebuffer == NULL)
        return -EINVAL;

    STM32_LTDC_LAYER1->CFBAR = (uint32_t)(uintptr_t)framebuffer;
    if (!wait_vblank)
    {
        STM32_LTDC->SRCR = STM32_LTDC_SRCR_IMR;
        return 0;
    }

    STM32_LTDC->ICR = STM32_LTDC_IT_RELOAD;
    STM32_LTDC->SRCR = STM32_LTDC_SRCR_VBR;
    return wait_for_event(STM32_LTDC_IT_RELOAD, STM32_LTDC_RELOAD_TIMEOUT_MS);
}

dmod_dmlcdtft_port_api_declaration(1.0, int, _set_enabled, ( dmlcdtft_instance_t instance, bool enabled ))
{
    if (!is_valid_instance(instance) || !s_ltdc.initialized)
        return -EINVAL;

    write_gcr(enabled ? (s_ltdc.gcr | STM32_LTDC_GCR_LTDCEN) : (s_ltdc.gcr & ~STM32_LTDC_GCR_LTDCEN));
    return 0;
}

dmod_dmlcdtft_port_api_declaration(1.0, int, _set_background_color, ( dmlcdtft_instance_t instance, uint32_t rgb ))
{
    if (!is_valid_instance(instance) || !s_ltdc.initialized)
        return -EINVAL;

    STM32_LTDC->BCCR = rgb & 0x00FFFFFFU;
    return 0;
}

dmod_dmlcdtft_port_api_declaration(1.0, int, _set_alpha, ( dmlcdtft_instance_t instance, uint8_t alpha ))
{
    if (!is_valid_instance(instance) || !s_ltdc.initialized)
        return -EINVAL;

    STM32_LTDC_LAYER1->CACR = alpha;
    STM32_LTDC->SRCR = STM32_LTDC_SRCR_IMR;
    return 0;
}

dmod_dmlcdtft_port_api_declaration(1.0, int, _wait_vsync, ( dmlcdtft_instance_t instance, uint32_t timeout_ms ))
{
    if (!is_valid_instance(instance) || !s_ltdc.initialized)
        return -EINVAL;
    if ((s_ltdc.gcr & STM32_LTDC_GCR_LTDCEN) == 0U)
        return -EAGAIN;

    STM32_LTDC->ICR = STM32_LTDC_IT_LINE;
    return wait_for_event(STM32_LTDC_IT_LINE, timeout_ms);
}

dmod_dmlcdtft_port_api_declaration(1.0, void, _sync, ( dmlcdtft_instance_t instance, const void* address, size_t size ))
{
    if (is_valid_instance(instance) && address != NULL && size != 0U)
        stm32_ltdc_family_clean_dcache(address, size);
}

dmod_dmlcdtft_port_api_declaration(1.0, uint32_t, _get_pixel_clock, ( dmlcdtft_instance_t instance ))
{
    return (is_valid_instance(instance) && s_ltdc.initialized) ? s_ltdc.pixel_clock_hz : 0U;
}

dmod_dmlcdtft_port_api_declaration(1.0, uint32_t, _get_underrun_count, ( dmlcdtft_instance_t instance ))
{
    return is_valid_instance(instance) ? s_ltdc.underrun_count : 0U;
}
